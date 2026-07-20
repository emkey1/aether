// High-level emulation (HLE) of hot, well-specified guest libc functions.
//
// When enabled (default OFF; CLI: ISH_HLE=1, app: the HLE toggle), block
// translation for the arm64 and riscv64 guests checks whether the block's
// start address is the entry point of a known libc function build --
// identified by an exact 64-byte prologue fingerprint, hashed against a
// table extracted from the libcs shipped in the bundled rootfs images. On a
// match, the whole block becomes one "giant gadget": a bridge into the C
// implementation below, which performs the function's entire contract
// against guest memory via tlb_read/tlb_write, writes the ABI return
// register, sets the guest pc to the return address, and exits the block.
//
// Correctness posture:
// - The ABI boundary is the contract: only architectural state at function
//   entry/exit has to be right. Caller-saved registers are legally dead, so
//   they are simply not touched (the values they held at entry persist,
//   which is a valid possible execution).
// - An updated/unknown libc simply never matches a fingerprint: execution
//   falls through to ordinary translation. HLE is a pure fast path; plain
//   emulation remains the always-correct fallback.
// - memcpy is implemented with memmove semantics (overlap-safe). For
//   overlapping buffers the guest program's behavior is undefined anyway;
//   a deterministic superset is the conservative choice.
// - memcmp returns the difference of the first differing bytes (exactly
//   musl's behavior; sign-compatible with any conforming caller of glibc's).
// - A fault mid-operation delivers a guest SIGSEGV with pc rewound to the
//   function entry, as if the first instruction had faulted. Partial writes
//   may have happened -- same as real hardware faulting mid-memcpy.
//
// The fingerprint table lives in hle-table.inc: glibc entries come from
// tools/hle_fingerprint_guest.c run inside the guest (dlsym resolves the
// ifunc to the variant actually selected under iSH's AT_HWCAP); musl
// entries from offline extraction of the .so's dynamic symbols (see that
// tool's header comment).

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "jit/gen.h"
#include "kernel/abi.h"
#include "kernel/task.h"
#include "debug.h"
#include "misc.h"

// Set from ISH_HLE (main.c) or the app preference observer (AppDelegate.m).
bool doEnableHLE = false;

enum hle_fn {
    HLE_MEMCPY,   // dst, src, n -> dst
    HLE_MEMMOVE,  // dst, src, n -> dst
    HLE_MEMSET,   // dst, c, n -> dst
    HLE_STRLEN,   // s -> len
    HLE_MEMCMP,   // a, b, n -> sign
    HLE_STRCMP,   // a, b -> sign
    HLE_STRNCMP,  // a, b, n -> sign
    HLE_MEMCHR,   // s, c, n -> ptr or NULL
    HLE_STRCHR,   // s, c -> ptr or NULL (incl. the NUL when c==0)
    HLE_STRCPY,   // dst, src -> dst
    HLE_STPCPY,   // dst, src -> dst + len (end)
    HLE_STRNCPY,  // dst, src, n -> dst (NUL-pad to n)
    HLE_STRCAT,   // dst, src -> dst
    HLE_STRRCHR,  // s, c -> last occurrence or NULL
    HLE_STRNLEN,  // s, n -> min(strlen, n)
    HLE_MEMRCHR,  // s, c, n -> last occurrence in n bytes or NULL
    HLE_STRNCAT,  // dst, src, n -> dst
    HLE_STPNCPY,  // dst, src, n -> dst + strnlen(src, n)
    HLE_RAWMEMCHR,// s, c -> first occurrence (c assumed present)
    HLE_STRSPN,   // s, accept -> len of initial all-in-accept run
    HLE_STRCSPN,  // s, reject -> len of initial none-in-reject run
    HLE_STRPBRK,  // s, accept -> first byte in accept, or NULL
};

#define HLE_PROLOGUE_LEN 64

struct hle_fingerprint {
    enum guest_abi abi;
    enum hle_fn fn;
    const char *name; // symbol + libc build, for tracing
    uint8_t bytes[HLE_PROLOGUE_LEN];
};

static const struct hle_fingerprint hle_table[] = {
#include "hle-table.inc"
};

