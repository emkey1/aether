/*
 * mem_madvise.c — madvise conformance: MADV_DONTNEED destructively zeroes private
 * anonymous pages (the behavior jemalloc depends on; landed in 41cadcda),
 * MADV_FREE is accepted, benign hint advices are no-ops, and the validation
 * corners (invalid advice, unaligned addr, unmapped range).
 *
 * MADV_FREE content is UNSPECIFIED after the call (the page may keep its old
 * bytes until reused, or read back zero), so we assert only its return value,
 * never its contents — the cmpxchg "don't compare the unspecified" discipline.
 *
 * Candidate iSH divergences: madvise ignored the advice argument (any advice but
 * DONTNEED returned 0) so an INVALID advice was accepted, an UNALIGNED addr was
 * accepted for non-DONTNEED advices, and DONTNEED over an unmapped gap returned
 * 0 instead of ENOMEM.
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

static void case_dontneed(void) {
    void *p = map_anon(2, RW);
    if (!p) { emit("madvise.dontneed", "%s", "SETUPFAIL"); return; }
    fill_pat(p, 2 * PS, 0xAA);
    emit("madvise.dontneed.ret", "%s", res_errno(madvise(p, 2 * PS, MADV_DONTNEED)));
    emit_b("madvise.dontneed.zeroed", all_zero(p, 2 * PS));   /* destructive zero */
    /* the mapping is still present and writable afterwards */
    emit("madvise.dontneed.writable", "%s", probe_write_tok(p, 0x5a));
    munmap(p, 2 * PS);
}

static void case_free(void) {
    void *p = map_anon(1, RW);
    if (!p) { emit("madvise.free", "%s", "SETUPFAIL"); return; }
    fill_pat(p, PS, 0xBB);
    emit("madvise.free.ret", "%s", res_errno(madvise(p, PS, MADV_FREE)));
    /* contents are intentionally NOT asserted (unspecified after MADV_FREE) */
    emit("madvise.free.writable", "%s", probe_write_tok(p, 0x5a));
    munmap(p, PS);
}

static void case_noop_hints(void) {
    void *p = map_anon(1, RW);
    if (!p) { emit("madvise.hints", "%s", "SETUPFAIL"); return; }
    /* benign advices Linux accepts as no-ops on a normal anon mapping */
    int advices[] = { 0 /*NORMAL*/, 1 /*RANDOM*/, 2 /*SEQUENTIAL*/, 3 /*WILLNEED*/ };
    const char *names[] = { "normal", "random", "sequential", "willneed" };
    for (int i = 0; i < 4; i++) {
        char key[48];
        snprintf(key, sizeof key, "madvise.hint.%s", names[i]);
        emit(key, "%s", res_errno(madvise(p, PS, advices[i])));
    }
    munmap(p, PS);
}

static void case_validation(void) {
    void *p = map_anon(1, RW);
    if (!p) { emit("madvise.validation", "%s", "SETUPFAIL"); return; }

    /* an advice value no kernel defines must be EINVAL */
    emit("madvise.invalid_advice", "%s", res_errno(madvise(p, PS, 0x7ffe)));
    /* an unaligned address must be EINVAL regardless of advice */
    emit("madvise.unaligned.willneed", "%s",
         res_errno(madvise((char *) p + 1, PS, MADV_WILLNEED)));
    emit("madvise.unaligned.dontneed", "%s",
         res_errno(madvise((char *) p + 1, PS, MADV_DONTNEED)));
    munmap(p, PS);

    /* DONTNEED over a fully-unmapped range -> ENOMEM */
    void *q = map_anon(1, RW);
    if (q) {
        munmap(q, PS);
        emit("madvise.dontneed.unmapped", "%s", res_errno(madvise(q, PS, MADV_DONTNEED)));
    }

    /* DONTNEED over a range straddling mapped + unmapped pages -> ENOMEM */
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two != MAP_FAILED) {
        munmap((char *) two + PS, PS);
        emit("madvise.dontneed.partial", "%s", res_errno(madvise(two, 2 * PS, MADV_DONTNEED)));
        munmap(two, PS);
    }
}

int main(int argc, char **argv) {
    mem_init(argc, argv);
    case_dontneed();
    case_free();
    case_noop_hints();
    case_validation();
    return 0;
}
