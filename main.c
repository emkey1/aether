#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
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
        // The amd64 JIT is only implemented and validated on aarch64 hosts (the
        // iOS target). On other hosts the gadget path is incomplete and SIGSEGVs
        // on even trivial amd64 programs, so default it off and run the
        // interpreter; force it on for development with ISH_HOST_AMD64_JIT=1.
#if defined(__aarch64__)
        amd64_jit_set_enabled(true);
#else
        amd64_jit_set_enabled(false);
#endif
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

// Invoked (via halt_hook) when guest init exits. Mirror init's wait-status as the
// host process exit code, then terminate immediately. Using _exit (after flushing
// stdio) rather than returning is deliberate: this runs on whichever guest thread
// happened to finalize the teardown, while pids_lock and the task's general_lock
// are still held — _exit avoids atexit handlers that might re-enter those locks,
// and lets the OS reclaim every lingering guest pthread cleanly instead of the
// pthread_kill(SIGKILL) sweep that would otherwise kill us with signal 9.
static noreturn void cli_halt(int status) {
    if (getenv("ISH_QUIESCE_STATS") != NULL) {
        extern void quiesce_stats_dump(const char *tag);
        quiesce_stats_dump("exit");
    }
    fflush(NULL);
    if ((status & 0x7f) == 0)          // WIFEXITED
        _exit((status >> 8) & 0xff);
    _exit(128 + (status & 0x7f));      // WIFSIGNALED: shell convention 128+signo
}

static void ignore_eexist(int err) {
    if (err < 0 && err != _EEXIST)
        fprintf(stderr, "warning: setup step failed: %s\n", strerror(-err));
}

static void setup_host_mounts(void) {
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev", 0755));
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev/pts", 0755));
    // Not every bundled root's base tarball ships /dev/shm, and iSH has no
    // boot-time tmpfs auto-mount for it; create it unconditionally so POSIX
    // shm (wl_shm clients, sem_open, etc.) always has somewhere to open.
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev/shm", 01777));
    // ...and enforce the mode when it already exists: mkdirat is an EEXIST
    // no-op, so a pre-existing /dev/shm with the wrong mode (e.g. 0755
    // root:root from a root image) breaks every non-root shm_open() with
    // EACCES. See the matching AppDelegate.m fix (Wayland session died on
    // first keyboard attach because wlroots couldn't allocate the keymap
    // shm file early in boot).
    generic_setattrat(AT_PWD, "/dev/shm", (struct attr) {.type = attr_mode, .mode = S_IFDIR|01777}, false);
    ignore_eexist(generic_mkdirat(AT_PWD, "/proc", 0555));
    ignore_eexist(generic_mkdirat(AT_PWD, "/sys", 0555));

    // aokfs's inline/generated content (README, /tools, /tests, /docs) needs no
    // backing files, but the few bundled real files (test audio, the benchmark
    // tarball) are read from the source tree via mount->source. Resolve that
    // against the source root baked in at build time rather than the process's
    // CWD -- a bare `access("tests/audio", R_OK)` only succeeded when ish was
    // launched from the repo root, so running it from e.g. the build directory
    // silently skipped mounting /AOK entirely (leaving whatever bare directory,
    // if any, the guest rootfs already had at that path).
#ifdef ISH_SOURCE_ROOT
    const char *aok_source_root = ISH_SOURCE_ROOT;
#else
    const char *aok_source_root = ".";
#endif
    char aok_audio_check[MAX_PATH + 1];
    snprintf(aok_audio_check, sizeof(aok_audio_check), "%s/tests/audio", aok_source_root);
    if (access(aok_audio_check, R_OK) == 0) {
        ignore_eexist(generic_mkdirat(AT_PWD, "/AOK", 0555));
        ignore_eexist(do_mount(&aokfs, aok_source_root, "/AOK", "", MS_READONLY_));
    }

    ignore_eexist(do_mount(&procfs, "proc", "/proc", "", 0));
    ignore_eexist(do_mount(&sysfs, "sysfs", "/sys", "", 0));
    ignore_eexist(do_mount(&devptsfs, "devpts", "/dev/pts", "", 0));

    // Dev-only: mount a host directory as realfs at /realmnt to reproduce
    // real-fs-backed behavior (e.g. /AOK/persist) against a local fakefs root.
    const char *real_mnt = getenv("ISH_REAL_MNT");
    if (real_mnt != NULL && real_mnt[0] != '\0') {
        ignore_eexist(generic_mkdirat(AT_PWD, "/realmnt", 0755));
        ignore_eexist(do_mount(&realfs, real_mnt, "/realmnt", "", 0));
    }

    // Dev-only: mount a second fakefs (SQLite-backed) root at /fakemnt2, to
    // reproduce cross-root bugs (e.g. mv between two /AOK/roots-style fakefs
    // mounts) against a local repro rig without the iOS app.
    const char *fake_mnt2 = getenv("ISH_FAKE_MNT2");
    if (fake_mnt2 != NULL && fake_mnt2[0] != '\0') {
        char fake_mnt2_data[MAX_PATH + 1];
        snprintf(fake_mnt2_data, sizeof(fake_mnt2_data), "%s/data", fake_mnt2);
        ignore_eexist(generic_mkdirat(AT_PWD, "/fakemnt2", 0755));
        ignore_eexist(do_mount(&fakefs, fake_mnt2_data, "/fakemnt2", "", 0));
    }
}

int main(int argc, char *const argv[]) {
    run_at_boot();
    configure_standalone_i386_safety(argc, argv);
    configure_standalone_amd64_jit();
    // The CLI now defaults to multicore (like the iOS app), so local and fakefs
    // repro runs exercise the same concurrency -- and the same races -- as a
    // multi-core device. The effective lever is the emulated CPU count
    // (get_cpu_count(), >= 4 on the CLI; see platform/darwin.c); doEnableMulticore
    // is a legacy toggle kept in sync for clarity. Set ISH_MULTICORE=0 to flip
    // the toggle back, or ISH_GUEST_CPU_COUNT=1 to actually run a serial guest.
    {
        extern bool doEnableMulticore;
        doEnableMulticore = true;
        const char *mc = getenv("ISH_MULTICORE");
        if (mc != NULL && (strcmp(mc, "0") == 0 || strcasecmp(mc, "false") == 0 ||
                           strcasecmp(mc, "no") == 0 || strcasecmp(mc, "off") == 0))
            doEnableMulticore = false;
    }
    halt_hook = cli_halt;

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

    // Dev harness for the LLM-Chat guest-shell tool primitive. With
    // ISH_TEST_GUEST_CMD set, run that command in the guest via
    // run_guest_command_capture(), print the captured result, and exit -- a way
    // to validate the primitive against a local fakefs without the iOS app.
    // e.g. ISH_TEST_GUEST_CMD='echo out; echo err >&2; exit 7' ./ish -f alpinex86 /bin/sh
    const char *test_cmd = getenv("ISH_TEST_GUEST_CMD");
    if (test_cmd != NULL) {
        struct guest_command_result r;
        int rc = run_guest_command_capture(test_cmd, NULL, 10000, 0, &r);
        fprintf(stderr,
                "[guest-cmd] rc=%d launched=%d exited=%d code=%d sig=%d timed_out=%d truncated=%d len=%zu\n",
                rc, r.launched, r.exited, r.exit_code, r.term_signal,
                r.timed_out, r.truncated, r.output_len);
        fprintf(stderr, "[guest-cmd] ---output---\n%s\n[guest-cmd] ---end---\n",
                r.output != NULL ? r.output : "(null)");
        free(r.output);
        _exit(0);
    }

    task_run_current();
}