static bool hle_trace_enabled(void) {
    static int enabled = -1;
    if (enabled == -1)
        enabled = getenv("ISH_HLE_TRACE") != NULL ? 1 : 0;
    return enabled == 1;
}

#if defined(__aarch64__)

// ---- Emission (called from jit.c at block-translation start) ------------

extern void gadget_arm64_hle_call(void);
extern void gadget_riscv64_hle_call(void);

bool hle_try_emit(struct gen_state *state, struct tlb *tlb, guest_addr_t ip,
        bool riscv64) {
    if (hle_trace_enabled()) {
        static _Atomic bool announced = false;
        if (!atomic_exchange(&announced, true))
            fprintf(stderr, "hle: hook reached, enabled=%d riscv64=%d ip=%#llx\n",
                    doEnableHLE, riscv64, (unsigned long long) ip);
    }
    if (!doEnableHLE)
        return false;
    // Skip addresses that cannot be a function entry: arm64 instructions are
    // 4-aligned, but riscv64 with RVC has 2-aligned entries -- musl-riscv64
    // really does place memcpy/strcmp/memchr/strnlen at 2-mod-4 addresses,
    // and an `ip & 3` test here silently excluded them from HLE entirely.
    if (ip & (riscv64 ? 1 : 3))
        return false;
    uint8_t code[HLE_PROLOGUE_LEN];
    if (!tlb_read(tlb, ip, code, sizeof(code)))
        return false;
    enum guest_abi abi = riscv64 ? GUEST_ABI_RISCV64 : GUEST_ABI_ARM64;
    for (size_t i = 0; i < sizeof(hle_table)/sizeof(hle_table[0]); i++) {
        const struct hle_fingerprint *fp = &hle_table[i];
        if (fp->abi != abi)
            continue;
        if (memcmp(fp->bytes, code, HLE_PROLOGUE_LEN) != 0)
            continue;
        if (hle_trace_enabled())
            fprintf(stderr, "hle: attach %s at %#llx (pid %d)\n", fp->name,
                    (unsigned long long) ip, current != NULL ? current->pid : -1);
        // Bit 8 of the fn word tells hle_call which guest ABI's registers to
        // use (cpu_state itself carries no abi field).
        gen_raw(state, (unsigned long) (riscv64
                    ? (void (*)(void)) gadget_riscv64_hle_call
                    : (void (*)(void)) gadget_arm64_hle_call));
        gen_raw(state, (unsigned long) fp->fn | (riscv64 ? 0x100ul : 0));
        gen_raw(state, (unsigned long) ip);
        // Claim the fingerprinted range so gen_end's end_addr covers it and
        // the block invalidates if these bytes are ever rewritten/unmapped.
        if (riscv64)
            state->riscv64_ip = ip + HLE_PROLOGUE_LEN;
        else
            state->arm64_ip = ip + HLE_PROLOGUE_LEN;
        return true;
    }
    // Near-miss candidate discovery (trace mode only): count recurring
    // unmatched block-start prologues so future fingerprints come from data.
    // Tiny open-addressed table keyed by a 64-bit FNV of the prologue; the
    // top recurrers are dumped at powers-of-two totals. Zero cost when
    // ISH_HLE_TRACE is off.
    if (hle_trace_enabled()) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned b = 0; b < HLE_PROLOGUE_LEN; b++)
            h = (h ^ code[b]) * 0x100000001b3ull;
        enum { NM_SLOTS = 4096 };
        static struct { _Atomic uint64_t hash; _Atomic uint64_t count;
                        _Atomic uint64_t ip; } nm[NM_SLOTS];
        static _Atomic uint64_t nm_total;
        unsigned slot = (unsigned) (h % NM_SLOTS);
        for (unsigned probe = 0; probe < 8; probe++, slot = (slot + 1) % NM_SLOTS) {
            uint64_t expect = 0;
            if (atomic_load(&nm[slot].hash) == h ||
                    atomic_compare_exchange_strong(&nm[slot].hash, &expect, h)) {
                atomic_store(&nm[slot].ip, ip);
                atomic_fetch_add(&nm[slot].count, 1);
                break;
            }
        }
        uint64_t total = atomic_fetch_add(&nm_total, 1) + 1;
        if (total >= 4096 && (total & (total - 1)) == 0) {
            // A block translates roughly once (then it's cached), so a given
            // prologue's count is "how many distinct translation events hit
            // this exact function" -- a function used across many processes
            // (each fork/exec re-translates its libc) recurs; a one-off does
            // not. Print the recurring ones (count >= 2), highest first, so
            // the output is a ranked candidate list, not 4096 singletons.
            // Selection sort over the small hot subset, capped at 24 lines.
            fprintf(stderr, "hle: near-miss candidates at %llu unmatched blocks (count ip hash):\n",
                    (unsigned long long) total);
            unsigned lines = 0;
            for (unsigned i2 = 0; i2 < NM_SLOTS && lines < 24; i2++) {
                uint64_t c = atomic_load(&nm[i2].count);
                if (c >= 2) {
                    fprintf(stderr, "hle:   %8llu %#llx %016llx\n",
                            (unsigned long long) c,
                            (unsigned long long) atomic_load(&nm[i2].ip),
                            (unsigned long long) atomic_load(&nm[i2].hash));
                    lines++;
                }
            }
        }
    }
    return false;
}

