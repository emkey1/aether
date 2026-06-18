/*
 * mem_msync.c — msync conformance. iSH treats msync as a pure no-op returning 0
 * for everything, which is fine for the *data* (anonymous/shared mappings share
 * host pages so there is nothing to flush) but diverges on VALIDATION: real Linux
 * rejects bad flag combinations and unmapped ranges.
 *
 * Candidate iSH divergences:
 *   - MS_SYNC and MS_ASYNC together must be EINVAL (mutually exclusive);
 *   - an unknown flag bit must be EINVAL;
 *   - an unaligned address must be EINVAL;
 *   - an unmapped range must be ENOMEM.
 * The valid forms (MS_SYNC / MS_ASYNC / MS_INVALIDATE) return 0 on both.
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

#ifndef MS_ASYNC
#define MS_ASYNC 1
#endif
#ifndef MS_INVALIDATE
#define MS_INVALIDATE 2
#endif
#ifndef MS_SYNC
#define MS_SYNC 4
#endif

int main(int argc, char **argv) {
    mem_init(argc, argv);

    /* a real (file-backed, shared) mapping so MS_SYNC has well-defined meaning */
    char path[] = "/tmp/memmsyncXXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) unlink(path);
    if (fd >= 0) {
        char *buf = malloc(PS);
        fill_pat(buf, PS, 0x10);
        ssize_t w = write(fd, buf, PS); (void) w;
        free(buf);
        void *m = mmap(NULL, PS, RW, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            /* valid forms succeed */
            emit("msync.sync", "%s", res_errno(msync(m, PS, MS_SYNC)));
            emit("msync.async", "%s", res_errno(msync(m, PS, MS_ASYNC)));
            emit("msync.invalidate", "%s", res_errno(msync(m, PS, MS_INVALIDATE)));
            /* invalid forms */
            emit("msync.sync_and_async", "%s", res_errno(msync(m, PS, MS_SYNC | MS_ASYNC)));
            emit("msync.bad_flag", "%s", res_errno(msync(m, PS, 0x10)));
            emit("msync.unaligned", "%s", res_errno(msync((char *) m + 1, PS, MS_SYNC)));
            munmap(m, PS);
        }
        close(fd);
    }

    /* anonymous shared mapping also msyncs cleanly (no-op) */
    void *a = mmap(NULL, PS, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (a != MAP_FAILED) {
        emit("msync.anon.sync", "%s", res_errno(msync(a, PS, MS_SYNC)));
        munmap(a, PS);
    }

    /* unmapped range -> ENOMEM */
    void *q = map_anon(1, RW);
    if (q) {
        munmap(q, PS);
        emit("msync.unmapped", "%s", res_errno(msync(q, PS, MS_SYNC)));
    }

    /* range straddling mapped + unmapped -> ENOMEM */
    void *two = mmap(NULL, 2 * PS, RW, ANON_PRIV, -1, 0);
    if (two != MAP_FAILED) {
        munmap((char *) two + PS, PS);
        emit("msync.partial", "%s", res_errno(msync(two, 2 * PS, MS_SYNC)));
        munmap(two, PS);
    }

    return 0;
}
