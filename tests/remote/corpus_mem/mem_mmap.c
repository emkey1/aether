/*
 * mem_mmap.c — mmap/munmap conformance: anonymous PRIVATE/SHARED mappings,
 * fresh-zero + read/write, flag validation (len 0, PRIVATE+SHARED, missing
 * sharing flag), PROT_NONE enforcement, MAP_FIXED replacement, and
 * MAP_FIXED_NOREPLACE collision semantics. Plus a minimal file-backed mapping
 * (content + offset alignment).
 *
 * Candidate iSH divergences this probes (the conductor confirms against mint):
 *   - MAP_FIXED_NOREPLACE is an unknown flag in iSH (only SHARED/PRIVATE/FIXED/
 *     ANON are defined) so an overlapping request silently RELOCATES instead of
 *     failing EEXIST.
 *   - an unaligned addr HINT (no MAP_FIXED) must be rounded down (success), but
 *     iSH returns EINVAL for any unaligned addr != 0.
 *   - mmap with neither MAP_PRIVATE nor MAP_SHARED must be EINVAL.
 *   - a non-page-aligned file offset must be EINVAL.
 * Everything is an invariant (success / errno / FAULT / "== requested page"),
 * never a raw address.
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

/* relationship of a returned pointer to a requested fixed address, as a stable
 * token: SAME (placed exactly there), ELSEWHERE (placed at a different page), or
 * the errno symbol on failure. */
static const char *placed(void *got, void *want) {
    if (got == MAP_FAILED)
        return errno_name(errno);
    return got == want ? "SAME" : "ELSEWHERE";
}

static void case_anon_basic(void) {
    /* PRIVATE|ANON: succeeds, reads back zero, holds a written byte */
    void *p = mmap(NULL, 4 * PS, RW, ANON_PRIV, -1, 0);
    emit("mmap.anon.priv.ok", "%s", res_mmap(p));
    if (p != MAP_FAILED) {
        emit_b("mmap.anon.priv.zero", all_zero(p, 4 * PS));
        fill_pat(p, 4 * PS, 0x11);
        emit_b("mmap.anon.priv.rw", check_pat(p, 4 * PS, 0x11));
        munmap(p, 4 * PS);
    }

    /* SHARED|ANON: same guarantees (write-visibility across fork is in mem_cow) */
    void *s = mmap(NULL, 2 * PS, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    emit("mmap.anon.shared.ok", "%s", res_mmap(s));
    if (s != MAP_FAILED) {
        emit_b("mmap.anon.shared.zero", all_zero(s, 2 * PS));
        fill_pat(s, 2 * PS, 0x22);
        emit_b("mmap.anon.shared.rw", check_pat(s, 2 * PS, 0x22));
        munmap(s, 2 * PS);
    }
}

static void case_flag_validation(void) {
    /* length 0 -> EINVAL */
    void *z = mmap(NULL, 0, RW, ANON_PRIV, -1, 0);
    emit("mmap.len0", "%s", res_mmap(z));
    if (z != MAP_FAILED) munmap(z, PS);

    /* PRIVATE and SHARED together -> EINVAL */
    void *b = mmap(NULL, PS, RW, MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    emit("mmap.priv_and_shared", "%s", res_mmap(b));
    if (b != MAP_FAILED) munmap(b, PS);

    /* neither PRIVATE nor SHARED -> EINVAL (exactly one is required) */
    void *n = mmap(NULL, PS, RW, MAP_ANONYMOUS, -1, 0);
    emit("mmap.no_sharing_flag", "%s", res_mmap(n));
    if (n != MAP_FAILED) munmap(n, PS);
}

static void case_prot_none(void) {
    /* PROT_NONE mapping must exist but fault on any access */
    void *p = mmap(NULL, PS, PROT_NONE, ANON_PRIV, -1, 0);
    emit("mmap.protnone.ok", "%s", res_mmap(p));
    if (p != MAP_FAILED) {
        emit("mmap.protnone.read", "%s", probe_read_tok(p));
        emit("mmap.protnone.write", "%s", probe_write_tok(p, 1));
        munmap(p, PS);
    }
}

static void case_fixed_replace(void) {
    /* MAP_FIXED over our own existing mapping replaces it exactly, and the
     * replacement is a fresh (zeroed) anonymous mapping. */
    void *base = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (base == MAP_FAILED) { emit("mmap.fixed.replace", "%s", "SETUPFAIL"); return; }
    fill_pat(base, PS, 0x33);
    void *r = mmap(base, PS, RW, ANON_PRIV | MAP_FIXED, -1, 0);
    emit("mmap.fixed.replace.place", "%s", placed(r, base));
    if (r != MAP_FAILED) {
        emit_b("mmap.fixed.replace.fresh_zero", all_zero(r, PS));
        munmap(r, PS);
    } else {
        munmap(base, PS);
    }

    /* MAP_FIXED with an unaligned address -> EINVAL */
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two != MAP_FAILED) {
        void *u = mmap((char *) two + 1, PS, RW, ANON_PRIV | MAP_FIXED, -1, 0);
        emit("mmap.fixed.unaligned", "%s", res_mmap(u));
        if (u != MAP_FAILED && u != two) munmap(u, PS);
        munmap(two, 2 * PS);
    }
}

static void case_fixed_noreplace(void) {
    /* over an EXISTING mapping: MAP_FIXED_NOREPLACE must fail EEXIST (never
     * silently relocate, never clobber). */
    void *occ = mmap(NULL, PS, RW, ANON_PRIV, -1, 0);
    if (occ == MAP_FAILED) { emit("mmap.fixed_noreplace.occupied", "%s", "SETUPFAIL"); return; }
    fill_pat(occ, PS, 0x44);
    void *r = mmap(occ, PS, RW, ANON_PRIV | MAP_FIXED_NOREPLACE, -1, 0);
    /* EEXIST | SAME | ELSEWHERE */
    emit("mmap.fixed_noreplace.occupied", "%s", placed(r, occ));
    /* the original mapping must be untouched whatever happened */
    emit_b("mmap.fixed_noreplace.orig_intact", check_pat(occ, PS, 0x44));
    if (r != MAP_FAILED && r != occ) munmap(r, PS);
    munmap(occ, PS);

    /* over a hole: MAP_FIXED_NOREPLACE must land exactly at the requested page */
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two == MAP_FAILED) { emit("mmap.fixed_noreplace.hole", "%s", "SETUPFAIL"); return; }
    void *want = (char *) two + PS;
    munmap(want, PS);                       /* punch a one-page hole */
    void *h = mmap(want, PS, RW, ANON_PRIV | MAP_FIXED_NOREPLACE, -1, 0);
    emit("mmap.fixed_noreplace.hole", "%s", placed(h, want));
    if (h != MAP_FAILED) munmap(h, PS);
    munmap(two, PS);                        /* free the first page (hole already gone) */
}

