#ifndef PSCAL_AETHER_EXPERIMENT_H
#define PSCAL_AETHER_EXPERIMENT_H

/*
 * Measurement switches read from the environment. None of them is a language
 * feature: with the variables unset the compiler behaves exactly as shipped.
 *
 * AETHER_DUMP_TYPES turns on the type oracle's dump (src/aether/types.c): "1"
 * writes it to stdout and exits after semantic analysis; any other value is a
 * file path to write it to, and compilation continues. (`--dump-types` is the
 * planned CLI spelling; it needs an option-table entry in the shared engine's
 * main.c, so it waits for the next rea release.)
 */

/* NULL when the dump is off; "1" for stdout-and-exit; otherwise a path. */
const char *aetherDumpTypesTarget(void);

#endif