#else // !__aarch64__

// The arm64/riscv64 guest JITs only exist on aarch64 hosts; elsewhere HLE
// never applies (jit.c still calls this, so keep the symbol).
bool hle_try_emit(struct gen_state *UNUSED(state), struct tlb *UNUSED(tlb),
        guest_addr_t UNUSED(ip), bool UNUSED(riscv64)) {
    return false;
}

#endif // __aarch64__

// ---- Runtime implementations (called from the hle_call gadgets) ---------

// These resolve a DIRECT host pointer to guest memory, one page at a time,
// and run a single native host memcpy/memset/memcmp over each in-page span
// -- the same direct-host-access the JIT's TLB fast path uses, but as one
// fully-vectorized libc call per page instead of a per-16-byte gadget loop.
// (An earlier revision bounced every 256 bytes through a stack buffer via
// tlb_read/tlb_write: 2x memory traffic plus a TLB lookup per chunk, which
// benchmarked ~2x SLOWER than the plain translated glibc memcpy. Direct
// pointers remove both costs.) __tlb_{read,write}_ptr handle TLB misses and
// write-revalidation and return NULL on a genuine fault.
//
// Note: unlike tlb_write, the direct write path does not invoke
// arm64_watch_scan_value (the ISH_ARM64_WATCH_LO16 debug store watchpoint).
// That instrumentation is a debug-only diagnostic, not correctness, so HLE
// stores are simply invisible to it -- acceptable, and HLE is off by default
// anyway.

// The fault path needs to know which guest pc to rewind; threaded through
// from hle_call's fn word rather than sniffing cpu state.
static __thread bool hle_is_riscv64;

static bool hle_fault(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t entry_ip, bool was_write) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = was_write;
    // Rewind to the function entry: the fault is reported as if the first
    // instruction of the function had taken it.
    if (hle_is_riscv64)
        cpu->riscv64_pc = entry_ip;
    else
        cpu->arm64_pc = entry_ip;
    return false;
}

// Bytes from addr to the end of its page.
static inline uint64_t hle_to_page_end(guest_addr_t addr) {
    return PAGE_SIZE - (addr & (PAGE_SIZE - 1));
}

