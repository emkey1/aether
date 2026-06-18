/*
 * mem_mprotect.c — mprotect conformance: removing write/all access must be
 * ENFORCED (subsequent access faults), restoring access must work and preserve
 * contents, and error corners (unmapped -> ENOMEM, unaligned -> EINVAL, len 0
 * -> success).
 *
 * The fault observations are the heart of this: a write to a PROT_READ page and
 * any access to a PROT_NONE page MUST raise SIGSEGV on a correct kernel. They are
 * emitted as FAULT/OK (never an address), so they compare cleanly across arches.
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

static void case_drop_write(void) {
    void *p = map_anon(1, RW);
    if (!p) { emit("mprotect.droptowrite", "%s", "SETUPFAIL"); return; }
    fill_pat(p, PS, 0x10);
    /* drop to read-only: reads still OK, writes must fault */
    emit("mprotect.ro.ret", "%s", res_errno(mprotect(p, PS, PROT_READ)));
    emit("mprotect.ro.read", "%s", probe_read_tok(p));            /* OK */
    emit("mprotect.ro.write", "%s", probe_write_tok(p, 0x99));    /* FAULT */
    emit_b("mprotect.ro.content_intact", check_pat(p, PS, 0x10)); /* unchanged */
    /* restore RW: writes work again and old content is preserved */
    emit("mprotect.restore.ret", "%s", res_errno(mprotect(p, PS, RW)));
    emit_b("mprotect.restore.content", check_pat(p, PS, 0x10));
    emit("mprotect.restore.write", "%s", probe_write_tok(p, 0x99)); /* OK */
    munmap(p, PS);
}

static void case_drop_all(void) {
    void *p = map_anon(1, RW);
    if (!p) { emit("mprotect.droptonone", "%s", "SETUPFAIL"); return; }
    fill_pat(p, PS, 0x20);
    emit("mprotect.none.ret", "%s", res_errno(mprotect(p, PS, PROT_NONE)));
    emit("mprotect.none.read", "%s", probe_read_tok(p));    /* FAULT */
    emit("mprotect.none.write", "%s", probe_write_tok(p, 1)); /* FAULT */
    /* re-grant read: the original bytes must still be there (same mapping) */
    emit("mprotect.regrant.ret", "%s", res_errno(mprotect(p, PS, PROT_READ)));
    emit("mprotect.regrant.read", "%s", probe_read_tok(p));  /* OK */
    emit_b("mprotect.regrant.content", check_pat(p, PS, 0x20));
    munmap(p, PS);
}

/* A page mapped PROT_NONE then promoted to PROT_READ reads back as zero
 * (anonymous), exercising iSH's "reserve PROT_NONE without host backing, then
 * allocate on first grant" path. */
static void case_none_then_read(void) {
    void *p = mmap(NULL, PS, PROT_NONE, ANON_PRIV, -1, 0);
    if (p == MAP_FAILED) { emit("mprotect.none_then_read", "%s", "SETUPFAIL"); return; }
    emit("mprotect.lazy.read_before", "%s", probe_read_tok(p)); /* FAULT */
    emit("mprotect.lazy.grant", "%s", res_errno(mprotect(p, PS, PROT_READ)));
    emit("mprotect.lazy.read_after", "%s", probe_read_tok(p));  /* OK */
    emit_b("mprotect.lazy.zero", all_zero(p, PS));
    munmap(p, PS);
}

static void case_errors(void) {
    /* unmapped range -> ENOMEM */
    void *p = map_anon(1, RW);
    void *hole = NULL;
    if (p) { hole = p; munmap(p, PS); }     /* a known-unmapped page address */
    if (hole)
        emit("mprotect.unmapped", "%s", res_errno(mprotect(hole, PS, PROT_READ)));

    /* len 0 is a successful no-op. (An unaligned address is not tested here:
     * musl's mprotect() rounds the address down to a page boundary before the
     * syscall, so the guest never sees an unaligned value.) */
    void *q = map_anon(1, RW);
    if (q) {
        emit("mprotect.len0", "%s", res_errno(mprotect(q, 0, PROT_READ)));
        munmap(q, PS);
    }

    /* a range straddling mapped + unmapped pages -> ENOMEM */
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two != MAP_FAILED) {
        munmap((char *) two + PS, PS);      /* unmap the 2nd page, keep the 1st */
        emit("mprotect.partial", "%s", res_errno(mprotect(two, 2 * PS, PROT_READ)));
        munmap(two, PS);
    }
}

int main(int argc, char **argv) {
    mem_init(argc, argv);
    case_drop_write();
    case_drop_all();
    case_none_then_read();
    case_errors();
    return 0;
}
