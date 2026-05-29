#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "jit/jit.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "xX_main_Xx.h"

extern void run_at_boot(void);

static void configure_standalone_i386_safety(int argc, char *const argv[]) {
#if defined(__APPLE__) && defined(__aarch64__)
    int saved_optind = optind;
    int saved_opterr = opterr;
    optind = 1;
    opterr = 0;

    int opt;
    while ((opt = getopt(argc, argv, "+r:f:d:c:")) != -1) {
        switch (opt) {
            case 'r':
            case 'f':
            case 'd':
            case 'c':
                break;
            default:
                optind = saved_optind;
                opterr = saved_opterr;
                return;
        }
    }

    const char *command = optind < argc ? argv[optind] : NULL;
    optind = saved_optind;
    opterr = saved_opterr;

    const char *force_jit = getenv("ISH_HOST_I386_JIT");
    if (force_jit == NULL)
        return;

    if (strcmp(force_jit, "1") == 0 || strcasecmp(force_jit, "true") == 0 ||
            strcasecmp(force_jit, "yes") == 0 || strcasecmp(force_jit, "on") == 0)
        return;

    if (command == NULL)
        return;

    const char *basename = strrchr(command, '/');
    const char *comm = basename != NULL ? basename + 1 : command;
    if (comm == NULL || comm[0] == '\0')
        return;

    i386_single_step_comm_set(comm);
    i386_no_cache_comm_set(comm);
#else
    (void) argc;
    (void) argv;
#endif
}

static void configure_standalone_amd64_jit(void) {
    const char *force_jit = getenv("ISH_HOST_AMD64_JIT");
    if (force_jit == NULL) {
        amd64_jit_set_enabled(false);
        return;
    }

    if (strcmp(force_jit, "1") == 0 || strcasecmp(force_jit, "true") == 0 ||
            strcasecmp(force_jit, "yes") == 0 || strcasecmp(force_jit, "on") == 0) {
        amd64_jit_set_enabled(true);
        return;
    }

    if (strcmp(force_jit, "0") == 0 || strcasecmp(force_jit, "false") == 0 ||
            strcasecmp(force_jit, "no") == 0 || strcasecmp(force_jit, "off") == 0) {
        amd64_jit_set_enabled(false);
        return;
    }
}

static char *build_envp_from_term(void) {
    const char *term = getenv("TERM");
    if (term == NULL) {
        char *envp = malloc(1);
        if (envp != NULL)
            envp[0] = '\0';
        return envp;
    }

    size_t term_len = strlen(term);
    char *envp = malloc(sizeof("TERM=") + term_len + 1);
    if (envp == NULL)
        return NULL;

    snprintf(envp, sizeof("TERM=") + term_len, "TERM=%s", term);
    envp[sizeof("TERM=") + term_len - 1] = '\0';
    envp[sizeof("TERM=") + term_len] = '\0';
    return envp;
}

static void ignore_eexist(int err) {
    if (err < 0 && err != _EEXIST)
        fprintf(stderr, "warning: setup step failed: %s\n", strerror(-err));
}

static void setup_host_mounts(void) {
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev", 0755));
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev/pts", 0755));
    ignore_eexist(generic_mkdirat(AT_PWD, "/proc", 0555));
    ignore_eexist(generic_mkdirat(AT_PWD, "/sys", 0555));

    if (access("tests/audio", R_OK) == 0) {
        ignore_eexist(generic_mkdirat(AT_PWD, "/AOK", 0555));
        ignore_eexist(do_mount(&aokfs, ".", "/AOK", "", MS_READONLY_));
    }

    ignore_eexist(do_mount(&procfs, "proc", "/proc", "", 0));
    ignore_eexist(do_mount(&sysfs, "sysfs", "/sys", "", 0));
    ignore_eexist(do_mount(&devptsfs, "devpts", "/dev/pts", "", 0));
}

int main(int argc, char *const argv[]) {
    run_at_boot();
    configure_standalone_i386_safety(argc, argv);
    configure_standalone_amd64_jit();

    char *envp = build_envp_from_term();
    if (envp == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        return 1;
    }

    int err = xX_main_Xx(argc, argv, envp);
    free(envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return 1;
    }

    setup_host_mounts();
    task_run_current();
}
