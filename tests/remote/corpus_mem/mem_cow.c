/*
 * mem_cow.c — copy-on-write semantics across fork(2):
 *   - a MAP_PRIVATE|ANON page is COW: the child sees the parent's bytes, and the
 *     child's write is private (the parent's copy is unchanged afterwards);
 *   - a MAP_SHARED|ANON page is shared: the child's write IS visible in the
 *     parent;
 *   - a MAP_PRIVATE file mapping is COW against the file: the child's write
 *     reaches neither the parent's mapping nor the on-disk file.
 *
 * Every emitted value is a content invariant ("parent still sees its byte",
 * "parent sees the child's byte"), independent of where the pages live.
 */
#include "mem_common.h"

#define RW (PROT_READ | PROT_WRITE)

static void case_private_anon(void) {
    void *p = mmap(NULL, 2 * PS, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { emit("cow.priv", "%s", "SETUPFAIL"); return; }
    fill_pat(p, 2 * PS, 0xA1);

    int pipefd[2];
    if (pipe(pipefd) != 0) { munmap(p, 2 * PS); emit("cow.priv", "%s", "PIPEFAIL"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        int saw_parent = check_pat(p, 2 * PS, 0xA1);  /* child inherits parent's bytes */
        fill_pat(p, 2 * PS, 0xB2);                    /* private write -> COW break */
        int wrote_own = check_pat(p, 2 * PS, 0xB2);
        int res[2] = { saw_parent, wrote_own };
        ssize_t w = write(pipefd[1], res, sizeof res); (void) w;
        _exit(0);
    }
    close(pipefd[1]);
    int res[2] = { 0, 0 };
    ssize_t r = read(pipefd[0], res, sizeof res); (void) r;
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    emit_b("cow.priv.child_saw_parent", res[0]);
    emit_b("cow.priv.child_wrote_own", res[1]);
    emit_b("cow.priv.parent_unchanged", check_pat(p, 2 * PS, 0xA1));  /* the key invariant */
    munmap(p, 2 * PS);
}

static void case_shared_anon(void) {
    void *s = mmap(NULL, PS, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) { emit("cow.shared", "%s", "SETUPFAIL"); return; }
    fill_pat(s, PS, 0xC3);

    pid_t pid = fork();
    if (pid == 0) {
        fill_pat(s, PS, 0xD4);    /* shared write -> visible in the parent */
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    emit_b("cow.shared.parent_sees_child", check_pat(s, PS, 0xD4));
    munmap(s, PS);
}

static void case_private_file(void) {
    char path[] = "/tmp/memcowXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { emit("cow.file", "%s", "MKSTEMPFAIL"); return; }
    char *buf = malloc(PS);
    fill_pat(buf, PS, 0xE5);
    ssize_t w = write(fd, buf, PS); (void) w;

    void *m = mmap(NULL, PS, RW, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) { free(buf); close(fd); unlink(path); emit("cow.file", "%s", "MMAPFAIL"); return; }
    emit_b("cow.file.maps_file", check_pat(m, PS, 0xE5));

    pid_t pid = fork();
    if (pid == 0) {
        fill_pat(m, PS, 0xF6);    /* private write: invisible to parent and file */
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    emit_b("cow.file.parent_unchanged", check_pat(m, PS, 0xE5));

    /* the on-disk file must also be untouched (MAP_PRIVATE never writes back) */
    char *rb = malloc(PS);
    ssize_t got = pread(fd, rb, PS, 0);
    emit_b("cow.file.disk_unchanged", got == (ssize_t) PS && check_pat(rb, PS, 0xE5));

    free(buf); free(rb);
    munmap(m, PS);
    close(fd);
    unlink(path);
}

int main(int argc, char **argv) {
    mem_init(argc, argv);
    case_private_anon();
    case_shared_anon();
    case_private_file();
    return 0;
}
