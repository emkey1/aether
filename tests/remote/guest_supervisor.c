/*
 * guest_supervisor — crash-resilient batch runner that lives in the guest.
 *
 * The conductor can run each corpus test as its own `ish` invocation locally,
 * because a local crash is just a subprocess signal-exit it can read directly.
 * On a real device that model breaks: when a JIT bug crashes the emulator it
 * takes down the whole app *including the sshd the conductor is talking to*, so
 * the in-flight test's exit status and the ssh session are both lost.
 *
 * This supervisor runs the whole batch in one launch and writes a JOURNAL:
 *   SUPER-START <idx> <name>      before each test (flushed; fsync'd to the fs)
 *   <the test's own output ...>   (streamed to stdout only)
 *   SUPER-END   <idx> <rc>        after each test
 * The journal goes both to stdout (live streaming over ssh) and, with
 * --journal, to a file on the guest filesystem. That file is fsync'd and
 * survives the app crashing and being relaunched, so afterwards the conductor
 * can read it and the one test with a SUPER-START but no SUPER-END is exactly
 * the test that crashed (or hung) the emulator. Each test self-selects its
 * engine via --engine, so the supervisor stays engine-agnostic.
 *
 * Usage: guest_supervisor [--engine SPEC] [--seed S] [--journal PATH]
 *                         <test-binary> [<test-binary> ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static int journal_fd = -1;

/* Emit a journal line to stdout (streamed) and, if open, the fs journal
 * (persisted across an app crash). Both are flushed immediately. */
static void journal(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf)
        n = sizeof buf;
    fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
    if (journal_fd >= 0) {
        (void)!write(journal_fd, buf, (size_t)n);
        fsync(journal_fd);
    }
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int main(int argc, char **argv) {
    const char *engine = NULL, *seed = NULL, *journal_path = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = argv[++i];
        else if (!strcmp(argv[i], "--journal") && i + 1 < argc)
            journal_path = argv[++i];
        else
            break; /* remaining args are test binaries */
    }
    if (journal_path)
        journal_fd = open(journal_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    int failures = 0;
    for (int t = i; t < argc; t++) {
        const char *path = argv[t];
        int idx = t - i;
        journal("SUPER-START %d %s\n", idx, base_name(path));

        pid_t pid = fork();
        if (pid == 0) {
            /* child inherits stdout, so the test's lines land in the stream
             * between this test's START and END markers. */
            const char *cargv[8];
            int c = 0;
            cargv[c++] = path;
            if (engine) { cargv[c++] = "--engine"; cargv[c++] = engine; }
            if (seed)   { cargv[c++] = "--seed";   cargv[c++] = seed; }
            cargv[c] = NULL;
            execv(path, (char *const *)cargv);
            _exit(127); /* exec failed */
        }
        if (pid < 0) {
            journal("SUPER-END %d %d\n", idx, -1);
            failures++;
            continue;
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0)
            ;
        int rc = WIFEXITED(status) ? WEXITSTATUS(status)
               : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
        journal("SUPER-END %d %d\n", idx, rc);
        if (rc != 0)
            failures++;
    }
    if (journal_fd >= 0)
        close(journal_fd);
    return failures ? 1 : 0;
}