static bool hle_copy(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, guest_addr_t src, uint64_t n) {
    if (dst == src || n == 0)
        return true;
    if (dst < src || src + n <= dst) {
        // Forward: safe when dst is below src or the ranges don't overlap.
        while (n > 0) {
            uint64_t span = n;
            uint64_t sp = hle_to_page_end(src), dp = hle_to_page_end(dst);
            if (span > sp) span = sp;
            if (span > dp) span = dp;
            void *sh = __tlb_read_ptr(tlb, src);
            if (sh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
            void *dh = __tlb_write_ptr(tlb, dst);
            if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
            memcpy(dh, sh, span);
            src += span; dst += span; n -= span;
        }
    } else {
        // Overlapping with dst above src: copy back-to-front, still one
        // native memcpy per (min-aligned) span.
        guest_addr_t s = src + n, d = dst + n;
        while (n > 0) {
            // Span backward is bounded by each address's offset within its
            // page (distance to page START), min 1.
            uint64_t so = (s & (PAGE_SIZE - 1)); if (so == 0) so = PAGE_SIZE;
            uint64_t doff = (d & (PAGE_SIZE - 1)); if (doff == 0) doff = PAGE_SIZE;
            uint64_t span = n;
            if (span > so) span = so;
            if (span > doff) span = doff;
            s -= span; d -= span; n -= span;
            void *sh = __tlb_read_ptr(tlb, s);
            if (sh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
            void *dh = __tlb_write_ptr(tlb, d);
            if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
            memmove(dh, sh, span);
        }
    }
    return true;
}

static bool hle_set(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, uint8_t c, uint64_t n) {
    while (n > 0) {
        uint64_t span = n, dp = hle_to_page_end(dst);
        if (span > dp) span = dp;
        void *dh = __tlb_write_ptr(tlb, dst);
        if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
        memset(dh, c, span);
        dst += span; n -= span;
    }
    return true;
}

static bool hle_strlen(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint64_t *len_out) {
    uint64_t len = 0;
    for (;;) {
        // Scan one page at a time (never touching the next page until the
        // current one has no NUL -- matches what a page-aligned vector strlen
        // is allowed to read, so a string ending just before an unmapped page
        // doesn't spuriously fault).
        uint64_t span = hle_to_page_end(s + len);
        const uint8_t *p = __tlb_read_ptr(tlb, s + len);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *nul = memchr(p, 0, span);
        if (nul != NULL) {
            *len_out = len + (uint64_t) (nul - p);
            return true;
        }
        len += span;
    }
}

static bool hle_cmp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t a, guest_addr_t b, uint64_t n, int64_t *res_out) {
    while (n > 0) {
        uint64_t span = n, ap = hle_to_page_end(a), bp = hle_to_page_end(b);
        if (span > ap) span = ap;
        if (span > bp) span = bp;
        const uint8_t *ah = __tlb_read_ptr(tlb, a);
        if (ah == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *bh = __tlb_read_ptr(tlb, b);
        if (bh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        if (memcmp(ah, bh, span) != 0) {
            for (uint64_t i = 0; i < span; i++) {
                if (ah[i] != bh[i]) {
                    *res_out = (int64_t) ah[i] - (int64_t) bh[i];
                    return true;
                }
            }
        }
        a += span; b += span; n -= span;
    }
    *res_out = 0;
    return true;
}

// strcmp / strncmp: compare byte-by-byte within each page span, stopping at
// the first difference or at a shared NUL. The result is the unsigned-char
// difference of the first differing bytes (musl's exact behavior; POSIX
// only guarantees the sign, which every conforming caller relies on). For
// strncmp, `limit` bounds the comparison; strcmp passes ~0.
static bool hle_strcmp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t a, guest_addr_t b, uint64_t limit, int64_t *res_out) {
    uint64_t done = 0;
    while (done < limit) {
        uint64_t span = limit - done;
        uint64_t ap = hle_to_page_end(a), bp = hle_to_page_end(b);
        if (span > ap) span = ap;
        if (span > bp) span = bp;
        const uint8_t *ah = __tlb_read_ptr(tlb, a);
        if (ah == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *bh = __tlb_read_ptr(tlb, b);
        if (bh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (ah[i] != bh[i]) {
                *res_out = (int64_t) ah[i] - (int64_t) bh[i];
                return true;
            }
            if (ah[i] == 0) { // shared terminator: strings equal up to here
                *res_out = 0;
                return true;
            }
        }
        a += span; b += span; done += span;
    }
    *res_out = 0; // strncmp hit its limit with no difference
    return true;
}

// memchr / strchr: scan for byte `c`. memchr is bounded by `n`; strchr is
// unbounded and also matches the terminating NUL when c==0 (its documented
// behavior). Returns the guest address of the match, or 0 for "not found".
// strchr never reads past the NUL (matches a page-safe vector strchr).
static bool hle_chr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, uint64_t n, bool is_str, guest_addr_t *res_out) {
    uint64_t done = 0;
    for (;;) {
        if (!is_str && done >= n) {
            *res_out = 0;
            return true;
        }
        uint64_t span = hle_to_page_end(s);
        if (!is_str && span > n - done)
            span = n - done;
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (p[i] == c) {
                *res_out = s + i;
                return true;
            }
            if (is_str && p[i] == 0) { // reached end of string without a match
                *res_out = 0;          // (c==0 matched above, so c!=0 here)
                return true;
            }
        }
        s += span; done += span;
    }
}

// strcpy / stpcpy: copy src (including its NUL) to dst. Single pass -- scan
// each src page span for the NUL, copy up to it (or the page/dst-page limit)
// with a native memcpy. stpcpy returns dst+len (the NUL); strcpy returns dst.
// `limit` bounds it for strncpy (~0 for strcpy/stpcpy); `pad` requests the
// strncpy NUL-fill of the tail. Result (end address) is returned via end_out.
static bool hle_stpcpy(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, guest_addr_t src, uint64_t limit, bool pad,
        guest_addr_t *end_out) {
    guest_addr_t d = dst, s = src;
    uint64_t copied = 0;
    for (;;) {
        if (copied == limit) { // strncpy hit n with no NUL: no terminator written
            *end_out = d;
            return true;
        }
        uint64_t span = hle_to_page_end(s);
        uint64_t dspan = hle_to_page_end(d);
        if (span > dspan) span = dspan;
        if (span > limit - copied) span = limit - copied;
        const uint8_t *sh = __tlb_read_ptr(tlb, s);
        if (sh == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        uint8_t *dh = __tlb_write_ptr(tlb, d);
        if (dh == NULL) return hle_fault(cpu, tlb, entry_ip, true);
        const uint8_t *nul = memchr(sh, 0, span);
        uint64_t take = nul != NULL ? (uint64_t) (nul - sh) + 1 : span;
        memcpy(dh, sh, take);
        if (nul != NULL) {
            *end_out = d + (uint64_t) (nul - sh);   // address of the written NUL
            if (pad) {                              // strncpy: zero-fill to limit
                guest_addr_t pd = d + take;
                uint64_t rem = limit - (copied + take);
                if (!hle_set(cpu, tlb, entry_ip, pd, 0, rem))
                    return false;
            }
            return true;
        }
        s += span; d += span; copied += span;
    }
}

static bool hle_strrchr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, guest_addr_t *res_out) {
    guest_addr_t last = 0; bool found = false;
    for (;;) {
        uint64_t span = hle_to_page_end(s);
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (p[i] == c) { last = s + i; found = true; }
            if (p[i] == 0) { // include the NUL as a candidate when c==0
                if (c == 0) { last = s + i; found = true; }
                *res_out = found ? last : 0;
                return true;
            }
        }
        s += span;
    }
}

