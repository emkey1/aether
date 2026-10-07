/*
 * experiment.c -- environment switches for measurement. See experiment.h.
 */

#include "aether/experiment.h"

#include <stdlib.h>
#include <string.h>

const char *aetherDumpTypesTarget(void) {
    const char *env = getenv("AETHER_DUMP_TYPES");
    return (env && *env && strcmp(env, "0") != 0) ? env : NULL;
}
