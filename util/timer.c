#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "util/timer.h"
#include "misc.h"

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
//    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timer *timer = malloc(sizeof(struct timer));
    timer->clockid = clockid;
    timer->start = (struct timespec) {};
    timer->end = (struct timespec) {};
    timer->interval = (struct timespec) {};
    timer->callback = callback;
    timer->data = data;
    timer->active = false;
    timer->thread_running = false;
    timer->generation = 0;
    lock_init(&timer->lock, "timer_new\0");
    timer->dead = false;
    return timer;
}

void timer_free(struct timer *timer) {
    lock(&timer->lock, 0);
    timer->active = false;
    if (timer->thread_running) {
        timer->dead = true;
        pthread_kill(timer->thread, SIGUSR1);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        free(timer);
    }
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    lock(&timer->lock, 1);
    while (true) {
        uint64_t generation = timer->generation;
        struct timespec end = timer->end;
        struct timespec interval = timer->interval;
        struct timespec remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        while (timer->active &&
                timer->generation == generation &&
                timespec_positive(remaining)) {
            unlock(&timer->lock);
            nanosleep(&remaining, NULL);
            lock(&timer->lock, 0);
            remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        }
        if (!timer->active)
            break;
        if (timer->generation != generation)
            continue;

        // Only fire the callback for the arm we actually slept on. A later
        // arm/cancel updates the generation and should not inherit this wakeup.
        if (timespec_positive(timespec_subtract(timer->end, timespec_now(timer->clockid))))
            continue;

        timer->callback(timer->data);
        if (timer->generation != generation)
            continue;
        if (timer->active && timespec_positive(interval)) {
            timer->start = end;
            timer->end = timespec_add(timer->start, interval);
        } else {
            break;
        }
    }
    timer->thread_running = false;
    if (timer->dead)
        free(timer);
    else
        unlock(&timer->lock);
    return NULL;
}

int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec) {
    lock(&timer->lock, 0);
    struct timespec now = timespec_now(timer->clockid);
    if (oldspec != NULL) {
        *oldspec = (struct timer_spec) {};
        if (timer->active) {
            oldspec->value = timespec_subtract(timer->end, now);
            if (!timespec_positive(oldspec->value))
                oldspec->value = (struct timespec) {};
            oldspec->interval = timer->interval;
        }
    }

    timer->generation++;
    timer->start = now;
    timer->end = timespec_add(timer->start, spec.value);
    timer->interval = spec.interval;
    timer->active = !timespec_is_zero(spec.value);
    if (timer->thread_running) {
        pthread_kill(timer->thread, SIGUSR1);
    } else if (timer->active) {
        timer->thread_running = true;
        pthread_create(&timer->thread, NULL, timer_thread, timer);
        pthread_detach(timer->thread);
    }
    unlock(&timer->lock);
    return 0;
}
