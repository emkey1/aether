/*
 * time_clock.c — clock_gettime(2) / clock_getres(2) conformance.
 *
 * Invariants only (no raw timestamps):
 *   - which clock ids are supported (gettime/getres return OK vs EINVAL);
 *   - REALTIME and TAI read EPOCH-based time (tv_sec is a huge number);
 *     MONOTONIC / MONOTONIC_RAW / BOOTTIME read BOOT-relative time (small,
 *     and strictly less than REALTIME). This structural distinction catches a
 *     clock that is silently aliased to the wrong base (e.g. TAI -> MONOTONIC);
 *   - MONOTONIC never goes backward across two reads;
 *   - BOOTTIME >= MONOTONIC (boottime also counts suspend);
 *   - resolution is a sane, sub-second positive value, equal for the two fine
 *     clocks (we do NOT assert the exact nanosecond value — that is host- and
 *     CONFIG_HZ-dependent, the masked part).
 */
#include "time_common.h"

/* tv_sec larger than this (2017-07-14) means epoch-based wall-clock time. A
 * boot-relative clock on any plausible test machine is far below it. */
#define EPOCH_FLOOR 1500000000LL

struct clk { int id; const char *name; };
static const struct clk CLOCKS[] = {
    { CLK_REALTIME,         "realtime" },
    { CLK_MONOTONIC,        "monotonic" },
    { CLK_MONOTONIC_RAW,    "monotonic_raw" },
    { CLK_BOOTTIME,         "boottime" },
    { CLK_PROCESS_CPUTIME,  "process_cputime" },
    { CLK_THREAD_CPUTIME,   "thread_cputime" },
    { CLK_REALTIME_COARSE,  "realtime_coarse" },
    { CLK_MONOTONIC_COARSE, "monotonic_coarse" },
    { CLK_TAI,              "tai" },
};

static int read_ok(int id, struct timespec *ts) {
    memset(ts, 0, sizeof *ts);
    return clk_now(id, ts) == 0;
}

/* support + field validity for every clock id */
static void case_supported(void) {
    for (size_t i = 0; i < sizeof CLOCKS / sizeof CLOCKS[0]; i++) {
        struct timespec ts;
        errno = 0;
        int r = clk_now(CLOCKS[i].id, &ts);
        char key[64];
        snprintf(key, sizeof key, "clock.%s.gettime", CLOCKS[i].name);
        emit(key, "%s", res_errno(r));
        if (r == 0) {
            snprintf(key, sizeof key, "clock.%s.nsec_valid", CLOCKS[i].name);
            emit_b(key, ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L);
        }
    }
}

/* epoch- vs boot-relative base — the structural property that distinguishes
 * the wall clocks from the uptime clocks. */
static void case_epoch_vs_boot(void) {
    struct timespec rt, tai, mono, mraw, boot;
    int have_rt = read_ok(CLK_REALTIME, &rt);
    int have_tai = read_ok(CLK_TAI, &tai);
    int have_mono = read_ok(CLK_MONOTONIC, &mono);
    int have_mraw = read_ok(CLK_MONOTONIC_RAW, &mraw);
    int have_boot = read_ok(CLK_BOOTTIME, &boot);

    if (have_rt)   emit_b("clock.realtime.is_epoch", rt.tv_sec   > EPOCH_FLOOR);
    if (have_tai)  emit_b("clock.tai.is_epoch",      tai.tv_sec  > EPOCH_FLOOR);
    if (have_mono) emit_b("clock.monotonic.is_boot", mono.tv_sec < EPOCH_FLOOR);
    if (have_mraw) emit_b("clock.monotonic_raw.is_boot", mraw.tv_sec < EPOCH_FLOOR);
    if (have_boot) emit_b("clock.boottime.is_boot",  boot.tv_sec < EPOCH_FLOOR);

    /* TAI is within a couple of minutes of REALTIME (leap-second offset, ~37s,
     * far under 600s); a clock aliased to MONOTONIC would be decades off. */
    if (have_rt && have_tai) {
        int64_t d = ts_to_ns(tai) - ts_to_ns(rt);
        if (d < 0) d = -d;
        emit_b("clock.tai.near_realtime", d < 600LL * 1000000000LL);
    }
    /* MONOTONIC is uptime: strictly less than epoch wall time. */
    if (have_rt && have_mono)
        emit_b("clock.monotonic.lt_realtime", ts_to_ns(mono) < ts_to_ns(rt));
}

