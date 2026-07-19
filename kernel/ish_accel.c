// iSH crypto accelerator: guest-facing syscall glue around the host-native
// ChaCha20-Poly1305 (kernel/ish_accel_crypto.c). A guest issues syscall
// ISH_SYS_AEAD with a pointer to a struct ish_aead_req; the host runs the
// AEAD natively on the guest's buffers instead of the guest emulating its
// own libcrypto. Off by default (doEnableCryptoAccel); enabled only if the
// RFC 8439 self-test passes at startup.
//
// This is the Phase-1 proof interface (a private syscall). The productized
// path is an OpenSSL provider in the guest talking to this, so ssh/scp
// accelerate transparently.

#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/ish_accel_crypto.h"
#include "debug.h"

// Set from ISH_CRYPTO_ACCEL (main.c) or the app preference. Only ever set
// true if ish_accel_crypto_selftest() passed (see ish_accel_init).
bool doEnableCryptoAccel = false;
static bool accel_selftest_ok = false;

void ish_accel_init(void) {
    accel_selftest_ok = ish_accel_crypto_selftest();
    if (!accel_selftest_ok)
        printk("ish-accel: crypto self-test FAILED, accelerator disabled\n");
}

// Guest ABI. Fixed-layout, all 64-bit fields after the two u32s so the
// struct is identical on arm64 and riscv64 guests. op: 0=seal, 1=open.
// alg: 0=chacha20-poly1305. All pointers are guest addresses.
struct ish_aead_req {
    uint32_t op;
    uint32_t alg;
    uint64_t key;    // 32 bytes
    uint64_t nonce;  // 12 bytes
    uint64_t aad;
    uint64_t aadlen;
    uint64_t in;     // plaintext (seal) or ciphertext (open)
    uint64_t inlen;
    uint64_t out;    // ciphertext (seal) or plaintext (open), inlen bytes
    uint64_t tag;    // 16 bytes: written (seal) or read (open)
};

enum { ISH_AEAD_ALG_CHACHA20_POLY1305 = 0 };
// Bound per-call sizes so a bogus request can't drive a huge host malloc.
// SSH records are <= ~256 KiB; anything larger just falls back to the guest.
#define ISH_AEAD_MAX_IN   (4u << 20)   // 4 MiB
#define ISH_AEAD_MAX_AAD  4096u

// Returns 0 on success, or a negative guest errno. _ENOSYS signals "not
// available" so a caller/provider falls back to its own software path.
dword_t sys_ish_aead_guest(guest_addr_t req_addr) {
    if (!doEnableCryptoAccel || !accel_selftest_ok)
        return _ENOSYS;

    struct ish_aead_req req;
    if (user_read(req_addr, &req, sizeof(req)))
        return _EFAULT;
    if (req.alg != ISH_AEAD_ALG_CHACHA20_POLY1305)
        return _EOPNOTSUPP;
    if (req.op > 1)
        return _EINVAL;
    if (req.inlen > ISH_AEAD_MAX_IN || req.aadlen > ISH_AEAD_MAX_AAD)
        return _EMSGSIZE; // too big for the fast path; caller falls back

    uint8_t key[32], nonce[12], tag[16];
    if (user_read(req.key, key, sizeof(key)) || user_read(req.nonce, nonce, sizeof(nonce)))
        return _EFAULT;

    // Reusable per-thread scratch for the common case (SSH records are
    // <= ~32 KiB), so the hot path never mallocs; only oversized requests
    // fall back to a heap allocation. aad is always small (<= ISH_AEAD_MAX_AAD).
    enum { SCRATCH = 64u << 10 };
    static __thread uint8_t scratch_in[SCRATCH], scratch_out[SCRATCH];
    static __thread uint8_t scratch_aad[ISH_AEAD_MAX_AAD];

    dword_t ret = 0;
    uint8_t *aad = NULL, *in = NULL, *out = NULL;
    bool in_heap = false, out_heap = false;
    if (req.aadlen)
        aad = scratch_aad; // aadlen bounded to ISH_AEAD_MAX_AAD above
    if (req.inlen) {
        if (req.inlen <= SCRATCH) { in = scratch_in; out = scratch_out; }
        else {
            in  = malloc(req.inlen); in_heap = true;
            out = malloc(req.inlen); out_heap = true;
            if (!in || !out) { ret = _ENOMEM; goto out; }
        }
    }
    if (req.aadlen && user_read(req.aad, aad, req.aadlen)) { ret = _EFAULT; goto out; }
    if (req.inlen  && user_read(req.in,  in,  req.inlen))  { ret = _EFAULT; goto out; }

    if (req.op == 0) { // seal
        ish_chacha20_poly1305_seal(key, nonce, aad, req.aadlen, in, req.inlen, out, tag);
        if (req.inlen && user_write(req.out, out, req.inlen)) { ret = _EFAULT; goto out; }
        if (user_write(req.tag, tag, sizeof(tag))) { ret = _EFAULT; goto out; }
    } else { // open
        if (user_read(req.tag, tag, sizeof(tag))) { ret = _EFAULT; goto out; }
        if (ish_chacha20_poly1305_open(key, nonce, aad, req.aadlen, in, req.inlen, tag, out) != 0) {
            ret = _EBADMSG; // authentication failed; out left untouched
            goto out;
        }
        if (req.inlen && user_write(req.out, out, req.inlen)) { ret = _EFAULT; goto out; }
    }

out:
    // Wipe key material and plaintext staging (scratch persists across calls,
    // so clearing it matters). Only free the heap fallbacks.
    memset(key, 0, sizeof(key));
    if (out) memset(out, 0, req.inlen);
    if (in && req.inlen <= SCRATCH) memset(in, 0, req.inlen);
    if (out_heap) free(out);
    if (in_heap)  free(in);
    return ret;
}
