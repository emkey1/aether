/*
 * proc_clone.c — observable effects of the clone(2) resource-sharing flags:
 * CLONE_VM (shared address space), CLONE_FILES (shared fd table), CLONE_FS
 * (shared cwd), CLONE_THREAD (same tgid, distinct tid), and the tid-reporting
 * flags CLONE_PARENT_SETTID / CLONE_CHILD_SETTID.
 *
 * Each flag is tested by a paired contrast: the same operation run WITH and
 * WITHOUT the sharing flag, so the difference isolates exactly that flag's
 * effect. Child functions use the glibc/musl clone() wrapper (returning from fn
 * does a thread-only SYS_exit, never exit_group) and a separate mmap'd stack.
 */
#include "proc_common.h"
#include <sys/mman.h>

#ifndef MAP_STACK
#define MAP_STACK 0
#endif

/* shared-by-CLONE_VM scratch (a plain heap object: visible to the child only
 * when the address space is shared — that is itself the CLONE_VM test) */
struct shared {
    volatile int done;
    volatile int child_pid;
    volatile int child_tid;
    volatile int child_ctid;
    volatile long magic;
    volatile int opened_ok;
};

#define STACKSZ (256 * 1024)
static char *new_stack(void) {
    void *p = mmap(NULL, STACKSZ, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    return (char *) p + STACKSZ;       /* clone() wants the top of the stack */
}

static void wait_done(struct shared *s) {
    for (int i = 0; i < 5000 && !s->done; i++)
        proc_sleep_ms(1);
}

/* identity: record getpid (tgid) and gettid into shared memory */
static int fn_identity(void *arg) {
    struct shared *s = arg;
    s->child_pid = getpid();
    s->child_tid = (int) syscall(SYS_gettid);
    __sync_synchronize();
    s->done = 1;
    return 0;
}

/* CLONE_THREAD: same tgid as parent, distinct tid. !CLONE_THREAD: distinct tgid. */
static void case_thread_identity(void) {
    struct shared *s = calloc(1, sizeof *s);
    char *stk = new_stack();
    if (!s || !stk) { emit("clone.thread", "%s", "SETUPFAIL"); return; }
    int parent_pid = getpid();
    int parent_tid = (int) syscall(SYS_gettid);

    s->done = 0;
    int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    pid_t rc = clone(fn_identity, stk, flags, s);
    if (rc < 0) { emit("clone.thread", "%s", "CLONEFAIL"); free(s); return; }
    wait_done(s);
    emit_i("clone.thread.same_tgid", s->child_pid == parent_pid);
    emit_i("clone.thread.distinct_tid", s->child_tid != parent_tid);

    /* contrast: a plain process clone has its own tgid == its own tid */
    struct shared *s2 = calloc(1, sizeof *s2);
    char *stk2 = new_stack();
    if (s2 && stk2) {
        s2->done = 0;
        pid_t rc2 = clone(fn_identity, stk2, SIGCHLD, s2);
        if (rc2 > 0) {
            proc_wait_blocking(rc2, NULL, 0);
            emit_i("clone.proc.own_tgid", s2->child_pid != parent_pid);
            emit_i("clone.proc.tgid_eq_tid", s2->child_pid == s2->child_tid);
        }
    }
    free(s);
}

/* CLONE_VM: a child's write to the shared heap object is visible to the parent */
static int fn_writemem(void *arg) {
    struct shared *s = arg;
    s->magic = 0x5eed1234;
    __sync_synchronize();
    s->done = 1;
    return 0;
}

static void case_vm_sharing(void) {
    struct shared *s = calloc(1, sizeof *s);
    char *stk = new_stack();
    if (!s || !stk) { emit("clone.vm", "%s", "SETUPFAIL"); return; }

    s->magic = 0; s->done = 0;
    pid_t rc = clone(fn_writemem, stk, CLONE_VM | CLONE_SIGHAND | SIGCHLD, s);
    if (rc < 0) { emit("clone.vm", "%s", "CLONEFAIL"); free(s); return; }
    wait_done(s);
    /* with a shared address space the parent observes the child's write */
    emit_i("clone.vm.shared_write_seen", s->magic == 0x5eed1234);
    proc_wait_blocking(rc, NULL, 0);

    /* contrast: a fork child's write to (copied) memory is NOT seen */
    s->magic = 0;
    pid_t f = fork();
    if (f == 0) { s->magic = 0x5eed1234; _exit(0); }
    proc_wait_blocking(f, NULL, 0);
    emit_i("clone.novm.write_not_seen", s->magic == 0);
    free(s);
}

/* CLONE_FILES: a child opens an fd at a fixed slot; with a shared fd table the
 * parent sees that slot valid, otherwise EBADF. CLONE_VM lets us pass the result. */
static int fn_openfd(void *arg) {
    struct shared *s = arg;
    int fd = open("/", O_RDONLY);
    s->opened_ok = 0;
    if (fd >= 0) {
        /* move to a high slot the parent is sure not to use */
        if (dup2(fd, 200) == 200)
            s->opened_ok = 1;
        if (fd != 200)
            close(fd);
    }
    __sync_synchronize();
    s->done = 1;
    return 0;
}

static void case_files_sharing(void) {
    struct shared *s = calloc(1, sizeof *s);
    char *stk = new_stack(), *stk2 = new_stack();
    if (!s || !stk || !stk2) { emit("clone.files", "%s", "SETUPFAIL"); return; }

    /* WITH CLONE_FILES: fd 200 opened by the child is valid in the parent */
    s->done = 0; s->opened_ok = 0;
    pid_t rc = clone(fn_openfd, stk, CLONE_VM | CLONE_FILES | CLONE_SIGHAND | SIGCHLD, s);
    if (rc < 0) { emit("clone.files", "%s", "CLONEFAIL"); free(s); return; }
    wait_done(s);
    int shared_valid = (s->opened_ok && fcntl(200, F_GETFD) != -1);
    emit_i("clone.files.shared_fd_visible", shared_valid);
    proc_wait_blocking(rc, NULL, 0);
    if (fcntl(200, F_GETFD) != -1) close(200);

    /* WITHOUT CLONE_FILES: the child's fd 200 is in its own (copied) table */
    s->done = 0; s->opened_ok = 0;
    pid_t rc2 = clone(fn_openfd, stk2, CLONE_VM | CLONE_SIGHAND | SIGCHLD, s);
    if (rc2 > 0) {
        wait_done(s);
        int parent_has = (fcntl(200, F_GETFD) != -1);
        emit_i("clone.nofiles.fd_not_visible", s->opened_ok && !parent_has);
        proc_wait_blocking(rc2, NULL, 0);
        if (fcntl(200, F_GETFD) != -1) close(200);
    }
    free(s);
}

/* CLONE_FS: a child's chdir changes the parent's cwd too */
static int fn_chdir_root(void *arg) {
    struct shared *s = arg;
    chdir("/");
    __sync_synchronize();
    s->done = 1;
    return 0;
}

static void case_fs_sharing(void) {
    struct shared *s = calloc(1, sizeof *s);
    char *stk = new_stack(), *stk2 = new_stack();
    if (!s || !stk || !stk2) { emit("clone.fs", "%s", "SETUPFAIL"); return; }
    char dir[] = "/tmp/proccloneXXXXXX";
    if (!mkdtemp(dir)) { emit("clone.fs", "%s", "MKDTEMPFAIL"); free(s); return; }

    /* WITH CLONE_FS: child chdir("/") moves the parent's cwd to "/" */
    chdir(dir);
    s->done = 0;
    pid_t rc = clone(fn_chdir_root, stk, CLONE_VM | CLONE_FS | CLONE_SIGHAND | SIGCHLD, s);
    if (rc < 0) { emit("clone.fs", "%s", "CLONEFAIL"); free(s); return; }
    proc_wait_blocking(rc, NULL, 0);
    char cwd[256] = {0}; char *g = getcwd(cwd, sizeof cwd);
    emit_i("clone.fs.cwd_followed", g != NULL && strcmp(cwd, "/") == 0);

    /* WITHOUT CLONE_FS: child chdir only affects its own (copied) fs context */
    chdir(dir);
    s->done = 0;
    pid_t rc2 = clone(fn_chdir_root, stk2, CLONE_VM | CLONE_SIGHAND | SIGCHLD, s);
    if (rc2 > 0) {
        proc_wait_blocking(rc2, NULL, 0);
        cwd[0] = 0; g = getcwd(cwd, sizeof cwd);
        emit_i("clone.nofs.cwd_unchanged", g != NULL && strcmp(cwd, dir) == 0);
    }
    chdir("/tmp"); rmdir(dir);
    free(s);
}

/* CLONE_PARENT_SETTID writes the child tid into the parent's buffer; the value
 * equals clone()'s return. CLONE_CHILD_SETTID writes it into a shared buffer
 * the child sees. */
static int fn_settid(void *arg) {
    struct shared *s = arg;
    __sync_synchronize();
    s->done = 1;
    return 0;
}

static void case_settid(void) {
    struct shared *s = calloc(1, sizeof *s);
    char *stk = new_stack(), *stk2 = new_stack();
    if (!s || !stk || !stk2) { emit("clone.settid", "%s", "SETUPFAIL"); return; }

    pid_t ptid = -1;
    s->done = 0;
    pid_t rc = clone(fn_settid, stk, CLONE_VM | CLONE_PARENT_SETTID | CLONE_SIGHAND | SIGCHLD,
                     s, &ptid);
    if (rc > 0) {
        wait_done(s);
        emit_i("clone.parent_settid.matches_ret", ptid == rc);
        proc_wait_blocking(rc, NULL, 0);
    } else emit("clone.parent_settid", "%s", "CLONEFAIL");

    s->done = 0; s->child_ctid = -1;
    pid_t rc2 = clone(fn_settid, stk2, CLONE_VM | CLONE_CHILD_SETTID | CLONE_SIGHAND | SIGCHLD,
                      s, NULL, NULL, (pid_t *) &s->child_ctid);
    if (rc2 > 0) {
        wait_done(s);
        emit_i("clone.child_settid.matches_ret", s->child_ctid == rc2);
        proc_wait_blocking(rc2, NULL, 0);
    } else emit("clone.child_settid", "%s", "CLONEFAIL");
    free(s);
}

int main(int argc, char **argv) {
    proc_init(argc, argv);
    case_thread_identity();
    case_vm_sharing();
    case_files_sharing();
    case_fs_sharing();
    case_settid();
    return 0;
}
