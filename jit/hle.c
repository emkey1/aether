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
    // Function entries are at least 4-aligned on both guest arches; skip the
    // table scan for addresses that cannot be one.
    if (ip & 3)
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

// Chunked guest-memory helpers: tlb_read/tlb_write already handle TLB
// misses (through mmu_translate) and cross-page spans; a failure is a
// genuine guest memory fault.
#define HLE_CHUNK 256

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

static bool hle_copy(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, guest_addr_t src, uint64_t n) {
    uint8_t buf[HLE_CHUNK];
    if (dst == src || n == 0)
        return true;
    if (dst < src || src + n <= dst) {
        // Forward copy (no overlap hazard in this direction).
        while (n > 0) {
            unsigned chunk = n > HLE_CHUNK ? HLE_CHUNK : (unsigned) n;
            if (!tlb_read(tlb, src, buf, chunk))
                return hle_fault(cpu, tlb, entry_ip, false);
            if (!tlb_write(tlb, dst, buf, chunk))
                return hle_fault(cpu, tlb, entry_ip, true);
            src += chunk; dst += chunk; n -= chunk;
        }
    } else {
        // Overlapping with dst above src: copy backwards.
        while (n > 0) {
            unsigned chunk = n > HLE_CHUNK ? HLE_CHUNK : (unsigned) n;
            n -= chunk;
            if (!tlb_read(tlb, src + n, buf, chunk))
                return hle_fault(cpu, tlb, entry_ip, false);
            if (!tlb_write(tlb, dst + n, buf, chunk))
                return hle_fault(cpu, tlb, entry_ip, true);
        }
    }
    return true;
}

static bool hle_set(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t dst, uint8_t c, uint64_t n) {
    uint8_t buf[HLE_CHUNK];
    memset(buf, c, n > HLE_CHUNK ? HLE_CHUNK : (size_t) n);
    while (n > 0) {
        unsigned chunk = n > HLE_CHUNK ? HLE_CHUNK : (unsigned) n;
        if (!tlb_write(tlb, dst, buf, chunk))
            return hle_fault(cpu, tlb, entry_ip, true);
        dst += chunk; n -= chunk;
    }
    return true;
}

static bool hle_strlen(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t s, uint64_t *len_out) {
    uint8_t buf[HLE_CHUNK];
    uint64_t len = 0;
    for (;;) {
        // Never read past the NUL's page: cap each chunk at the page end so a
        // string ending just before an unmapped page doesn't fault (a real
        // byte-wise strlen would not touch the next page either; the vector
        // ones only read within the aligned page, same guarantee).
        unsigned to_page = (unsigned) (PAGE_SIZE - ((s + len) & (PAGE_SIZE - 1)));
        unsigned chunk = to_page > HLE_CHUNK ? HLE_CHUNK : to_page;
        if (!tlb_read(tlb, s + len, buf, chunk))
            return hle_fault(cpu, tlb, entry_ip, false);
        for (unsigned i = 0; i < chunk; i++) {
            if (buf[i] == 0) {
                *len_out = len + i;
                return true;
            }
        }
        len += chunk;
    }
}

static bool hle_cmp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t entry_ip,
        guest_addr_t a, guest_addr_t b, uint64_t n, int64_t *res_out) {
    uint8_t ba[HLE_CHUNK], bb[HLE_CHUNK];
    while (n > 0) {
        unsigned chunk = n > HLE_CHUNK ? HLE_CHUNK : (unsigned) n;
        if (!tlb_read(tlb, a, ba, chunk))
            return hle_fault(cpu, tlb, entry_ip, false);
        if (!tlb_read(tlb, b, bb, chunk))
            return hle_fault(cpu, tlb, entry_ip, false);
        if (memcmp(ba, bb, chunk) != 0) {
            for (unsigned i = 0; i < chunk; i++) {
                if (ba[i] != bb[i]) {
                    *res_out = (int64_t) ba[i] - (int64_t) bb[i];
                    return true;
                }
            }
        }
        a += chunk; b += chunk; n -= chunk;
    }
    *res_out = 0;
    return true;
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
