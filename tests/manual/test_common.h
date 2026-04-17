#ifndef ISH_TESTS_COMMON_H
#define ISH_TESTS_COMMON_H

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures_total;
static int test_verbose;

static void test_init(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            test_verbose = 1;
            continue;
        }
        fprintf(stderr, "unknown option: %s\n", argv[i]);
        exit(2);
    }
}

static void test_logf(const char *fmt, ...) {
    if (!test_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void test_log_if(int cond, const char *fmt, ...) {
    if (!cond && !test_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void failf(const char *label, uint64_t got0, uint64_t got1, uint64_t got2,
                  uint64_t exp0, uint64_t exp1, uint64_t exp2) {
    printf("FAIL %s got=%016" PRIx64 " %016" PRIx64 " %016" PRIx64
           " expected=%016" PRIx64 " %016" PRIx64 " %016" PRIx64 "\n",
           label, got0, got1, got2, exp0, exp1, exp2);
    failures_total++;
}

static int finish_suite(const char *suite_name) {
    if (failures_total != 0) {
        printf("%s: FAIL failures=%u\n", suite_name, failures_total);
        return 1;
    }
    printf("%s: PASS\n", suite_name);
    return 0;
}

#endif