static bool hle_strnlen(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint64_t maxlen, uint64_t *len_out) {
    uint64_t len = 0;
    while (len < maxlen) {
        uint64_t span = hle_to_page_end(s + len);
        if (span > maxlen - len) span = maxlen - len;
        const uint8_t *p = __tlb_read_ptr(tlb, s + len);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *nul = memchr(p, 0, span);
        if (nul != NULL) { *len_out = len + (uint64_t) (nul - p); return true; }
        len += span;
    }
    *len_out = maxlen;
    return true;
}

// memrchr: last occurrence of c in n bytes. Must examine all n bytes; scan
// each page span with the native memrchr-free approach (walk forward tracking
// the last match -- one native pass per span, cache-friendly).
static bool hle_memrchr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, uint64_t n, guest_addr_t *res_out) {
    guest_addr_t last = 0; bool found = false;
    uint64_t done = 0;
    while (done < n) {
        uint64_t span = hle_to_page_end(s);
        if (span > n - done) span = n - done;
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++)
            if (p[i] == c) { last = s + i; found = true; }
        s += span; done += span;
    }
    *res_out = found ? last : 0;
    return true;
}

// rawmemchr: like memchr but unbounded -- the caller guarantees c is present,
// so the scan finds it before running off into unmapped memory (a fault here
// means the guarantee was violated, i.e. a genuine guest bug -> guest SIGSEGV).
static bool hle_rawmemchr(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint8_t c, guest_addr_t *res_out) {
    for (;;) {
        uint64_t span = hle_to_page_end(s);
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        const uint8_t *hit = memchr(p, c, span);
        if (hit != NULL) { *res_out = s + (uint64_t) (hit - p); return true; }
        s += span;
    }
}

