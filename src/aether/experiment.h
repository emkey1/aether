#ifndef PSCAL_AETHER_EXPERIMENT_H
#define PSCAL_AETHER_EXPERIMENT_H

#include "ast/ast.h"

/*
 * Experiment flags: measurement arms for language decisions that wait on
 * evidence (docs/aether_decisions.md). They are read once from the environment
 * variable AETHER_EXPERIMENT, a comma-separated list of key=value items:
 *
 *   div=current|int|intplus|realplus|ctx   Int/Int `/` (D4, W8-08)
 *   arrays=value|vstrict|ref               array parameters (D8, W8-09)
 *   tail=current|reject|ret                discarded values, tail exprs (D39, W4-03)
 *
 * With the variable unset or empty every flag is at its first value, which is
 * the shipped behaviour. An unknown key or value is a usage error (exit 2): a
 * census run with a typo must not quietly measure the default. Because an arm
 * changes the compiled program, run it with --no-cache (a cached bytecode file
 * from another arm would otherwise be reused).
 *
 * AETHER_EXPERIMENT_LOG=<path> appends one tab-separated row per site an arm
 * classifies (also with every flag at its default, which is how the census
 * counts sites under the shipped rule):
 *   div  <source>  <line>  <use>  <sink label>  <sink type>  <action>
 * action is int (rewritten to integer division), real (left Real), div001
 * (rejected), or unknown (an operand the oracle cannot type; left as today).
 *   arr  <source>  <line>  <param>  <write kind>  <fn kind>  <verdict>
 * one row per write to an array parameter: write kind is index, nested (an
 * element of an element), resize (setlength / append) or reassign; fn kind
 * is void or value; verdict is returned (the function returns that parameter,
 * alone or in a tuple) or lost (V-strict's error). Also
 *   arr-ref  <source>  <line>  <callee>  <param index>  -  hoisted|unhoistable
 * for every rvalue argument the `ref` arm moved into a temporary.
 *
 * AETHER_DUMP_TYPES turns on the type oracle's dump (src/aether/types.c): "1"
 * writes it to stdout and exits after semantic analysis; any other value is a
 * file path to write it to, and compilation continues. (`--dump-types` is the
 * planned CLI spelling; it needs an option-table entry in the shared engine's
 * main.c, so it waits for the next rea release.)
 *
 * None of these is a language feature. A flag either becomes the language
 * (with a VERSION bump and its own CHANGELOG entry) or is deleted once its
 * decision is recorded.
 */

typedef enum {
    AETHER_DIV_CURRENT = 0, /* `/` always Real; Int sinks truncate (D1)            */
    AETHER_DIV_INT,         /* A:   Int / Int is integer division                   */
    AETHER_DIV_INTPLUS,     /* A+:  A, plus DIV-001 where it feeds a Real sink      */
    AETHER_DIV_REALPLUS,    /* B+:  Real, plus DIV-001 at %, index, bound, Int arg  */
    AETHER_DIV_CTX          /* ctx: integer where an Int is demanded, else DIV-001  */
} AetherDivRule;

typedef enum {
    AETHER_ARRAYS_VALUE = 0, /* value copies at the call boundary; ARR-001 as shipped */
    AETHER_ARRAYS_VSTRICT,   /* V-strict in warn mode: ARR-001 at every lost write  */
    AETHER_ARRAYS_REF        /* array parameters by reference, no prologue copy     */
} AetherArraysRule;

typedef enum {
    AETHER_TAIL_CURRENT = 0, /* discarded expression statements accepted            */
    AETHER_TAIL_REJECT,      /* D39 (a): FLOW-001 / SYN-001 for a discarded value,
                              * SYN-001 for two expressions juxtaposed on a line   */
    AETHER_TAIL_RET          /* D39 (b): a final bare value is returned (Rust-style) */
} AetherTailRule;

typedef struct {
    AetherDivRule div;
    AetherArraysRule arrays;
    AetherTailRule tail;
    int any; /* nonzero when any flag is off its default */
} AetherExperiment;

const AetherExperiment *aetherExperiment(void);

/* NULL when the dump is off; "1" for stdout-and-exit; otherwise a path. */
const char *aetherDumpTypesTarget(void);

/* Runs the active arms over the analysed program (after rea's semantic pass,
 * before the type dump). A no-op with every flag at its default and no log. */
void aetherRunExperiments(AST *root);

/* The arms that must act before rea's semantic pass (arrays=ref moves rvalue
 * arguments of by-reference parameters into temporaries). */
void aetherRunExperimentsBeforeRea(AST *root);

#endif
