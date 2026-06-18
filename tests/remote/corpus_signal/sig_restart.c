/*
 * sig_restart.c — SA_RESTART vs EINTR across syscalls.
 *
 * Linux rule, asserted against the real kernel:
 *   - read/waitpid with    SA_RESTART -> transparently restart (no EINTR seen);
 *   - read/waitpid without SA_RESTART -> fail with EINTR;
 *   - poll/select and pause NEVER restart, even with SA_RESTART -> always EINTR.
 *
 * A helper thread delivers the signal ~50ms into the blocking call (well inside
 * the multi-second blocking window), so the observable outcome (rc/errno) is
 * deterministic and identical on iSH and on mint regardless of scheduling.
 */
#include "sig_common.h"
#include <pthread.h>
#include <poll.h>
#include <sys/wait.h>

static volatile sig_atomic_t hits;
static void onsig(int s) { (void) s; hits++; }

static void install(int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onsig;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

static pthread_t main_thread;
struct poke { int delay_ms; int then_write_fd; };

static void *poke_thread(void *arg) {
    struct poke *p = arg;
    sig_sleep_ms(p->delay_ms);
    pthread_kill(main_thread, SIGUSR1);
    if (p->then_write_fd >= 0) {
        sig_sleep_ms(p->delay_ms);
        ssize_t w = write(p->then_write_fd, "Z", 1);
        (void) w;
    }
    return NULL;
}

int main(int argc, char **argv) {
    sig_init(argc, argv);
    main_thread = pthread_self();

    /* read() WITHOUT SA_RESTART -> EINTR */
    {
        int pp[2]; if (pipe(pp)) return 1;
        install(0);
        hits = 0;
        pthread_t t; struct poke p = { 50, -1 };
        pthread_create(&t, NULL, poke_thread, &p);
        char c; errno = 0;
        ssize_t rc = read(pp[0], &c, 1);
        int e = errno;
        pthread_join(t, NULL);
        emit_i("restart.read_norestart.rc", (int) rc);
        emit("restart.read_norestart.errno", "%s", rc < 0 ? errno_name(e) : "OK");
        close(pp[0]); close(pp[1]);
    }

    /* read() WITH SA_RESTART -> restarts, returns the late byte */
    {
        int pp[2]; if (pipe(pp)) return 1;
        install(SA_RESTART);
        hits = 0;
        pthread_t t; struct poke p = { 50, pp[1] };
        pthread_create(&t, NULL, poke_thread, &p);
        char c = 0; errno = 0;
        ssize_t rc = read(pp[0], &c, 1);
        pthread_join(t, NULL);
        emit_i("restart.read_restart.rc", (int) rc);
        emit_i("restart.read_restart.byte", c);
        emit_i("restart.read_restart.hits", hits);   /* handler still ran once */
        close(pp[0]); close(pp[1]);
    }

    /* waitpid() WITHOUT SA_RESTART -> EINTR */
    {
        pid_t kid = fork();
        if (kid == 0) { sig_sleep_ms(400); _exit(33); }
        install(0);
        pthread_t t; struct poke p = { 50, -1 };
        pthread_create(&t, NULL, poke_thread, &p);
        int st = 0; errno = 0;
        pid_t rc = waitpid(kid, &st, 0);
        int e = errno;
        pthread_join(t, NULL);
        emit_i("restart.waitpid_norestart.rc", (int) rc);
        emit("restart.waitpid_norestart.errno", "%s", rc < 0 ? errno_name(e) : "OK");
        waitpid(kid, &st, 0);   /* reap for real */
        emit_i("restart.waitpid_norestart.reaped_status", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }

    /* poll() with SA_RESTART -> still EINTR (poll never restarts) */
    {
        int pp[2]; if (pipe(pp)) return 1;
        install(SA_RESTART);
        pthread_t t; struct poke p = { 50, -1 };
        pthread_create(&t, NULL, poke_thread, &p);
        struct pollfd pfd = { .fd = pp[0], .events = POLLIN };
        errno = 0;
        int rc = poll(&pfd, 1, 10000);
        int e = errno;
        pthread_join(t, NULL);
        emit_i("restart.poll.rc", rc);
        emit("restart.poll.errno", "%s", rc < 0 ? errno_name(e) : "OK");
        close(pp[0]); close(pp[1]);
    }

    /* pause() -> EINTR after the handler (never restarts) */
    {
        install(SA_RESTART);
        hits = 0;
        pthread_t t; struct poke p = { 50, -1 };
        pthread_create(&t, NULL, poke_thread, &p);
        errno = 0;
        int rc = pause();
        int e = errno;
        pthread_join(t, NULL);
        emit_i("restart.pause.rc", rc);
        emit("restart.pause.errno", "%s", rc < 0 ? errno_name(e) : "OK");
    }

    return 0;
}