// Read a guest C string into a 256-entry membership set (for strspn/cspn/pbrk).
// Bounded work: stops at the NUL or once all 256 byte values are marked.
static bool hle_build_set(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t set_str, bool set[256]) {
    memset(set, 0, 256 * sizeof(bool));
    guest_addr_t s = set_str;
    unsigned distinct = 0;
    for (;;) {
        uint64_t span = hle_to_page_end(s);
        const uint8_t *p = __tlb_read_ptr(tlb, s);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            if (p[i] == 0)
                return true;
            if (!set[p[i]]) { set[p[i]] = true; if (++distinct == 255) { /* all non-NUL seen */ } }
        }
        s += span;
    }
}

// strspn/strcspn: length of the initial run of s whose bytes are (in_set ?
// members : non-members) of the accept/reject set. strpbrk (via ptr_out !=
// NULL) instead returns the address of the first set member, or 0.
static bool hle_spn(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, guest_addr_t set_str, bool want_in_set,
        uint64_t *len_out, guest_addr_t *ptr_out) {
    bool set[256];
    if (!hle_build_set(cpu, tlb, entry_ip, set_str, set))
        return false;
    guest_addr_t cur = s;
    for (;;) {
        uint64_t span = hle_to_page_end(cur);
        const uint8_t *p = __tlb_read_ptr(tlb, cur);
        if (p == NULL) return hle_fault(cpu, tlb, entry_ip, false);
        for (uint64_t i = 0; i < span; i++) {
            bool member = set[p[i]];
            // strspn stops at first non-member (also at NUL, which is a
            // non-member unless '\0' is in accept -- but strspn's accept never
            // meaningfully contains NUL since build_set stops at it, so NUL
            // always terminates the run). strcspn/strpbrk stop at first member
            // or the NUL.
            bool stop = want_in_set ? !member : member;
            if (p[i] == 0) stop = true;
            if (stop) {
                if (ptr_out != NULL)
                    *ptr_out = (p[i] != 0 && member) ? cur + i : 0; // strpbrk
                if (len_out != NULL)
                    *len_out = (uint64_t) (cur + i - s);
                return true;
            }
        }
        cur += span;
    }
}

