#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "kernel/signal.h"
#include "kernel/task.h"

// AdvSIMD LD1/ST1 (load/store multiple single-element structures,
// contiguous form): transfer `count` consecutive V registers (wrapping
// mod 32) of `regbytes` (8 for the .8b/.4h/.2s arrangements, 16 for the
// .16b/... Q=1 arrangements) each, starting at `addr`. Done in C because
// the whole transfer can span page boundaries per register — reusing the
// crosspage-capable tlb_read/tlb_write is far simpler and safer than
// hand-rolling the multi-register crosspage assembly. Returns INT_NONE on
// success, or INT_PF (with cpu->segfault_addr/was_write set from the tlb)
// on the first faulting access; the calling gadget rewinds PC and exits.
// The scalar-write zero-extension (Q=0 clears the upper 64 bits) falls out
// of copying through a zero-initialized union.
//
// Success returns 0, NOT INT_NONE (which is -1) — the gadget branches to
// its fault path on a nonzero result, so INT_NONE would take the fault
// path on every success. (Real bug: it did exactly that — every
// successful ld1 exited INT_PF and the block re-ran forever.)
int arm64_vldst_multi(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                      unsigned rt, unsigned count, unsigned regbytes, int is_load) {
    for (unsigned r = 0; r < count; r++) {
        unsigned v = (rt + r) & 31;
        if (is_load) {
            union xmm_reg tmp = {};
            if (!tlb_read(tlb, addr, &tmp, regbytes)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return INT_PF;
            }
            cpu->arm64_v[v] = tmp;
        } else {
            if (!tlb_write(tlb, addr, &cpu->arm64_v[v], regbytes)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return INT_PF;
            }
        }
        addr += regbytes;
    }
    return 0;
}

// AdvSIMD structured load/store forms beyond the contiguous LD1/ST1
// above (same crosspage-safety reasoning; OpenMinis splits these across
// per-element micro-gadgets instead). spec packs the shape:
//   [3:0] count  [5:4] esize_log2  [6] q  [11:8] lane
//   [13:12] kind (1=interleaved multiple, 2=single lane, 3=replicate)
//   [14] is_load
// Returns 0 on success (see the INT_NONE note above), INT_PF on fault.
int arm64_vldst_struct(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                       unsigned rt, unsigned spec) {
    unsigned count = spec & 0xf;
    unsigned esize = 1u << ((spec >> 4) & 3);
    unsigned q = (spec >> 6) & 1;
    unsigned lane = (spec >> 8) & 0xf;
    unsigned kind = (spec >> 12) & 3;
    int is_load = (spec >> 14) & 1;
    unsigned regbytes = q ? 16 : 8;

    if (kind == 1) {
        // LD2/LD3/LD4 (multiple structures): de-interleave count registers'
        // worth of elements; ST2-4 interleave. Whole transfer buffered so a
        // fault mid-way never leaves half-updated guest registers.
        unsigned lanes = regbytes / esize;
        unsigned total = count * regbytes;
        uint8_t buf[64];
        if (is_load) {
            if (!tlb_read(tlb, addr, buf, total)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return INT_PF;
            }
            for (unsigned r = 0; r < count; r++) {
                union xmm_reg tmp = {};
                for (unsigned e = 0; e < lanes; e++)
                    memcpy(&tmp.u8[e * esize], &buf[(e * count + r) * esize], esize);
                cpu->arm64_v[(rt + r) & 31] = tmp;
            }
        } else {
            for (unsigned r = 0; r < count; r++)
                for (unsigned e = 0; e < lanes; e++)
                    memcpy(&buf[(e * count + r) * esize],
                           &cpu->arm64_v[(rt + r) & 31].u8[e * esize], esize);
            if (!tlb_write(tlb, addr, buf, total)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return INT_PF;
            }
        }
        return 0;
    }

    if (kind == 2) {
        // LD1-4/ST1-4 (single structure): one element per register at a
        // fixed lane; loads leave the register's other lanes intact.
        for (unsigned r = 0; r < count; r++) {
            unsigned v = (rt + r) & 31;
            if (is_load) {
                uint8_t tmp[8];
                if (!tlb_read(tlb, addr, tmp, esize)) {
                    cpu->segfault_addr = tlb->segfault_addr;
                    cpu->segfault_was_write = false;
                    return INT_PF;
                }
                memcpy(&cpu->arm64_v[v].u8[lane * esize], tmp, esize);
            } else {
                if (!tlb_write(tlb, addr, &cpu->arm64_v[v].u8[lane * esize], esize)) {
                    cpu->segfault_addr = tlb->segfault_addr;
                    cpu->segfault_was_write = true;
                    return INT_PF;
                }
            }
            addr += esize;
        }
        return 0;
    }

    // kind == 3: LD1R-LD4R — load one element per register and replicate
    // it across the register's arrangement (upper 64 bits zero if Q=0).
    for (unsigned r = 0; r < count; r++) {
        unsigned v = (rt + r) & 31;
        uint8_t tmp[8];
        if (!tlb_read(tlb, addr, tmp, esize)) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return INT_PF;
        }
        union xmm_reg rep = {};
        for (unsigned e = 0; e < regbytes / esize; e++)
            memcpy(&rep.u8[e * esize], tmp, esize);
        cpu->arm64_v[v] = rep;
        addr += esize;
    }
    return 0;
}

