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

// Per-span callback for user_transform_two: feed one page-span through the
// streaming cipher (keystream + Poly1305 state carry across calls).
static void ish_aead_span(const void *in_host, void *out_host, size_t span, void *ctx) {
    ish_aead_update((struct ish_aead_stream *) ctx,
            (const uint8_t *) in_host, (uint8_t *) out_host, span);
}
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
    // aad is small and bounded; staging it in a per-thread scratch avoids a
    // second direct-pointer walk (the streaming begin consumes it up front).
    static __thread uint8_t scratch_aad[ISH_AEAD_MAX_AAD];
    if (req.aadlen && user_read(req.aad, scratch_aad, req.aadlen)) {
        memset(key, 0, sizeof(key));
        return _EFAULT;
    }

    dword_t ret = 0;
    struct ish_aead_stream stream;
    ish_aead_begin(&stream, key, nonce, scratch_aad, req.aadlen, (int) req.op);
    // The data is transformed IN PLACE over guest memory, no bounce buffer:
    // the streaming cipher runs directly on each page-span (user_transform_two
    // resolves direct host pointers under the mem lock; seal encrypts pt->ct
    // and Poly1305s the ciphertext, open decrypts ct->pt eagerly).
    if (req.inlen &&
            user_transform_two(req.in, req.out, req.inlen, ish_aead_span, &stream)) {
        ret = _EFAULT;
        goto out;
    }

    if (req.op == 0) { // seal
        ish_aead_finish(&stream, NULL, tag);
        if (user_write(req.tag, tag, sizeof(tag))) { ret = _EFAULT; goto out; }
    } else { // open
        if (user_read(req.tag, tag, sizeof(tag))) { ret = _EFAULT; goto out; }
        if (ish_aead_finish(&stream, tag, NULL) != 0) {
            // Authentication failed. open decrypted eagerly into req.out, so
            // scrub the guest buffer before returning -- the guest must never
            // observe unauthenticated plaintext.
            if (req.inlen)
                user_zero(req.out, req.inlen);
            ret = _EBADMSG;
        }
    }

out:
    memset(key, 0, sizeof(key));
    memset(&stream, 0, sizeof(stream)); // holds keystream + poly state
    return ret;
}
