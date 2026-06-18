/*
 * mem_mincore.c — mincore conformance: success on a mapped range, ENOMEM on an
 * unmapped range, EINVAL on an unaligned address, EFAULT on a bad vector
 * pointer, and the len-0 corner.
 *
 * RESIDENCY (the low bit of each vec byte) is inherently unpredictable — a page
 * may or may not be resident at any instant — so we NEVER assert vec contents,
 * only the return value / errno (the cmpxchg "mask the unspecified" rule).
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

int main(int argc, char **argv) {
    mem_init(argc, argv);

    unsigned char vec[16];

    /* mapped + touched region: mincore succeeds */
    void *p = map_anon(4, RW);
    if (p) {
        fill_pat(p, 4 * PS, 0x10);   /* fault the pages in */
        emit("mincore.mapped.ret", "%s", res_errno(mincore(p, 4 * PS, vec)));
        /* len 0: Linux returns success (0); a stable corner to compare */
        emit("mincore.len0", "%s", res_errno(mincore(p, 0, vec)));
        /* unaligned addr -> EINVAL */
        emit("mincore.unaligned", "%s", res_errno(mincore((char *) p + 1, PS, vec)));
        /* NULL / unwritable vector -> EFAULT */
        emit("mincore.badvec", "%s", res_errno(mincore(p, PS, NULL)));
        munmap(p, 4 * PS);
    }

    /* fully-unmapped range -> ENOMEM */
    void *q = map_anon(1, RW);
    if (q) {
        munmap(q, PS);
        emit("mincore.unmapped", "%s", res_errno(mincore(q, PS, vec)));
    }

    /* range straddling mapped + unmapped pages -> ENOMEM */
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two != MAP_FAILED) {
        munmap((char *) two + PS, PS);
        emit("mincore.partial", "%s", res_errno(mincore(two, 2 * PS, vec)));
        munmap(two, PS);
    }

    return 0;
}