// Gadget entry point. Reads args from the guest ABI registers, performs the
// operation, writes the return register, and sets the guest pc to the return
// address. Returns INT_NONE on success (the gadget then exits the block via
// jit_ret) or INT_PF on a guest memory fault (the gadget exits via jit_exit).
int hle_call(struct cpu_state *cpu, struct tlb *tlb, unsigned long fn,
        unsigned long entry_ip) {
    bool rv = (fn & 0x100) != 0;
    fn &= 0xff;
    hle_is_riscv64 = rv;
    qword_t *regs = rv ? cpu->riscv64_regs : cpu->arm64_regs;
    qword_t a0 = regs[rv ? (int) riscv64_a0 : 0];
    qword_t a1 = regs[rv ? (int) riscv64_a1 : 1];
    qword_t a2 = regs[rv ? (int) riscv64_a2 : 2];
    qword_t ret_addr = regs[rv ? (int) riscv64_ra : (int) arm64_x30];
    qword_t result = a0;
    bool ok;

    switch ((enum hle_fn) fn) {
    case HLE_MEMCPY:
    case HLE_MEMMOVE:
        ok = hle_copy(cpu, tlb, entry_ip, a0, a1, a2);
        break;
    case HLE_MEMSET:
        ok = hle_set(cpu, tlb, entry_ip, a0, (uint8_t) a1, a2);
        break;
    case HLE_STRLEN: {
        uint64_t len = 0;
        ok = hle_strlen(cpu, tlb, entry_ip, a0, &len);
        result = len;
        break;
    }
    case HLE_MEMCMP: {
        int64_t res = 0;
        ok = hle_cmp(cpu, tlb, entry_ip, a0, a1, a2, &res);
        result = (qword_t) res;
        break;
    }
    case HLE_STRCMP: {
        int64_t res = 0;
        ok = hle_strcmp(cpu, tlb, entry_ip, a0, a1, ~0ull, &res);
        result = (qword_t) res;
        break;
    }
    case HLE_STRNCMP: {
        int64_t res = 0;
        ok = hle_strcmp(cpu, tlb, entry_ip, a0, a1, a2, &res);
        result = (qword_t) res;
        break;
    }
    case HLE_MEMCHR: {
        guest_addr_t p = 0;
        ok = hle_chr(cpu, tlb, entry_ip, a0, (uint8_t) a1, a2, false, &p);
        result = p;
        break;
    }
    case HLE_STRCHR: {
        guest_addr_t p = 0;
        ok = hle_chr(cpu, tlb, entry_ip, a0, (uint8_t) a1, 0, true, &p);
        result = p;
        break;
    }
    case HLE_STRCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, ~0ull, false, &end);
        result = a0; // strcpy returns dst
        break;
    }
    case HLE_STPCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, ~0ull, false, &end);
        result = end; // stpcpy returns the address of the written NUL
        break;
    }
    case HLE_STRNCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, a2, true, &end);
        result = a0;
        break;
    }
    case HLE_STRCAT: {
        // strcat = strcpy(dst + strlen(dst), src); returns dst.
        uint64_t dlen = 0;
        ok = hle_strlen(cpu, tlb, entry_ip, a0, &dlen);
        if (ok) {
            guest_addr_t end = 0;
            ok = hle_stpcpy(cpu, tlb, entry_ip, a0 + dlen, a1, ~0ull, false, &end);
        }
        result = a0;
        break;
    }
    case HLE_STRRCHR: {
        guest_addr_t p = 0;
        ok = hle_strrchr(cpu, tlb, entry_ip, a0, (uint8_t) a1, &p);
        result = p;
        break;
    }
    case HLE_STRNLEN: {
        uint64_t len = 0;
        ok = hle_strnlen(cpu, tlb, entry_ip, a0, a1, &len);
        result = len;
        break;
    }
    case HLE_MEMRCHR: {
        guest_addr_t p = 0;
        ok = hle_memrchr(cpu, tlb, entry_ip, a0, (uint8_t) a1, a2, &p);
        result = p;
        break;
    }
    case HLE_STRNCAT: {
        // strncat = copy min(strlen(src), n) bytes after dst's NUL, then
        // always terminate. Returns dst.
        uint64_t dlen = 0;
        ok = hle_strlen(cpu, tlb, entry_ip, a0, &dlen);
        if (ok) {
            guest_addr_t end = 0;
            ok = hle_stpcpy(cpu, tlb, entry_ip, a0 + dlen, a1, a2, false, &end);
            if (ok)
                ok = hle_set(cpu, tlb, entry_ip, end, 0, 1); // terminate (no-op if already NUL)
        }
        result = a0;
        break;
    }
    case HLE_STPNCPY: {
        guest_addr_t end = 0;
        ok = hle_stpcpy(cpu, tlb, entry_ip, a0, a1, a2, true, &end);
        result = end; // dst + strnlen(src, n)
        break;
    }
    case HLE_RAWMEMCHR: {
        guest_addr_t p = 0;
        ok = hle_rawmemchr(cpu, tlb, entry_ip, a0, (uint8_t) a1, &p);
        result = p;
        break;
    }
    case HLE_STRSPN: {
        uint64_t len = 0;
        ok = hle_spn(cpu, tlb, entry_ip, a0, a1, true, &len, NULL);
        result = len;
        break;
    }
    case HLE_STRCSPN: {
        uint64_t len = 0;
        ok = hle_spn(cpu, tlb, entry_ip, a0, a1, false, &len, NULL);
        result = len;
        break;
    }
    case HLE_STRPBRK: {
        guest_addr_t p = 0;
        ok = hle_spn(cpu, tlb, entry_ip, a0, a1, false, NULL, &p);
        result = p;
        break;
    }
    default:
        ok = false; // unreachable with a well-formed table
        cpu->segfault_addr = entry_ip;
        cpu->segfault_was_write = false;
        break;
    }

    if (!ok)
        return INT_PF;
    // x0/a0 gets the return value; pc jumps to the caller. riscv64's x0 slot
    // is the hardwired zero -- writing regs[10] (a0) is correct there.
    regs[rv ? riscv64_a0 : 0] = result;
    if (rv)
        cpu->riscv64_pc = ret_addr;
    else
        cpu->arm64_pc = ret_addr;
    return INT_NONE;
}