// LSE atomic read-modify-write (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/
// LDUMAX/LDUMIN and SWP). GENUINELY host-atomic: it resolves the guest
// address to its backing host pointer and runs a real host __atomic RMW
// there, so concurrent guest threads (each on its own host pthread) don't
// lose updates. LSE requires natural alignment, so a valid access never
// crosses a page — one host pointer suffices. size_bytes is 1/2/4/8;
// op 0-7 are the arithmetic forms, op 8 is SWP. Returns 0 / INT_PF.
//
// The min/max variants and the sub-64-bit widths are done with a
// compare-exchange loop (there's no direct __atomic_fetch_max, and byte/
// half atomics still lower to LL/SC on the host anyway), which is itself
// lock-free and race-free.
int arm64_lse_rmw(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                  unsigned size_bytes, unsigned op, uint64_t operand,
                  uint64_t *old_out) {
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_PF;
    }
    unsigned bits = size_bytes * 8;
    uint64_t smask = bits < 64 ? (1ull << (bits - 1)) : 0;

#define LSE_RMW_AT(TYPE) do {                                             \
        TYPE *p = ptr;                                                    \
        TYPE arg = (TYPE) operand;                                        \
        TYPE old = __atomic_load_n(p, __ATOMIC_RELAXED), neu;            \
        do {                                                             \
            switch (op) {                                                \
                case 0: neu = (TYPE) (old + arg); break;   /* LDADD */   \
                case 1: neu = (TYPE) (old & ~arg); break;  /* LDCLR */   \
                case 2: neu = (TYPE) (old ^ arg); break;   /* LDEOR */   \
                case 3: neu = (TYPE) (old | arg); break;   /* LDSET */   \
                case 4: neu = (int64_t) ((old ^ smask) - smask) >         \
                              (int64_t) ((arg ^ smask) - smask)           \
                              ? old : arg; break;          /* LDSMAX */  \
                case 5: neu = (int64_t) ((old ^ smask) - smask) <         \
                              (int64_t) ((arg ^ smask) - smask)           \
                              ? old : arg; break;          /* LDSMIN */  \
                case 6: neu = old > arg ? old : arg; break; /* LDUMAX */ \
                case 7: neu = old < arg ? old : arg; break; /* LDUMIN */ \
                default: neu = arg; break;                 /* SWP */     \
            }                                                            \
        } while (!__atomic_compare_exchange_n(p, &old, neu, true,        \
                     __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));               \
        *old_out = (uint64_t) old;                                       \
    } while (0)

    switch (size_bytes) {
        case 1: LSE_RMW_AT(uint8_t); break;
        case 2: LSE_RMW_AT(uint16_t); break;
        case 4: LSE_RMW_AT(uint32_t); break;
        default: LSE_RMW_AT(uint64_t); break;
    }
#undef LSE_RMW_AT
    return 0;
}

// Host-atomic compare-and-swap for the LSE CAS gadget and the STXR
// store-conditional. Compares memory at size_bytes against `expected`; if
// equal, atomically stores `desired` and sets *swapped=1, else leaves it
// and sets *swapped=0. Always returns the observed old value (zero-
// extended) in *old_out. Genuinely atomic against concurrent guest
// threads — replaces the old load-compare-store, which had an ABA race
// (two threads could both pass the compare and both store). Returns
// 0 / INT_PF.
int arm64_cas(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
              unsigned size_bytes, uint64_t expected, uint64_t desired,
              uint64_t *old_out, uint32_t *swapped) {
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_PF;
    }
#define LSE_CAS_AT(TYPE) do {                                            \
        TYPE exp = (TYPE) expected;                                      \
        bool ok = __atomic_compare_exchange_n((TYPE *) ptr, &exp,        \
                     (TYPE) desired, false, __ATOMIC_SEQ_CST,            \
                     __ATOMIC_SEQ_CST);                                  \
        *swapped = ok ? 1 : 0;                                           \
        *old_out = (uint64_t) exp; /* CAS writes the observed value on fail */ \
    } while (0)
    switch (size_bytes) {
        case 1: LSE_CAS_AT(uint8_t); break;
        case 2: LSE_CAS_AT(uint16_t); break;
        case 4: LSE_CAS_AT(uint32_t); break;
        default: LSE_CAS_AT(uint64_t); break;
    }
