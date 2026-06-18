/*
 * mem_brk.c — brk/sbrk conformance: the break starts at a sane nonzero value,
 * grows by exactly the requested amount, the freshly-exposed heap reads back
 * zero and is writable, and shrinking restores the break. These are the
 * invariants the amd64 PIE-placement fix (project_amd64_brk_pie_placement)
 * unblocked: dynamic PIEs used to start with the break pinned at the mmap
 * ceiling, capping growth at ~32 MiB.
 *
 * All comparisons are relationships between values we read back (old vs new
 * break, "advanced by inc", "reads zero"), never a raw break address.
 */
#include "mem_common.h"

int main(int argc, char **argv) {
    mem_init(argc, argv);

    void *base = sbrk(0);
    emit_b("brk.initial_valid", base != (void *) -1 && base != NULL);
    if (base == (void *) -1) return 0;

    /* grow by 8 pages: sbrk returns the OLD break, the break advances by exactly
     * the requested increment. */
    size_t inc = 8 * (size_t) PS;
    void *old = sbrk((intptr_t) inc);
    emit_b("brk.grow.ret_is_old_break", old == base);
    if (old == (void *) -1) { emit("brk.grow", "%s", "GROWFAIL"); return 0; }

    void *grown = sbrk(0);
    emit_b("brk.grow.advanced_by_inc", grown == (char *) old + inc);

    /* the newly-exposed heap [old, old+inc) reads back zero and is writable */
    emit_b("brk.grow.zero", all_zero(old, inc));
    emit("brk.grow.writable", "%s", probe_write_tok(old, 0x5a));
    fill_pat(old, inc, 0x77);
    emit_b("brk.grow.readback", check_pat(old, inc, 0x77));

    /* shrink back: the break returns to where it started */
    void *ret = sbrk(-(intptr_t) inc);
    emit_b("brk.shrink.ret_is_grown_break", ret == grown);
    emit_b("brk.shrink.restored", sbrk(0) == base);

    return 0;
}
