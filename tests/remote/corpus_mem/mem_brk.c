/*
 * mem_brk.c — brk conformance: the break starts at a sane nonzero value,
 * grows by exactly the requested amount, the freshly-exposed heap reads back
 * zero and is writable, and shrinking restores the break. These are the
 * invariants the amd64 PIE-placement fix (project_amd64_brk_pie_placement)
 * unblocked: dynamic PIEs used to start with the break pinned at the mmap
 * ceiling, capping growth at ~32 MiB.
 *
 * Uses the raw brk syscall, never sbrk(). musl's sbrk() returns ENOMEM for
 * ANY nonzero increment by design -- its malloc doesn't use the brk heap, so
 * growing it behind malloc's back is unsafe (issue #486). The conductor
 * builds this corpus as a static musl ELF, so the sbrk() version this
 * replaces failed identically on iSH and on the mint oracle, compared equal,
 * and reported pass while never exercising anything past the first grow.
 *
 * Linux's brk(2) SYSCALL returns the resulting break -- the new one when the
 * request is honored, the unchanged current one when it is refused. It never
 * reports an errno, so "did it work" is a comparison against the address we
 * asked for. (That is the raw kernel ABI; glibc's brk() wrapper returns 0/-1
 * instead, which is why this goes through syscall() directly on every libc.)
 *
 * All comparisons are relationships between values we read back (old vs new
 * break, "advanced by inc", "reads zero"), never a raw break address.
 */
#include "mem_common.h"

static void *brk_call(void *addr) {
    return (void *) syscall(SYS_brk, addr);
}

static void *brk_current(void) {
    return brk_call(NULL);
}

int main(int argc, char **argv) {
    mem_init(argc, argv);

    void *base = brk_current();
    emit_b("brk.initial_valid", base != (void *) -1 && base != NULL);
    if (base == (void *) -1 || base == NULL) return 0;

    /* grow by 8 pages: the syscall returns the new break, which must be
     * exactly the address asked for. */
    size_t inc = 8 * (size_t) PS;
    void *want = (char *) base + inc;
    void *grown = brk_call(want);
    emit_b("brk.grow.ret_is_new_break", grown == want);
    if (grown != want) { emit("brk.grow", "%s", "GROWFAIL"); return 0; }

    /* a fresh query agrees with what the growing call reported */
    emit_b("brk.grow.advanced_by_inc", brk_current() == want);

    /* the newly-exposed heap [base, base+inc) reads back zero and is writable */
    emit_b("brk.grow.zero", all_zero(base, inc));
    emit("brk.grow.writable", "%s", probe_write_tok(base, 0x5a));
    fill_pat(base, inc, 0x77);
    emit_b("brk.grow.readback", check_pat(base, inc, 0x77));

    /* shrink back: the break returns to where it started */
    void *shrunk = brk_call(base);
    emit_b("brk.shrink.ret_is_old_break", shrunk == base);
    emit_b("brk.shrink.restored", brk_current() == base);

    return 0;
}