#undef LSE_CAS_AT
    return 0;
}

// Soft fallbacks for arm64-guest gadgets whose native instructions are
// optional host extensions: FEAT_SHA512 arrived with A13 and FEAT_CRC32
// with A10, but we support devices back to A7-class hardware. gen.c
// probes the host and emits the sha512_soft/crc32_soft gadgets (crypto.S,
// dpextra.S) on older chips, so AT_HWCAP and ID_AA64ISAR0 can advertise
// the same feature set on every device. Formulas verified bit-exact
// against the native instructions on an M-series host (random vectors).

static inline uint64_t ror64(uint64_t x, unsigned n) {
    return x >> n | x << (64 - n);
}
static inline uint64_t sha512_cho(uint64_t x, uint64_t y, uint64_t z) {
    return (x & (y ^ z)) ^ z;
}
static inline uint64_t sha512_maj(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) | ((x | y) & z);
}

// packed = rd | rn<<8 | rm<<16 | op<<24; op: 0=H 1=H2 2=SU1 3=SU0.
void arm64_sha512_soft(struct cpu_state *cpu, uint64_t packed) {
    uint64_t *d = cpu->arm64_v[packed & 0x1f].qw;
    // Snapshot operands: rd may alias rn/rm, and the instruction reads
    // everything before writing.
    uint64_t d0 = d[0], d1 = d[1];
    uint64_t n0 = cpu->arm64_v[(packed >> 8) & 0x1f].qw[0];
    uint64_t n1 = cpu->arm64_v[(packed >> 8) & 0x1f].qw[1];
    uint64_t m0 = cpu->arm64_v[(packed >> 16) & 0x1f].qw[0];
    uint64_t m1 = cpu->arm64_v[(packed >> 16) & 0x1f].qw[1];
    switch ((packed >> 24) & 3) {
        case 0: { // SHA512H
            d1 += (ror64(m1, 14) ^ ror64(m1, 18) ^ ror64(m1, 41))
                + sha512_cho(m1, n0, n1);
            uint64_t t = d1 + m0;
            d0 += (ror64(t, 14) ^ ror64(t, 18) ^ ror64(t, 41))
                + sha512_cho(t, m1, n0);
            break;
        }
        case 1: // SHA512H2
            d1 += (ror64(m0, 28) ^ ror64(m0, 34) ^ ror64(m0, 39))
                + sha512_maj(m0, m1, n0);
            d0 += (ror64(d1, 28) ^ ror64(d1, 34) ^ ror64(d1, 39))
                + sha512_maj(d1, m0, m1);
            break;
        case 2: // SHA512SU1
            d0 += (ror64(n0, 19) ^ ror64(n0, 61) ^ (n0 >> 6)) + m0;
            d1 += (ror64(n1, 19) ^ ror64(n1, 61) ^ (n1 >> 6)) + m1;
            break;
        case 3: // SHA512SU0 (two-reg; rm unused)
            d0 += ror64(d1, 1) ^ ror64(d1, 8) ^ (d1 >> 7);
            d1 += ror64(n0, 1) ^ ror64(n0, 8) ^ (n0 >> 7);
            break;
    }
    d[0] = d0;
    d[1] = d1;
}

uint32_t arm64_crc32_soft(uint32_t acc, uint64_t val, uint64_t size_log, uint64_t is_c) {
    uint32_t poly = is_c ? 0x82F63B78u : 0xEDB88320u; // reflected CRC32C / CRC32
    for (unsigned i = 0; i < (1u << size_log); i++) {
        acc ^= (uint8_t) (val >> (8 * i));
        for (int bit = 0; bit < 8; bit++)
            acc = (acc >> 1) ^ (poly & -(acc & 1));
    }
    return acc;
}

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu &&
            tlb->mem_changes == atomic_load_explicit(&mmu->changes, memory_order_relaxed)) {
        return;
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = atomic_load_explicit(&mmu->changes, memory_order_relaxed);
    tlb_flush(tlb);
}

void tlb_flush(struct tlb *tlb) {
    tlb->mem_changes = atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed);
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
}

void tlb_free(struct tlb *tlb) {
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, guest_addr_t addr, char *value, unsigned size) {
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, guest_addr_t addr, const char *value, unsigned size) {
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, guest_addr_t addr, int type) {
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed) != tlb->mem_changes)
        tlb_flush(tlb);
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }
    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    if (type == MEM_WRITE)
        tlb_ent->page_if_writable = tlb_ent->page;
    else
        // 1 is not a valid page so this won't look like a hit
        tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);
    return (void *) (tlb_ent->data_minus_addr + addr);
}

__no_instrument void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr) {
    return __tlb_write_ptr(tlb, addr);
}
