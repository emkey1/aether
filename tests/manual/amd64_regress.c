#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

static volatile unsigned char exec_bss_guard[1 << 20];

struct lock_race_state {
    int base_fd;
    volatile int shared_fd;
    volatile int stop;
    volatile unsigned long setlk_ok;
    volatile unsigned long setlk_ebadf;
    volatile unsigned long setlk_other;
    volatile unsigned long reopen_ok;
};

static int run_wait_ok(pid_t pid, int *status_out) {
    int status = 0;
    int rc = waitpid(pid, &status, 0);
    if (rc != pid)
        return -1;
    if (status_out != NULL)
        *status_out = status;
    return 0;
}

static int helper_exec_child(void) {
    size_t i;
    for (i = 0; i < sizeof(exec_bss_guard); i++) {
        if (exec_bss_guard[i] != 0)
            return 91;
    }
    exec_bss_guard[0] = 1;
    exec_bss_guard[sizeof(exec_bss_guard) - 1] = 2;
    return 0;
}

static void fill_pattern(int fd, size_t len) {
    unsigned char *buf = malloc(len);
    size_t i;
    ssize_t written;

    if (buf == NULL) {
        perror("malloc");
        exit(1);
    }
    for (i = 0; i < len; i++)
        buf[i] = (unsigned char) ((i * 37u) ^ (i >> 3));

    written = pwrite(fd, buf, len, 0);
    if (written != (ssize_t) len) {
        perror("pwrite");
        exit(1);
    }
    free(buf);
}

static void test_cross_page_private_store(void) {
    static const uint64_t value = 0x1122334455667788ULL;
    char template[] = "/tmp/ish-aok-amd64-cross-page-XXXXXX";
    const unsigned char expected[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    long page_size = sysconf(_SC_PAGESIZE);
    int fd;
    int off;
    int iter;

    if (page_size <= 0) {
        perror("sysconf");
        exit(1);
    }

    fd = mkstemp(template);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    unlink(template);

    if (ftruncate(fd, page_size * 2) != 0) {
        perror("ftruncate");
        exit(1);
    }
    fill_pattern(fd, (size_t) page_size * 2);

    for (iter = 0; iter < 32; iter++) {
        for (off = 1; off <= 7; off++) {
            unsigned char *map = mmap(NULL, (size_t) page_size * 2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE, fd, 0);
            unsigned char file_bytes[8];
            unsigned char original_bytes[8];
            volatile uint64_t *slot;
            int failed;

            if (map == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }

            memcpy(original_bytes, map + page_size - off, sizeof(original_bytes));
            slot = (volatile uint64_t *) (map + page_size - off);
            *slot = value;

            failed = memcmp((const void *) (map + page_size - off), expected, sizeof(expected)) != 0;
            if (failed) {
                test_log_if(1, "cross page store mismatch iter=%d off=%d\n", iter, off);
                failf("amd64 cross page store map", (uint64_t) map[page_size - off + 0],
                      (uint64_t) map[page_size - off + 1], (uint64_t) map[page_size - off + 2],
                      expected[0], expected[1], expected[2]);
            }

            if (pread(fd, file_bytes, sizeof(file_bytes), page_size - off) != (ssize_t) sizeof(file_bytes)) {
                perror("pread");
                exit(1);
            }
            failed = memcmp(file_bytes, original_bytes, sizeof(file_bytes)) != 0;
            test_log_if(failed, "cross page file backing changed iter=%d off=%d\n", iter, off);
            if (failed)
                failf("amd64 cross page backing", file_bytes[0], file_bytes[1], file_bytes[2],
                      original_bytes[0], original_bytes[1], original_bytes[2]);

            if (munmap(map, (size_t) page_size * 2) != 0) {
                perror("munmap");
                exit(1);
            }
        }
    }

    close(fd);
}

static void test_exec_loader_zero(char **argv) {
    int iter;

    for (iter = 0; iter < 64; iter++) {
        pid_t pid = fork();
        int status = 0;

        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            char *child_argv[] = { argv[0], "--exec-child", NULL };
            execv(argv[0], child_argv);
            _exit(127);
        }

        if (run_wait_ok(pid, &status) != 0) {
            perror("waitpid");
            exit(1);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            test_log_if(1, "exec loader zero failed iter=%d status=%#x\n", iter, status);
            failf("amd64 exec loader zero", (uint64_t) WIFEXITED(status),
                  (uint64_t) WEXITSTATUS(status), (uint64_t) status, 1, 0, 0);
            return;
        }
    }
}