static void case_unaligned_hint(void) {
    /* an unaligned addr HINT without MAP_FIXED is rounded down and succeeds
     * (placement is the kernel's choice; we only assert it did NOT error). */
    void *region = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (region == MAP_FAILED) { emit("mmap.hint.unaligned", "%s", "SETUPFAIL"); return; }
    munmap(region, 2 * PS);                 /* free it; use a stale unaligned hint into it */
    void *r = mmap((char *) region + 1, PS, RW, ANON_PRIV, -1, 0);
    emit("mmap.hint.unaligned", "%s", res_mmap(r));
    if (r != MAP_FAILED) munmap(r, PS);
}

static void case_file_backed(void) {
    char path[] = "/tmp/memmapXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { emit("mmap.file", "%s", "MKSTEMPFAIL"); return; }
    unlink(path);                            /* keep it anonymous-on-disk; fd holds it */
    char *src = malloc(PS);
    fill_pat(src, PS, 0x55);
    ssize_t w = write(fd, src, PS);
    (void) w;

    /* PROT_READ MAP_PRIVATE: contents must equal the file */
    void *fp = mmap(NULL, PS, PROT_READ, MAP_PRIVATE, fd, 0);
    emit("mmap.file.priv.ok", "%s", res_mmap(fp));
    if (fp != MAP_FAILED) {
        emit_b("mmap.file.priv.content", check_pat(fp, PS, 0x55));
        munmap(fp, PS);
    }

    /* a non-page-aligned file offset must be EINVAL */
    void *bad = mmap(NULL, PS, PROT_READ, MAP_PRIVATE, fd, 1);
    emit("mmap.file.offset.unaligned", "%s", res_mmap(bad));
    if (bad != MAP_FAILED) munmap(bad, PS);

    free(src);
    close(fd);
}

int main(int argc, char **argv) {
    mem_init(argc, argv);
    case_anon_basic();
    case_flag_validation();
    case_prot_none();
    case_fixed_replace();
    case_fixed_noreplace();
    case_unaligned_hint();
    case_file_backed();
    return 0;
}
