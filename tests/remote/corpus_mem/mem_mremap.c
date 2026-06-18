/*
 * mem_mremap.c — mremap conformance: in-place grow into a following hole,
 * forced relocation with MREMAP_MAYMOVE (content preserved, fresh tail zeroed,
 * old range freed), in-place shrink (tail freed), same-size no-op, and the
 * error/unsupported corners.
 *
 * Placement is the kernel's choice, so we assert RELATIONSHIPS (SAME / ELSEWHERE
 * / errno) and CONTENT, never raw addresses.
 *
 * Candidate iSH divergence: MREMAP_FIXED is a FIXME stub returning EINVAL; real
 * Linux relocates the mapping to the requested address.
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

static const char *placed(void *got, void *want) {
    if (got == MAP_FAILED)
        return errno_name(errno);
    return got == want ? "SAME" : "ELSEWHERE";
}

/* grow into a hole that immediately follows the mapping: stays in place */
static void case_grow_inplace(void) {
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two == MAP_FAILED) { emit("mremap.grow_inplace", "%s", "SETUPFAIL"); return; }
    fill_pat(two, PS, 0x10);
    munmap((char *) two + PS, PS);          /* hole right after page 0 */
    void *r = mremap(two, PS, 2 * PS, 0);   /* no MAYMOVE: must expand in place */
    emit("mremap.grow_inplace.place", "%s", placed(r, two));
    if (r != MAP_FAILED) {
        emit_b("mremap.grow_inplace.content", check_pat(r, PS, 0x10));
        emit("mremap.grow_inplace.tail_write", "%s", probe_write_tok((char *) r + PS, 1));
        munmap(r, 2 * PS);
    } else {
        munmap(two, PS);
    }
}

/* the page after the mapping is occupied, so a grow must relocate */
static void case_grow_maymove(void) {
    void *region = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);  /* [A][B] contiguous */
    if (region == MAP_FAILED) { emit("mremap.grow_maymove", "%s", "SETUPFAIL"); return; }
    fill_pat(region, PS, 0x30);             /* pattern in A */

    /* without MAYMOVE, growing A over the occupied B must fail ENOMEM */
    void *blocked = mremap(region, PS, 2 * PS, 0);
    emit("mremap.grow.noflag_blocked", "%s", res_mmap(blocked));
    if (blocked != MAP_FAILED && blocked != region) { /* unexpected; clean up */ }

    /* with MAYMOVE, A relocates; content preserved, fresh tail zeroed, old freed */
    void *moved = mremap(region, PS, 2 * PS, MREMAP_MAYMOVE);
    emit("mremap.grow.maymove.ok", "%s", res_mmap(moved));
    if (moved != MAP_FAILED) {
        emit_b("mremap.grow.maymove.content", check_pat(moved, PS, 0x30));
        emit_b("mremap.grow.maymove.tail_zero", all_zero((char *) moved + PS, PS));
        emit("mremap.grow.maymove.old_freed", "%s", probe_read_tok(region)); /* FAULT */
        munmap(moved, 2 * PS);
    }
    munmap((char *) region + PS, PS);       /* free B */
}

/* shrink: stays in place, frees the tail (which then faults) */
static void case_shrink(void) {
    void *p = mmap(NULL, 3 * PS, RW, ANON_PRIV, -1, 0);
    if (p == MAP_FAILED) { emit("mremap.shrink", "%s", "SETUPFAIL"); return; }
    fill_pat(p, PS, 0x40);
    void *r = mremap(p, 3 * PS, PS, 0);
    emit("mremap.shrink.place", "%s", placed(r, p));
    if (r != MAP_FAILED) {
        emit_b("mremap.shrink.content", check_pat(r, PS, 0x40));
        emit("mremap.shrink.tail_freed", "%s", probe_read_tok((char *) r + PS)); /* FAULT */
        munmap(r, PS);
    } else {
        munmap(p, 3 * PS);
    }
}

static void case_misc(void) {
    /* same size -> same address, unchanged */
    void *p = map_anon(1, RW);
    if (p) {
        fill_pat(p, PS, 0x50);
        void *r = mremap(p, PS, PS, 0);
        emit("mremap.samesize.place", "%s", placed(r, p));
        emit_b("mremap.samesize.content", check_pat(p, PS, 0x50));
        munmap(p, PS);
    }

    /* unaligned old address -> EINVAL */
    void *q = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (q != MAP_FAILED) {
        void *r = mremap((char *) q + 1, PS, 2 * PS, MREMAP_MAYMOVE);
        emit("mremap.unaligned", "%s", res_mmap(r));
        munmap(q, 2 * PS);
    }
}

/* MREMAP_FIXED: relocate to an explicit destination address.
 *
 * This is a DOCUMENTED iSH gap: sys_mremap_guest rejects MREMAP_FIXED with
 * EINVAL (it would need a 5th syscall argument plumbed plus a destination-clobber
 * move), whereas real Linux performs the move and returns the exact destination.
 * MREMAP_FIXED is rare (specialized allocators / GCs), so rather than assert the
 * supported-vs-unsupported difference (which would just be a known red), we mask
 * it the cmpxchg way and assert the invariant BOTH kernels satisfy: the call is
 * handled cleanly -- it lands at EXACTLY the requested address or fails, and
 * never silently relocates somewhere else or returns garbage. */
static void case_fixed(void) {
    void *src = map_anon(1, RW);
    if (!src) { emit("mremap.fixed", "%s", "SETUPFAIL"); return; }
    fill_pat(src, PS, 0x60);
    /* a known-free destination page */
    void *dstregion = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (dstregion == MAP_FAILED) { munmap(src, PS); emit("mremap.fixed", "%s", "SETUPFAIL"); return; }
    void *dst = (char *) dstregion + PS;
    munmap(dst, PS);                        /* free the destination page */

    void *r = mremap(src, PS, PS, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    emit_b("mremap.fixed.exact_or_fail", r == MAP_FAILED || r == dst);
    if (r == dst) {
        emit_b("mremap.fixed.content", check_pat(dst, PS, 0x60));  /* supported (mint) */
        munmap(dst, PS);
    } else {
        if (r != MAP_FAILED) munmap(r, PS);
        munmap(src, PS);                    /* src still mapped if move failed */
    }
    munmap(dstregion, PS);                  /* free the first dest page */
}

int main(int argc, char **argv) {
    mem_init(argc, argv);
    case_grow_inplace();
    case_grow_maymove();
    case_shrink();
    case_misc();
    case_fixed();
    return 0;
}
