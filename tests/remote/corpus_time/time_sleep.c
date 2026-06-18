/*
 * time_sleep.c — nanosleep(2) / clock_nanosleep(2) conformance.
 *
 * Invariants:
 *   - a completed sleep lasts AT LEAST the requested duration (measured with
 *     CLOCK_MONOTONIC, so it is wall-clock independent);
 *   - success returns 0;
 *   - a signal interrupts the sleep -> EINTR, and the remaining time written
 *     back is positive and no larger than the request;
 *   - TIMER_ABSTIME sleeps until the absolute deadline; an already-past
 *     absolute deadline returns 0 immediately;
 *   - bad arguments: nsec out of range -> EINVAL; bad clock id -> EINVAL;
 *     unknown flags -> EINVAL; CLOCK_THREAD_CPUTIME_ID is not permitted ->
 *     EINVAL (man clock_nanosleep).
 */
#include "time_common.h"

static volatile sig_atomic_t got_usr1;
static void on_usr1(int s) { (void) s; got_usr1 = 1; }

static void install_usr1(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;            /* no SA_RESTART: the sleep must EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

/* fork a child that pokes us with SIGUSR1 after `ms`, so a blocking sleep in
 * the parent is interrupted deterministically without relying on timers. */
static pid_t spawn_poker(int ms) {
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec d = ms_ts(ms);
        nanosleep(&d, NULL);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    return pid;
}

static void reap(pid_t pid) { int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }

/* relative nanosleep of 60ms lasts >= 60ms and returns 0 */
static void case_relative(void) {
    struct timespec a, b, req = ms_ts(60);
    clk_now(CLK_MONOTONIC, &a);
    int r = nanosleep(&req, NULL);
    clk_now(CLK_MONOTONIC, &b);
    emit_i("nanosleep.relative.ret", r);
    emit_b("nanosleep.relative.slept_enough", elapsed_ns(a, b) >= 60000000LL);
}

/* a signal interrupts nanosleep -> EINTR, rem in (0, req] */
static void case_eintr(void) {
    got_usr1 = 0;
    pid_t poker = spawn_poker(60);
    struct timespec req = ms_ts(1000), rem = {0, 0};
    errno = 0;
    int r = nanosleep(&req, &rem);
    int saved = errno;
    emit("nanosleep.eintr.ret", "%s", r < 0 ? errno_name(saved) : "OK");
    emit_b("nanosleep.eintr.handler_ran", got_usr1);
    if (r < 0 && saved == EINTR) {
        int64_t rem_ns = ts_to_ns(rem);
        emit_b("nanosleep.eintr.rem_in_range", rem_ns > 0 && rem_ns <= 1000000000LL);
    }
    reap(poker);
}

/* clock_nanosleep relative on MONOTONIC: >= requested, returns 0 */
static void case_clock_relative(void) {
    struct timespec a, b, req = ms_ts(60);
    clk_now(CLK_MONOTONIC, &a);
    int r = clock_nanosleep(CLK_MONOTONIC, 0, &req, NULL);
    clk_now(CLK_MONOTONIC, &b);
    emit_i("clock_nanosleep.relative.ret", r);   /* clock_nanosleep returns errno, not -1 */
    emit_b("clock_nanosleep.relative.slept_enough", elapsed_ns(a, b) >= 60000000LL);
}

/* TIMER_ABSTIME: sleep until now+60ms (>=60ms elapse); a past deadline -> 0 now */
static void case_abstime(void) {
    struct timespec start, deadline, end;
    clk_now(CLK_MONOTONIC, &start);
    deadline = start;
    deadline.tv_nsec += 60000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
    int r = clock_nanosleep(CLK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    clk_now(CLK_MONOTONIC, &end);
    emit_i("clock_nanosleep.abstime.ret", r);
    emit_b("clock_nanosleep.abstime.slept_until", elapsed_ns(start, end) >= 60000000LL);

    struct timespec past;
    clk_now(CLK_MONOTONIC, &past);
    if (past.tv_sec > 1) past.tv_sec -= 1;        /* a deadline 1s in the past */
    clk_now(CLK_MONOTONIC, &start);
    r = clock_nanosleep(CLK_MONOTONIC, TIMER_ABSTIME, &past, NULL);
    clk_now(CLK_MONOTONIC, &end);
    emit_i("clock_nanosleep.abstime.past_ret", r);
    emit_b("clock_nanosleep.abstime.past_immediate", elapsed_ns(start, end) < 40000000LL);
}

/* invalid arguments */
static void case_invalid(void) {
    struct timespec bad_nsec = { .tv_sec = 0, .tv_nsec = 1000000000L };  /* == 1e9, invalid */
    errno = 0;
    int r = nanosleep(&bad_nsec, NULL);
    emit("nanosleep.badnsec", "%s", r < 0 ? errno_name(errno) : "OK");

    /* clock_nanosleep returns the errno directly (0 on success) */
    emit("clock_nanosleep.badnsec", "%s", errno_name(clock_nanosleep(CLK_MONOTONIC, 0, &bad_nsec, NULL)));

    struct timespec ok = ms_ts(1);
    emit("clock_nanosleep.badclock", "%s", errno_name(clock_nanosleep(99, 0, &ok, NULL)));
    /* NOTE: we do NOT assert clock_nanosleep flag validation. The man page
     * documents EINVAL for unknown flags, but real Linux (verified on mint,
     * both arches) silently IGNORES unknown flag bits and returns 0. iSH's
     * stricter EINVAL matches the documented contract; masking per the
     * cmpxchg discipline (oracle disagrees with its own docs). */

    /* CLOCK_THREAD_CPUTIME_ID is explicitly not permitted for clock_nanosleep */
    emit("clock_nanosleep.thread_cputime",
         "%s", errno_name(clock_nanosleep(CLK_THREAD_CPUTIME, 0, &ok, NULL)));
}

int main(int argc, char **argv) {
    time_init(argc, argv);
    install_usr1();
    case_relative();
    case_eintr();
    case_clock_relative();
    case_abstime();
    case_invalid();
    return 0;
}