/* MONOTONIC and MONOTONIC_RAW never decrease; BOOTTIME >= MONOTONIC */
static void case_monotonic(void) {
    struct timespec a, b;
    read_ok(CLK_MONOTONIC, &a);
    for (volatile int i = 0; i < 1000; i++) { }
    read_ok(CLK_MONOTONIC, &b);
    emit_b("clock.monotonic.nondecreasing", ts_to_ns(b) >= ts_to_ns(a));

    read_ok(CLK_MONOTONIC_RAW, &a);
    for (volatile int i = 0; i < 1000; i++) { }
    read_ok(CLK_MONOTONIC_RAW, &b);
    emit_b("clock.monotonic_raw.nondecreasing", ts_to_ns(b) >= ts_to_ns(a));

    struct timespec mono, boot;
    if (read_ok(CLK_MONOTONIC, &mono) && read_ok(CLK_BOOTTIME, &boot))
        emit_b("clock.boottime.ge_monotonic", ts_to_ns(boot) >= ts_to_ns(mono));
}

/* process/thread CPU time is non-negative and never decreases */
static void case_cputime(void) {
    struct timespec a, b;
    if (read_ok(CLK_PROCESS_CPUTIME, &a)) {
        emit_b("clock.process_cputime.nonneg", ts_to_ns(a) >= 0);
        for (volatile long i = 0; i < 2000000; i++) { }
        read_ok(CLK_PROCESS_CPUTIME, &b);
        emit_b("clock.process_cputime.nondecreasing", ts_to_ns(b) >= ts_to_ns(a));
    }
    if (read_ok(CLK_THREAD_CPUTIME, &a)) {
        emit_b("clock.thread_cputime.nonneg", ts_to_ns(a) >= 0);
        for (volatile long i = 0; i < 2000000; i++) { }
        read_ok(CLK_THREAD_CPUTIME, &b);
        emit_b("clock.thread_cputime.nondecreasing", ts_to_ns(b) >= ts_to_ns(a));
    }
}

/* clock_getres: positive, sub-second; the two fine clocks share a resolution */
static void case_getres(void) {
    struct timespec res;
    for (size_t i = 0; i < sizeof CLOCKS / sizeof CLOCKS[0]; i++) {
        errno = 0;
        int r = clock_getres((clockid_t) CLOCKS[i].id, &res);
        char key[64];
        snprintf(key, sizeof key, "clock.%s.getres", CLOCKS[i].name);
        emit(key, "%s", res_errno(r));
        if (r == 0) {
            snprintf(key, sizeof key, "clock.%s.res_valid", CLOCKS[i].name);
            emit_b(key, res.tv_sec == 0 && res.tv_nsec > 0 && res.tv_nsec < 1000000000L);
        }
    }
    struct timespec rr, rm;
    if (clock_getres(CLK_REALTIME, &rr) == 0 && clock_getres(CLK_MONOTONIC, &rm) == 0)
        emit_b("clock.res_realtime_eq_monotonic", ts_to_ns(rr) == ts_to_ns(rm));
}

/* unknown clock ids -> EINVAL on both gettime and getres.
 * NOTE: we deliberately do NOT probe NEGATIVE clock ids: Linux decodes a
 * negative clockid as a "dynamic" / per-process-CPU clock (the pid is packed
 * into the high bits), so e.g. clock_gettime(-7) returns OK on real Linux
 * rather than EINVAL. That is Linux-internal encoding iSH does not implement;
 * masking it per the "assert only spec-guaranteed invariants" discipline. */
static void case_bad_id(void) {
    struct timespec ts;
    errno = 0; emit("clock.badid99.gettime", "%s", res_errno(clk_now(99, &ts)));
    errno = 0; emit("clock.badidbig.gettime", "%s", res_errno(clk_now(1000000, &ts)));
    errno = 0; emit("clock.badid99.getres", "%s", res_errno(clock_getres((clockid_t) 99, &ts)));
}

int main(int argc, char **argv) {
    time_init(argc, argv);
    case_supported();
    case_epoch_vs_boot();
    case_monotonic();
    case_cputime();
    case_getres();
    case_bad_id();
    return 0;
}
