#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "atomic_common.h"

enum { THREADS = 4, LOOPS = 100000 };
enum { VECTOR_LOOPS = 2000 };

static volatile uint32_t shared_cmpxchg;

struct worker_result {
    uint32_t retries;
};

static inline int lock_cmpxchg_u32(volatile uint32_t *ptr, uint32_t expected,
                                   uint32_t desired, uint32_t *actual_out) {
    unsigned long eflags;
    asm volatile(
        "lock cmpxchgl %4, %1\n\t"
        "pushf\n\t"
        "pop %2\n\t"
        : "=a"(expected), "+m"(*ptr), "=r"(eflags)
        : "0"(expected), "r"(desired)
        : "cc", "memory");
    *actual_out = expected;
    return (eflags & CC_Z) != 0;
}

static void test_lock_cmpxchgl_single(uint32_t eax_init) {
    volatile uint32_t mem = 0xfbca7654u;
    uint32_t eax = eax_init;
    uint32_t src = 0x12345678u;
    unsigned long eflags;
    uint32_t expected_result = eax_init - mem;
    uint32_t expected_flags = sub_flags32(eax_init, mem, expected_result);
    uint32_t expected_mem = eax_init == mem ? src : mem;
    uint32_t expected_eax = eax_init == mem ? eax_init : mem;

    asm volatile(
        "lock cmpxchgl %4, %1\n\t"
        "pushf\n\t"
        "pop %2\n\t"
        : "=a"(eax), "+m"(mem), "=r"(eflags)
        : "0"(eax), "r"(src)
        : "cc", "memory");

    test_logf("lock cmpxchgl eax=%08" PRIx32 ": eax=%08" PRIx32 " mem=%08" PRIx32 " flags=%03" PRIx32 "\n",
              eax_init, eax, mem, (uint32_t) (eflags & CC_MASK));

    if (eax != expected_eax || mem != expected_mem || (eflags & CC_MASK) != expected_flags)
        failf("lock cmpxchgl single", eax, mem, (uint32_t) (eflags & CC_MASK),
              expected_eax, expected_mem, expected_flags);
}

static void test_cmpxchg_jcc_single(uint32_t eax_init, uint32_t initial, uint32_t desired) {
    volatile uint32_t mem = initial;
    uint32_t eax = eax_init;
    uint32_t branch = 0;

    asm volatile(
        "lock cmpxchgl %3, %2\n\t"
        "jnz 1f\n\t"
        "movl $1, %0\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "movl $0, %0\n"
        "2:\n\t"
        : "=r"(branch), "+a"(eax), "+m"(mem)
        : "r"(desired)
        : "cc", "memory");

    test_logf("lock cmpxchgl jz eax=%08" PRIx32 " mem0=%08" PRIx32
              " -> branch=%" PRIu32 " eax=%08" PRIx32 " mem=%08" PRIx32 "\n",
              eax_init, initial, branch, eax, mem);

    if (branch != (eax_init == initial) || eax != (eax_init == initial ? eax_init : initial) ||
        mem != (eax_init == initial ? desired : initial))
        failf("lock cmpxchgl jcc", branch, eax, mem,
              eax_init == initial, eax_init == initial ? eax_init : initial,
              eax_init == initial ? desired : initial);
}

static void test_lock_cmpxchgl_vectors(void) {
    uint32_t seed = 0x2468ace1u;
    unsigned failures = 0;

    for (unsigned i = 0; i < VECTOR_LOOPS; i++) {
        volatile uint32_t mem = next_u32(&seed);
        uint32_t initial_mem = mem;
        uint32_t eax = next_u32(&seed);
        uint32_t initial_eax = eax;
        uint32_t src = next_u32(&seed);
        unsigned long eflags;
        uint32_t result = initial_eax - initial_mem;
        uint32_t expected_flags = sub_flags32(initial_eax, initial_mem, result);
        uint32_t expected_mem = initial_eax == initial_mem ? src : initial_mem;
        uint32_t expected_eax = initial_eax == initial_mem ? initial_eax : initial_mem;

        asm volatile(
            "lock cmpxchgl %4, %1\n\t"
            "pushf\n\t"
            "pop %2\n\t"
            : "=a"(eax), "+m"(mem), "=r"(eflags)
            : "0"(eax), "r"(src)
            : "cc", "memory");

        if (eax != expected_eax || mem != expected_mem || (eflags & CC_MASK) != expected_flags)
            failures++;
    }

    test_log_if(failures != 0, "lock cmpxchgl vectors: loops=%u failures=%u\n", VECTOR_LOOPS, failures);
    failures_total += failures;
}

static void *cmpxchg_worker(void *arg) {
    struct worker_result *result = arg;
    uint32_t retries = 0;
    for (unsigned i = 0; i < LOOPS; i++) {
        for (;;) {
            uint32_t old = shared_cmpxchg;
            uint32_t actual;
            if (lock_cmpxchg_u32(&shared_cmpxchg, old, old + 1, &actual))
                break;
            retries++;
        }
    }
    result->retries = retries;
    return NULL;
}

static void run_cmpxchg_stress(void) {
    pthread_t threads[THREADS];
    struct worker_result results[THREADS];
    uint32_t total_retries = 0;
    uint32_t expected = THREADS * LOOPS;

    shared_cmpxchg = 0;
    for (unsigned i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, cmpxchg_worker, &results[i]);
    for (unsigned i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_retries += results[i].retries;
    }

    int failed = shared_cmpxchg != expected;
    test_log_if(failed, "lock cmpxchgl stress: final=%08" PRIx32 " expected=%08" PRIx32 " retries=%" PRIu32 "\n",
                shared_cmpxchg, expected, total_retries);

    if (shared_cmpxchg != expected)
        failf("lock cmpxchgl stress", shared_cmpxchg, total_retries, 0, expected, total_retries, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_lock_cmpxchgl_single(0xfbca7654u);
    test_lock_cmpxchgl_single(0xfffefdfcu);
    test_cmpxchg_jcc_single(0xfbca7654u, 0xfbca7654u, 0x12345678u);
    test_cmpxchg_jcc_single(0xfffefdfcu, 0xfbca7654u, 0x12345678u);
    test_lock_cmpxchgl_vectors();
    run_cmpxchg_stress();
    return finish_suite("atomic_cmpxchg32");
}