static void *lock_race_fcntl_thread(void *arg) {
    struct lock_race_state *state = arg;
    struct flock fl;
    unsigned long iter;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_CUR;
    fl.l_start = 0;
    fl.l_len = 1;

    for (iter = 0; iter < 200000; iter++) {
        int fd = __atomic_load_n(&state->shared_fd, __ATOMIC_ACQUIRE);
        int rc;
        int saved_errno;

        if (fd < 0) {
            sched_yield();
            continue;
        }
        rc = fcntl(fd, F_SETLK, &fl);
        saved_errno = errno;
        if (rc == 0)
            __atomic_add_fetch(&state->setlk_ok, 1, __ATOMIC_RELAXED);
        else if (saved_errno == EBADF)
            __atomic_add_fetch(&state->setlk_ebadf, 1, __ATOMIC_RELAXED);
        else
            __atomic_add_fetch(&state->setlk_other, 1, __ATOMIC_RELAXED);
        if ((iter & 255u) == 0)
            sched_yield();
    }

    __atomic_store_n(&state->stop, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *lock_race_reopen_thread(void *arg) {
    struct lock_race_state *state = arg;

    while (!__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE)) {
        int old_fd = __atomic_exchange_n(&state->shared_fd, -1, __ATOMIC_ACQ_REL);
        int new_fd;

        if (old_fd >= 0)
            close(old_fd);

        new_fd = dup(state->base_fd);
        if (new_fd >= 0) {
            __atomic_add_fetch(&state->reopen_ok, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->shared_fd, new_fd, __ATOMIC_RELEASE);
        }
        sched_yield();
    }
    return NULL;
}

static void test_fcntl_lock_close_race(void) {
    char template[] = "/tmp/ish-aok-amd64-lock-race-XXXXXX";
    struct lock_race_state state;
    pthread_t fcntl_thread;
    pthread_t reopen_thread;
    int fd;
    int failed;

    memset(&state, 0, sizeof(state));
    fd = mkstemp(template);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    unlink(template);
    if (ftruncate(fd, 4096) != 0) {
        perror("ftruncate");
        exit(1);
    }

    state.base_fd = fd;
    state.shared_fd = dup(fd);
    if (state.shared_fd < 0) {
        perror("dup");
        exit(1);
    }

    if (pthread_create(&fcntl_thread, NULL, lock_race_fcntl_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }
    if (pthread_create(&reopen_thread, NULL, lock_race_reopen_thread, &state) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pthread_join(fcntl_thread, NULL);
    pthread_join(reopen_thread, NULL);

    if (state.shared_fd >= 0)
        close((int) state.shared_fd);
    close(fd);

    failed = state.setlk_ok == 0 || state.reopen_ok == 0 || state.setlk_other != 0;
    test_log_if(failed,
                "fcntl lock race: ok=%lu ebadf=%lu other=%lu reopen=%lu\n",
                state.setlk_ok, state.setlk_ebadf, state.setlk_other, state.reopen_ok);
    if (failed)
        failf("amd64 fcntl lock race", state.setlk_ok, state.setlk_ebadf,
              state.setlk_other, 1, 0, 0);
}

static int write_text_file(const char *path, const char *text) {
    size_t len = strlen(text);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    ssize_t written;

    if (fd < 0)
        return -1;
    written = write(fd, text, len);
    close(fd);
    return written == (ssize_t) len ? 0 : -1;
}

static int run_command(char *const argv[]) {
    pid_t pid = fork();
    int status = 0;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (run_wait_ok(pid, &status) != 0)
        return -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void test_cc1_varargs_compile_stress(void) {
    static const char source_text[] =
        "#include <stdarg.h>\n"
        "#include <stdint.h>\n"
        "struct node { long a; long b; long c; long d; };\n"
        "static volatile long sink;\n"
        "__attribute__((noinline))\n"
        "static long consume(int tag, int count, ...) {\n"
        "    va_list ap, copy;\n"
        "    long acc = tag;\n"
        "    int i;\n"
        "    va_start(ap, count);\n"
        "    va_copy(copy, ap);\n"
        "    for (i = 0; i < count; i++) {\n"
        "        long v0 = va_arg(ap, long);\n"
        "        long v1 = va_arg(copy, long);\n"
        "        acc += (v0 ^ (v1 + i)) - (acc >> (i & 7));\n"
        "        if ((i & 3) == 0)\n"
        "            acc ^= (v0 << (i & 15));\n"
        "    }\n"
        "    va_end(copy);\n"
        "    va_end(ap);\n"
        "    return acc;\n"
        "}\n"
        "__attribute__((noinline))\n"
        "static long drive(const struct node *nodes, int n) {\n"
        "    long acc = 0;\n"
        "    int i;\n"
        "    for (i = 0; i < n; i++) {\n"
        "        acc += consume(i, 6,\n"
        "                nodes[i].a, nodes[i].b, nodes[i].c,\n"
        "                nodes[i].d, nodes[i].a + nodes[i].c,\n"
        "                nodes[i].b + nodes[i].d);\n"
        "        if ((acc & 7) == 3)\n"
        "            acc ^= nodes[i].a + nodes[i].d;\n"
        "    }\n"
        "    return acc;\n"
        "}\n"
        "int main(void) {\n"
        "    struct node nodes[64];\n"
        "    int i;\n"
        "    for (i = 0; i < 64; i++) {\n"
        "        nodes[i].a = i + 1;\n"
        "        nodes[i].b = i * 3 + 2;\n"
        "        nodes[i].c = i * 5 + 7;\n"
        "        nodes[i].d = i * 7 + 11;\n"
        "    }\n"
        "    sink = drive(nodes, 64);\n"
        "    return sink == 0x7fffffff ? 1 : 0;\n"
        "}\n";
    char dir_template[] = "/tmp/ish-aok-amd64-cc1-XXXXXX";
    char src_path[256];
    char out_path[256];
    char *compiler = NULL;
    char *cmd_argv[10];
    int rc;
    int iter;

    if (access("/usr/bin/gcc", X_OK) == 0)
        compiler = "/usr/bin/gcc";
    else if (access("/bin/gcc", X_OK) == 0)
        compiler = "/bin/gcc";
    else if (access("/usr/bin/cc", X_OK) == 0)
        compiler = "/usr/bin/cc";
    else if (access("/bin/cc", X_OK) == 0)
        compiler = "/bin/cc";

    if (compiler == NULL) {
        test_logf("skip cc1 stress: no gcc/cc found\n");
        return;
    }

    if (mkdtemp(dir_template) == NULL) {
        perror("mkdtemp");
        exit(1);
    }

    snprintf(src_path, sizeof(src_path), "%s/cc1-varargs.c", dir_template);
    snprintf(out_path, sizeof(out_path), "%s/cc1-varargs.s", dir_template);
    if (write_text_file(src_path, source_text) != 0) {
        perror("write");
        exit(1);
    }

    for (iter = 0; iter < 8; iter++) {
        int argc = 0;
        cmd_argv[argc++] = compiler;
        cmd_argv[argc++] = "-O2";
        cmd_argv[argc++] = "-fno-inline";
        cmd_argv[argc++] = "-fno-tree-vectorize";
        cmd_argv[argc++] = "-fno-omit-frame-pointer";
        cmd_argv[argc++] = "-S";
        cmd_argv[argc++] = src_path;
        cmd_argv[argc++] = "-o";
        cmd_argv[argc++] = out_path;
        cmd_argv[argc] = NULL;

        rc = run_command(cmd_argv);
        test_log_if(rc != 0, "cc1 stress iteration=%d rc=%d compiler=%s\n", iter, rc, compiler);
        if (rc != 0) {
            failf("amd64 cc1 stress", (uint64_t) rc, (uint64_t) iter, 0, 0, 0, 0);
            break;
        }
    }
}

// Native JIT push/pop gadgets: push r8-r15 (REX.B-encoded) and pop them back in
// reverse, verifying strict LIFO order and 64-bit value integrity. The compiler
// also saves/restores its own callee-saved regs around this block, adding more
// push/pop. (gen.c emits gadget_amd64_push_reg/pop_reg for 0x50-0x5f.)
static void test_push_pop_lifo(void) {
    uint64_t out[8];
    uint64_t *p = out;
    __asm__ volatile(
        "movq $0x000000000000A000, %%r8\n\t"
        "movq $0x000000000000B000, %%r9\n\t"
        "movq $0x000000000000C000, %%r10\n\t"
        "movq $0x000000000000D000, %%r11\n\t"
        "movq $0x000000000000E000, %%r12\n\t"
        "movq $0x000000000000F000, %%r13\n\t"
        "movq $0x000000000001A000, %%r14\n\t"
        "movq $0x000000000001B000, %%r15\n\t"
        "pushq %%r8\n\t pushq %%r9\n\t pushq %%r10\n\t pushq %%r11\n\t"
        "pushq %%r12\n\t pushq %%r13\n\t pushq %%r14\n\t pushq %%r15\n\t"
        "popq %%rax\n\t movq %%rax, 0(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 8(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 16(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 24(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 32(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 40(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 48(%0)\n\t"
        "popq %%rax\n\t movq %%rax, 56(%0)\n\t"
        : : "r"(p)
        : "rax", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "memory");
    static const uint64_t want[8] = {
        0x1B000, 0x1A000, 0xF000, 0xE000, 0xD000, 0xC000, 0xB000, 0xA000
    };
    for (int i = 0; i < 8; i++) {
        if (out[i] != want[i]) {
            failf("push_pop_lifo", (uint64_t) i, out[i], want[i], 0, 0, 0);
            return;
        }
    }
    test_logf("push_pop_lifo: ok\n");
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--exec-child") == 0)
        return helper_exec_child();

    test_init(argc, argv);
    test_cross_page_private_store();
    test_exec_loader_zero(argv);
    test_fcntl_lock_close_race();
    test_push_pop_lifo();
    test_cc1_varargs_compile_stress();
    return finish_suite("amd64_regress");
}
