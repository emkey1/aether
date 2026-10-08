/*
 * ast_parser.c -- the Aether recursive-descent parser.
 *
 * This is the only Aether frontend. It replaced the line-based text rewriter
 * (translate.c), which was retired on 2026-07-01 after the P7 cutover; see
 * docs/parser_roadmap.md for the history.
 *
 * The parser tokenizes Aether source with Rea's lexer and emits the *shared
 * pscal AST* directly -- the same node shapes Rea's own parser builds -- so
 * semantic analysis, codegen and the VM are untouched. Every node carries the
 * true source line (taken straight from the lexer token), which is the whole
 * point of this rewrite: diagnostics report the real line, not a rewrite-offset
 * line.
 *
 * Node shapes are mirrored from rea's src/rea/parser.c (the canonical AST
 * producer). Aether's surface syntax differs from Rea's, but the AST it lowers
 * to has the same shapes, so we reproduce rea's node construction and only
 * change how the tokens are recognized:
 *
 *   - Aether keywords (fn, ret, fx, loop, let, const) are NOT Rea keywords, so
 *     the lexer hands them back as REA_TOKEN_IDENTIFIER; we match them by text.
 *   - Aether type names (Int/Real/Text/Bool/Void) arrive as identifiers too;
 *     we map them to the Rea keyword-name + VarType (Int->"int"/INT64,
 *     Real->"float"/DOUBLE, Text->"str"/UNICODE_STRING, Bool->"bool"/BOOLEAN,
 *     Void->VOID).
 *
 * The desugarings live in ast_lower.c, type-name inference in ast_types.c and
 * the post-parse checks in ast_checks.c; ast_internal.h holds the shared
 * AetherParser state and prototypes.
 *
 * There is no grammar in this file. The grammar is docs/aether_spec.md, whose
 * examples run in CI (CTest aether_spec), together with the fixtures in tests/
 * and the guide snippet gates; a comment here that disagrees with them is the
 * thing that is wrong.
 */

#include "aether/parser.h"

#include "backend_ast/builtin.h"   /* getVmBuiltinID: does this builtin exist at all? */

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

#include "ast/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/globals.h"
#include "core/type_registry.h"
#include "symbol/symbol.h"
#include "rea/lexer.h"
#include "rea/frontend_hooks.h"
#include "aether/parser.h"
#include "aether/semantic.h"
#include "aether/diagnostics.h"
#include "aether/ast_prepasses.h"
#include "aether/experiment.h"
#include "aether/ast_internal.h"

/* Count of user-facing diagnostics written to stderr during the current parse.
 * Every parser diagnostic funnels through aetherDiagf (all `fprintf(stderr, ...)`
 * sites in this file were mechanically routed to it), so a nonzero count means the
 * user already has something to react to. parseAetherAst resets this to 0 just
 * before the authoritative parse loop (the muted forward-declaration pre-pass runs
 * first and its increments must not count), then, if it bails with p.hadError but
 * this count is still 0, emits a backstop [SYN-001] so a parse failure is NEVER
 * silent (exit 1 with empty stderr + empty --diagnostics-json was the worst case
 * for both humans and the LLM repair loop -- nothing to react to). */
static int g_aetherAstDiagCount = 0;

/* stderr diagnostic sink: identical to fprintf(stderr, ...) but bumps the
 * emitted-diagnostic counter so the silent-failure backstop can tell whether the
 * real parse pass said anything. Muting (aetherMuteStderr) still discards the text
 * of the forward-declaration pre-pass; the count from that pass is discarded by the
 * reset in parseAetherAst. */
int aetherDiagf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stderr, fmt, ap);
    va_end(ap);
    g_aetherAstDiagCount++;
    return r;
}

/* ------------------------------------------------------------------ */
/* Semantic side-registries (see parser.h)                             */
/*                                                                     */
/* The parser lowers `fx { ... }` to a plain AST_COMPOUND, drops @pure */
/* (annotation with no codegen), and canonicalizes builtin aliases     */
/* (println -> writeln). These registries retain those three facts so  */
/* the semantic pass can enforce the effect fence (FX-001) and purity  */
/* (ANN-001) on the real AST. Entries accumulate across parses (main   */
/* program + imported modules) and are cleared only via                */
/* aetherAstClearSemanticRegistries() from aetherInvalidateGlobalState.*/
/* ------------------------------------------------------------------ */

typedef struct {
    const AST *node;
    int line; /* line of the `fx` keyword, for diagnostics */
} AetherFxBlockEntry;

typedef struct {
    AetherFxBlockEntry *items;
    size_t count;
    size_t cap;
} AetherNodePtrSet;

typedef struct {
    char *name;
    int isPure;
    /* Declared return type is a Real flavour. Recorded here rather than read
     * off the call node because an AST_PROCEDURE_CALL still carries
     * var_type 0 through semantic analysis -- the JSON dump's type
     * annotations are produced by a later stage. Used by NARROW-001. */
    int returnsReal;
} AetherFnPurityEntry;

typedef struct {
    const AST *node;
    char *surface;
} AetherCallSurfaceEntry;

static AetherNodePtrSet g_aetherFxBlocks;
static struct { AetherFnPurityEntry *items; size_t count; size_t cap; } g_aetherFnPurity;
static struct { char **items; size_t count; size_t cap; } g_aetherTopLevelFns;
static struct { AetherCallSurfaceEntry *items; size_t count; size_t cap; } g_aetherCallSurfaces;
static struct { const AST **items; size_t count; size_t cap; } g_aetherSynthesized;
/* AST_VAR_DECL nodes whose type the author actually wrote (`let x: Int = ...`),
 * as opposed to an inferred `let x = ...`. NARROW-001 needs the distinction and
 * cannot get it from the node: an inferred binding whose initializer mentions
 * variables still carries the wrong var_type during semantic analysis, so it
 * looks Int-declared when it will in fact resolve to Real. Only a declaration
 * the author typed can contradict its initializer. */
static struct { const AST **items; size_t count; size_t cap; } g_aetherExplicitTypedDecls;
/* Upper-bound expressions of `loop i in a..b`. Lowering turns the bound into
 * the right operand of the loop's `i < b` test, where it reads like any other
 * comparison; the type oracle (types.c) needs to tell the two apart. */
static struct { const AST **items; size_t count; size_t cap; } g_aetherRangeBounds;

void aetherAstRegisterFxBlock(const AST *block, int line) {
    if (!block) return;
    if (aetherAstNodeIsFxBlock(block)) return;
    if (g_aetherFxBlocks.count == g_aetherFxBlocks.cap) {
        size_t newCap = g_aetherFxBlocks.cap ? g_aetherFxBlocks.cap * 2 : 16;
        AetherFxBlockEntry *grown = (AetherFxBlockEntry *)realloc(
            g_aetherFxBlocks.items, newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherFxBlocks.items = grown;
        g_aetherFxBlocks.cap = newCap;
    }
    g_aetherFxBlocks.items[g_aetherFxBlocks.count].node = block;
    g_aetherFxBlocks.items[g_aetherFxBlocks.count].line = line;
    g_aetherFxBlocks.count++;
}

int aetherAstNodeIsFxBlock(const AST *node) {
    if (!node) return 0;
    for (size_t i = 0; i < g_aetherFxBlocks.count; i++) {
        if (g_aetherFxBlocks.items[i].node == node) return 1;
    }
    return 0;
}

int aetherAstFxBlockLine(const AST *node) {
    if (!node) return 0;
    for (size_t i = 0; i < g_aetherFxBlocks.count; i++) {
        if (g_aetherFxBlocks.items[i].node == node) return g_aetherFxBlocks.items[i].line;
    }
    return 0;
}

void aetherAstRegisterFunctionPurity(const char *name, int isPure) {
    if (!name || !*name) return;
    for (size_t i = 0; i < g_aetherFnPurity.count; i++) {
        if (strcmp(g_aetherFnPurity.items[i].name, name) == 0) {
            /* Re-registration (forward-scan pre-pass + real pass): keep the
             * latest annotation state. */
            g_aetherFnPurity.items[i].isPure = isPure;
            return;
        }
    }
    if (g_aetherFnPurity.count == g_aetherFnPurity.cap) {
        size_t newCap = g_aetherFnPurity.cap ? g_aetherFnPurity.cap * 2 : 16;
        AetherFnPurityEntry *grown = (AetherFnPurityEntry *)realloc(
            g_aetherFnPurity.items, newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherFnPurity.items = grown;
        g_aetherFnPurity.cap = newCap;
    }
    {
        char *copy = strdup(name);
        if (!copy) return;
        g_aetherFnPurity.items[g_aetherFnPurity.count].name = copy;
        g_aetherFnPurity.items[g_aetherFnPurity.count].isPure = isPure;
        g_aetherFnPurity.items[g_aetherFnPurity.count].returnsReal = 0;
        g_aetherFnPurity.count++;
    }
}

/* Kept separate from aetherAstRegisterFunctionPurity so that function's two
 * existing call sites (mangled `Type.method` and bare method name) keep their
 * signature; this reuses whichever entry they just created. */
void aetherAstRegisterFunctionReturnsReal(const char *name, int returnsReal) {
    if (!name || !*name) return;
    for (size_t i = 0; i < g_aetherFnPurity.count; i++) {
        if (strcmp(g_aetherFnPurity.items[i].name, name) == 0) {
            g_aetherFnPurity.items[i].returnsReal = returnsReal;
            return;
        }
    }
    aetherAstRegisterFunctionPurity(name, 0);
    for (size_t i = 0; i < g_aetherFnPurity.count; i++) {
        if (strcmp(g_aetherFnPurity.items[i].name, name) == 0) {
            g_aetherFnPurity.items[i].returnsReal = returnsReal;
            return;
        }
    }
}

int aetherAstLookupFunctionReturnsReal(const char *name, int *returnsReal) {
    if (!name) return 0;
    for (size_t i = 0; i < g_aetherFnPurity.count; i++) {
        if (strcmp(g_aetherFnPurity.items[i].name, name) == 0) {
            if (returnsReal) *returnsReal = g_aetherFnPurity.items[i].returnsReal;
            return 1;
        }
    }
    return 0;
}

int aetherAstLookupFunctionPurity(const char *name, int *isPure) {
    if (!name) return 0;
    for (size_t i = 0; i < g_aetherFnPurity.count; i++) {
        if (strcmp(g_aetherFnPurity.items[i].name, name) == 0) {
            if (isPure) *isPure = g_aetherFnPurity.items[i].isPure;
            return 1;
        }
    }
    return 0;
}

void aetherAstRegisterTopLevelFunction(const char *name) {
    if (!name || !*name) return;
    for (size_t i = 0; i < g_aetherTopLevelFns.count; i++) {
        if (strcmp(g_aetherTopLevelFns.items[i], name) == 0) return;
    }
    if (g_aetherTopLevelFns.count == g_aetherTopLevelFns.cap) {
        size_t newCap = g_aetherTopLevelFns.cap ? g_aetherTopLevelFns.cap * 2 : 16;
        char **grown = (char **)realloc(g_aetherTopLevelFns.items, newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherTopLevelFns.items = grown;
        g_aetherTopLevelFns.cap = newCap;
    }
    {
        char *copy = strdup(name);
        if (!copy) return;
        g_aetherTopLevelFns.items[g_aetherTopLevelFns.count++] = copy;
    }
}

int aetherAstIsTopLevelUserFunction(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < g_aetherTopLevelFns.count; i++) {
        if (strcmp(g_aetherTopLevelFns.items[i], name) == 0) return 1;
    }
    return 0;
}

void aetherAstRegisterCallSurfaceName(const AST *call, const char *surfaceName) {
    if (!call || !surfaceName || !*surfaceName) return;
    if (g_aetherCallSurfaces.count == g_aetherCallSurfaces.cap) {
        size_t newCap = g_aetherCallSurfaces.cap ? g_aetherCallSurfaces.cap * 2 : 16;
        AetherCallSurfaceEntry *grown = (AetherCallSurfaceEntry *)realloc(
            g_aetherCallSurfaces.items, newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherCallSurfaces.items = grown;
        g_aetherCallSurfaces.cap = newCap;
    }
    {
        char *copy = strdup(surfaceName);
        if (!copy) return;
        g_aetherCallSurfaces.items[g_aetherCallSurfaces.count].node = call;
        g_aetherCallSurfaces.items[g_aetherCallSurfaces.count].surface = copy;
        g_aetherCallSurfaces.count++;
    }
}

const char *aetherAstCallSurfaceName(const AST *call) {
    if (!call) return NULL;
    for (size_t i = 0; i < g_aetherCallSurfaces.count; i++) {
        if (g_aetherCallSurfaces.items[i].node == call) {
            return g_aetherCallSurfaces.items[i].surface;
        }
    }
    return NULL;
}

typedef struct {
    int line;
    char *canonical;
    char *surface;
} AetherLineAliasEntry;

static struct { AetherLineAliasEntry *items; size_t count; size_t cap; } g_aetherLineAliases;

void aetherAstRegisterAliasAtLine(int line, const char *canonical, const char *surface) {
    if (line <= 0 || !canonical || !*canonical || !surface || !*surface) return;
    if (g_aetherLineAliases.count == g_aetherLineAliases.cap) {
        size_t newCap = g_aetherLineAliases.cap ? g_aetherLineAliases.cap * 2 : 16;
        AetherLineAliasEntry *grown = (AetherLineAliasEntry *)realloc(
            g_aetherLineAliases.items, newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherLineAliases.items = grown;
        g_aetherLineAliases.cap = newCap;
    }
    {
        char *canonCopy = strdup(canonical);
        char *surfCopy = strdup(surface);
        if (!canonCopy || !surfCopy) {
            free(canonCopy);
            free(surfCopy);
            return;
        }
        g_aetherLineAliases.items[g_aetherLineAliases.count].line = line;
        g_aetherLineAliases.items[g_aetherLineAliases.count].canonical = canonCopy;
        g_aetherLineAliases.items[g_aetherLineAliases.count].surface = surfCopy;
        g_aetherLineAliases.count++;
    }
}

const char *aetherAstAliasSurfaceAtLine(int line, const char *canonical) {
    if (line <= 0 || !canonical) return NULL;
    for (size_t i = 0; i < g_aetherLineAliases.count; i++) {
        if (g_aetherLineAliases.items[i].line == line &&
            strcasecmp(g_aetherLineAliases.items[i].canonical, canonical) == 0) {
            return g_aetherLineAliases.items[i].surface;
        }
    }
    return NULL;
}

void aetherAstRegisterSynthesizedSubtree(const AST *node) {
    if (!node) return;
    if (aetherAstNodeIsSynthesizedSubtree(node)) return;
    if (g_aetherSynthesized.count == g_aetherSynthesized.cap) {
        size_t newCap = g_aetherSynthesized.cap ? g_aetherSynthesized.cap * 2 : 16;
        const AST **grown = (const AST **)realloc((void *)g_aetherSynthesized.items,
                                                  newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherSynthesized.items = grown;
        g_aetherSynthesized.cap = newCap;
    }
    g_aetherSynthesized.items[g_aetherSynthesized.count++] = node;
}

void aetherAstRegisterExplicitTypedDecl(const AST *node) {
    if (!node) return;
    if (aetherAstDeclHasExplicitType(node)) return;
    if (g_aetherExplicitTypedDecls.count == g_aetherExplicitTypedDecls.cap) {
        size_t newCap = g_aetherExplicitTypedDecls.cap ? g_aetherExplicitTypedDecls.cap * 2 : 16;
        const AST **grown = (const AST **)realloc((void *)g_aetherExplicitTypedDecls.items,
                                                  newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherExplicitTypedDecls.items = grown;
        g_aetherExplicitTypedDecls.cap = newCap;
    }
    g_aetherExplicitTypedDecls.items[g_aetherExplicitTypedDecls.count++] = node;
}

int aetherAstDeclHasExplicitType(const AST *node) {
    if (!node) return 0;
    for (size_t i = 0; i < g_aetherExplicitTypedDecls.count; i++) {
        if (g_aetherExplicitTypedDecls.items[i] == node) return 1;
    }
    return 0;
}

void aetherAstRegisterRangeBound(const AST *node) {
    if (!node) return;
    if (g_aetherRangeBounds.count == g_aetherRangeBounds.cap) {
        size_t newCap = g_aetherRangeBounds.cap ? g_aetherRangeBounds.cap * 2 : 16;
        const AST **grown = (const AST **)realloc((void *)g_aetherRangeBounds.items,
                                                  newCap * sizeof(*grown));
        if (!grown) return;
        g_aetherRangeBounds.items = grown;
        g_aetherRangeBounds.cap = newCap;
    }
    g_aetherRangeBounds.items[g_aetherRangeBounds.count++] = node;
}

int aetherAstIsRangeBound(const AST *node) {
    if (!node) return 0;
    for (size_t i = 0; i < g_aetherRangeBounds.count; i++) {
        if (g_aetherRangeBounds.items[i] == node) return 1;
    }
    return 0;
}

int aetherAstNodeIsSynthesizedSubtree(const AST *node) {
    if (!node) return 0;
    for (size_t i = 0; i < g_aetherSynthesized.count; i++) {
        if (g_aetherSynthesized.items[i] == node) return 1;
    }
    return 0;
}

void aetherAstClearSemanticRegistries(void) {
    free((void *)g_aetherFxBlocks.items);
    g_aetherFxBlocks.items = NULL;
    g_aetherFxBlocks.count = 0;
    g_aetherFxBlocks.cap = 0;

    for (size_t i = 0; i < g_aetherFnPurity.count; i++) {
        free(g_aetherFnPurity.items[i].name);
    }
    free(g_aetherFnPurity.items);
    g_aetherFnPurity.items = NULL;
    g_aetherFnPurity.count = 0;
    g_aetherFnPurity.cap = 0;

    for (size_t i = 0; i < g_aetherTopLevelFns.count; i++) {
        free(g_aetherTopLevelFns.items[i]);
    }
    free(g_aetherTopLevelFns.items);
    g_aetherTopLevelFns.items = NULL;
    g_aetherTopLevelFns.count = 0;
    g_aetherTopLevelFns.cap = 0;

    for (size_t i = 0; i < g_aetherCallSurfaces.count; i++) {
        free(g_aetherCallSurfaces.items[i].surface);
    }
    free(g_aetherCallSurfaces.items);
    g_aetherCallSurfaces.items = NULL;
    g_aetherCallSurfaces.count = 0;
    g_aetherCallSurfaces.cap = 0;

    free((void *)g_aetherSynthesized.items);
    free((void *)g_aetherRangeBounds.items);
    g_aetherRangeBounds.items = NULL;
    g_aetherRangeBounds.count = 0;
    g_aetherRangeBounds.cap = 0;
    free((void *)g_aetherExplicitTypedDecls.items);
    g_aetherExplicitTypedDecls.items = NULL;
    g_aetherExplicitTypedDecls.count = 0;
    g_aetherExplicitTypedDecls.cap = 0;
    g_aetherSynthesized.items = NULL;
    g_aetherSynthesized.count = 0;
    g_aetherSynthesized.cap = 0;

    for (size_t i = 0; i < g_aetherLineAliases.count; i++) {
        free(g_aetherLineAliases.items[i].canonical);
        free(g_aetherLineAliases.items[i].surface);
    }
    free(g_aetherLineAliases.items);
    g_aetherLineAliases.items = NULL;
    g_aetherLineAliases.count = 0;
    g_aetherLineAliases.cap = 0;
}

/* Emit a diagnostic for an AST-parser-detected error. Reuses the shared
 * diagnostics helpers (aetherInferDiagnosticCode / aetherReportGuideHelp). */
void reportAetherAstError(const char *path, int line, const char *kind,
                          const char *detail, const char *hint) {
    const char *code = aetherInferDiagnosticCode(kind, detail);
    const char *label = (kind && strcmp(kind, "parser") != 0) ? kind : NULL;
    if (code) {
        if (label) {
            aetherDiagf( "%s:%d: [%s] Aether %s parser error: %s\n",
                    path ? path : "<aether>", line > 0 ? line : 1, code,
                    label, detail ? detail : "unknown parser error.");
        } else {
            aetherDiagf( "%s:%d: [%s] Aether parser error: %s\n",
                    path ? path : "<aether>", line > 0 ? line : 1, code,
                    detail ? detail : "unknown parser error.");
        }
    } else {
        if (label) {
            aetherDiagf( "%s:%d: Aether %s parser error: %s\n",
                    path ? path : "<aether>", line > 0 ? line : 1,
                    label, detail ? detail : "unknown parser error.");
        } else {
            aetherDiagf( "%s:%d: Aether parser error: %s\n",
                    path ? path : "<aether>", line > 0 ? line : 1,
                    detail ? detail : "unknown parser error.");
        }
    }
    if (hint && *hint) {
        aetherDiagf( "hint: %s\n", hint);
    }
    aetherReportGuideHelp(code);
}

/* Parse-time warning. Mirrors reportAetherAstError's formatting but never sets
 * p->hadError and never increments an error counter -- a warning must not fail
 * a build. First user: PREC-001. Note the forward-declaration pre-pass mutes
 * stderr (see below), so a warning emitted from an expression it re-parses is
 * printed once, by the authoritative pass. */
static void reportAetherAstWarning(const char *path, int line, const char *kind,
                                   const char *detail, const char *hint) {
    const char *code = aetherInferDiagnosticCode(kind, detail);
    if (code) {
        aetherDiagf("%s:%d: warning: [%s] Aether %s warning: %s\n",
                path ? path : "<aether>", line > 0 ? line : 1, code,
                kind ? kind : "parser", detail ? detail : "unknown warning.");
    } else {
        aetherDiagf("%s:%d: warning: Aether %s warning: %s\n",
                path ? path : "<aether>", line > 0 ? line : 1,
                kind ? kind : "parser", detail ? detail : "unknown warning.");
    }
    if (hint && *hint) {
        aetherDiagf("hint: %s\n", hint);
    }
    aetherReportGuideHelp(code);
}

/* Temporarily mute stderr. Returns a saved descriptor to pass to
 * aetherUnmuteStderr, or -1 if muting could not be set up (in which case
 * restoration is a no-op). Used to silence the throwaway forward-declaration
 * pre-pass: it re-parses the same top-level signatures the authoritative real
 * pass parses, so every diagnostic it could emit (SYN-001, parameter/return-type
 * syntax errors, etc.) is re-emitted with the correct line number by the real
 * pass. Without this the pre-pass prints each such diagnostic a second time. */
static int aetherMuteStderr(void) {
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    if (saved < 0) return -1;
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0) { close(saved); return -1; }
    if (dup2(devnull, STDERR_FILENO) < 0) { close(devnull); close(saved); return -1; }
    close(devnull);
    return saved;
}

static void aetherUnmuteStderr(int saved) {
    if (saved < 0) return;
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
}

/* ------------------------------------------------------------------ */
/* Binding table (the types are in ast_internal.h)                    */
/* ------------------------------------------------------------------ */

static void bindingTableInit(AetherBindingTable *t) {
    t->items = NULL; t->count = 0; t->cap = 0;
    t->scopeDepth = 0; t->scopeMark = 0;
    t->shadowed = NULL; t->shadowedCount = 0; t->shadowedCap = 0;
}

static void bindingTableFree(AetherBindingTable *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->items[i].name);
        free(t->items[i].typeName);
    }
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
    for (size_t i = 0; i < t->shadowedCount; i++) {
        free(t->shadowed[i].name);
        free(t->shadowed[i].typeName);
    }
    free(t->shadowed);
    t->shadowed = NULL;
    t->shadowedCount = 0;
    t->shadowedCap = 0;
    t->scopeDepth = 0;
    t->scopeMark = 0;
}

/* Record that pre-scope entry `name` (whose current type string ownership is
 * transferred in as `oldTypeName`) was overwritten inside the active function
 * scope, so bindingScopeLeave can restore it. On allocation failure the old
 * type is freed and the restore is silently skipped (OOM-only degradation). */
static void bindingTableSaveShadowed(AetherBindingTable *t, const char *name,
                                     char *oldTypeName) {
    if (t->shadowedCount == t->shadowedCap) {
        size_t newCap = t->shadowedCap ? t->shadowedCap * 2 : 4;
        AetherBinding *grown =
            (AetherBinding *)realloc(t->shadowed, newCap * sizeof(*grown));
        if (!grown) { free(oldTypeName); return; }
        t->shadowed = grown;
        t->shadowedCap = newCap;
    }
    char *nameDup = strdup(name);
    if (!nameDup) { free(oldTypeName); return; }
    t->shadowed[t->shadowedCount].name = nameDup;
    t->shadowed[t->shadowedCount].typeName = oldTypeName;
    t->shadowedCount++;
}

void bindingTableSet(AetherBindingTable *t, const char *name, const char *typeName) {
    if (!name || !typeName) return;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].name, name) == 0) {
            char *dup = strdup(typeName);
            if (dup) {
                if (t->scopeDepth > 0 && i < t->scopeMark) {
                    /* Shadowing a global (pre-scope) entry: keep its original
                     * type for restoration when the function scope closes. */
                    bindingTableSaveShadowed(t, name, t->items[i].typeName);
                } else {
                    free(t->items[i].typeName);
                }
                t->items[i].typeName = dup;
            }
            return;
        }
    }
    if (t->count == t->cap) {
        size_t newCap = t->cap ? t->cap * 2 : 8;
        AetherBinding *items = (AetherBinding *)realloc(t->items, newCap * sizeof(*items));
        if (!items) return;
        t->items = items;
        t->cap = newCap;
    }
    t->items[t->count].name = strdup(name);
    t->items[t->count].typeName = strdup(typeName);
    if (t->items[t->count].name && t->items[t->count].typeName) t->count++;
    else { free(t->items[t->count].name); free(t->items[t->count].typeName); }
}

const char *bindingTableGet(const AetherBindingTable *t, const char *name, size_t len) {
    if (!t || !name) return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strlen(t->items[i].name) == len && strncmp(t->items[i].name, name, len) == 0) {
            return t->items[i].typeName;
        }
    }
    return NULL;
}

/* Open a function scope: everything set from here until the matching
 * bindingScopeLeave (parameters, body lets, temps) is function-local.
 * Re-entrant as a counter so an accidental nested enter/leave pair is
 * harmless; only the outermost pair marks/truncates. */
static void bindingScopeEnter(AetherBindingTable *t) {
    if (!t) return;
    if (t->scopeDepth++ == 0) t->scopeMark = t->count;
}

/* Close a function scope: drop the function's local entries and restore any
 * shadowed pre-scope (global/imported) bindings to their original types.
 * Restoration runs in reverse save order so the value from before the scope
 * is the one that survives even if a global was overwritten repeatedly. */
static void bindingScopeLeave(AetherBindingTable *t) {
    if (!t || t->scopeDepth == 0) return;
    if (--t->scopeDepth > 0) return;
    for (size_t i = t->scopeMark; i < t->count; i++) {
        free(t->items[i].name);
        free(t->items[i].typeName);
    }
    t->count = t->scopeMark;
    while (t->shadowedCount > 0) {
        AetherBinding *s = &t->shadowed[--t->shadowedCount];
        for (size_t i = 0; i < t->count; i++) {
            if (strcmp(t->items[i].name, s->name) == 0) {
                free(t->items[i].typeName);
                t->items[i].typeName = s->typeName;
                s->typeName = NULL;
                break;
            }
        }
        free(s->name);
        free(s->typeName); /* NULL when restored; frees only if entry vanished */
    }
    t->scopeMark = 0;
}

/* ------------------------------------------------------------------ */
/* Contract + tuple support tables (types in ast_internal.h)          */
/* ------------------------------------------------------------------ */

static void tupleTableInit(AetherTupleTable *t) { t->items = NULL; t->count = 0; t->cap = 0; }

static void tupleSigFree(AetherTupleSig *s) {
    if (!s) return;
    free(s->functionName);
    for (size_t i = 0; i < s->itemCount; i++) free(s->itemTypes[i]);
    free(s->itemTypes);
    s->functionName = NULL;
    s->itemTypes = NULL;
    s->itemCount = 0;
}

static void tupleTableFree(AetherTupleTable *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) tupleSigFree(&t->items[i]);
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

/* Record (or replace) a tuple signature for `name`. itemTypes are deep-copied. */
static bool tupleTableSet(AetherTupleTable *t, const char *name, int typeId,
                          char **itemTypes, size_t itemCount) {
    if (!t || !name || !itemTypes || itemCount == 0) return false;
    AetherTupleSig *slot = NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].functionName, name) == 0) { slot = &t->items[i]; break; }
    }
    if (!slot) {
        if (t->count == t->cap) {
            size_t newCap = t->cap ? t->cap * 2 : 4;
            AetherTupleSig *items = (AetherTupleSig *)realloc(t->items, newCap * sizeof(*items));
            if (!items) return false;
            t->items = items;
            t->cap = newCap;
        }
        slot = &t->items[t->count];
        memset(slot, 0, sizeof(*slot));
        slot->functionName = strdup(name);
        if (!slot->functionName) return false;
        t->count++;
    } else {
        for (size_t i = 0; i < slot->itemCount; i++) free(slot->itemTypes[i]);
        free(slot->itemTypes);
        slot->itemTypes = NULL;
        slot->itemCount = 0;
    }
    slot->typeId = typeId;
    slot->itemTypes = (char **)calloc(itemCount, sizeof(char *));
    if (!slot->itemTypes) return false;
    for (size_t i = 0; i < itemCount; i++) {
        slot->itemTypes[i] = strdup(itemTypes[i] ? itemTypes[i] : "");
        if (!slot->itemTypes[i]) return false;
    }
    slot->itemCount = itemCount;
    return true;
}

const AetherTupleSig *tupleTableGet(const AetherTupleTable *t, const char *name, size_t len) {
    if (!t || !name) return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strlen(t->items[i].functionName) == len &&
            strncmp(t->items[i].functionName, name, len) == 0) {
            return &t->items[i];
        }
    }
    return NULL;
}

/* Look up a tuple signature by its synthesized record type's id (the N in
 * "__AetherTupleN") rather than by owning-function name. Used to recover a
 * tuple's arity from a receiver's *type name* alone (e.g. a `let`-bound
 * variable's recorded binding type, or a call's recorded return type), which
 * is all `t.0`/`pair().0` field-index parsing has to work with -- neither
 * necessarily still has the original function name in scope. Every
 * registered signature has a distinct typeId (assigned once per tuple-return
 * `fn`, see nextTupleTypeId in aetherRegisterTupleGlobals), so this is a
 * simple linear scan, not a name collision hazard. */
static const AetherTupleSig *tupleTableGetByTypeId(const AetherTupleTable *t, int typeId) {
    if (!t) return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (t->items[i].typeId == typeId) return &t->items[i];
    }
    return NULL;
}

/* True if `name` is a synthesized tuple record type name ("__AetherTupleN"),
 * and if so, extracts N into *outTypeId. Mirrors the naming convention from
 * aetherTupleSyntheticTypeName (the inverse operation). */
static bool aetherParseSyntheticTupleTypeId(const char *name, int *outTypeId) {
    static const char kPrefix[] = "__AetherTuple";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (!name || strncmp(name, kPrefix, kPrefixLen) != 0) return false;
    const char *digits = name + kPrefixLen;
    if (!*digits) return false;
    for (const char *c = digits; *c; c++) {
        if (!isdigit((unsigned char)*c)) return false;
    }
    if (outTypeId) *outTypeId = atoi(digits);
    return true;
}

/* Deterministic synthetic type name for a tuple signature's record lowering,
 * e.g. "__AetherTuple3" for typeId 3. Written into a caller-supplied buffer
 * (no storage needed on AetherTupleSig -- every use site can re-derive it from
 * typeId alone). */
void aetherTupleSyntheticTypeName(char *buf, size_t bufSize, int typeId) {
    snprintf(buf, bufSize, "__AetherTuple%d", typeId);
}

/* A tuple-return function calling itself (directly or indirectly), or two
 * `par` branches calling the same tuple-returning function, used to be
 * tracked and rejected here (TUP-001 call-cycle check / PAR-003) because
 * tuple returns lowered to shared per-function globals that raced under
 * reentrancy. Tuple returns now lower to a record returned by value (see
 * buildSyntheticTupleRecordType, parseTupleReturn, parseLetTupleDestructure);
 * the VM deep-copies a record on every return (returnFromCall/copyRecord in
 * pscal-core), so each call -- recursive or concurrent -- gets its own
 * independent result. There is no shared state left for a call-graph tracker
 * to protect, so it was removed rather than left as dead bookkeeping.
 */

static void fieldNameListInit(AetherFieldNameList *l) { l->names = NULL; l->count = 0; l->cap = 0; }

static void fieldNameListFree(AetherFieldNameList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) free(l->names[i]);
    free(l->names);
    l->names = NULL;
    l->count = 0;
    l->cap = 0;
}

static void fieldNameListAdd(AetherFieldNameList *l, const char *name, size_t len) {
    if (!l || !name) return;
    if (l->count == l->cap) {
        size_t newCap = l->cap ? l->cap * 2 : 8;
        char **names = (char **)realloc(l->names, newCap * sizeof(char *));
        if (!names) return;
        l->names = names;
        l->cap = newCap;
    }
    char *dup = (char *)malloc(len + 1);
    if (!dup) return;
    memcpy(dup, name, len);
    dup[len] = '\0';
    l->names[l->count++] = dup;
}

static bool fieldNameListHas(const AetherFieldNameList *l, const char *name, size_t len) {
    if (!l || !name) return false;
    for (size_t i = 0; i < l->count; i++) {
        if (strlen(l->names[i]) == len && strncmp(l->names[i], name, len) == 0) return true;
    }
    return false;
}

/* Combine a fresh contract expression onto an existing one with `&&`, wrapping
 * each operand in parentheses -- byte for byte as translate.c appendContractExpr
 * (which yields `(a) && (b)` style: actually `((a) && (b))`-free combination).
 * The rewriter emits `(A) && (B)` then later wraps the whole in `!( ... )`; for
 * three it nests `((A && B)) && C` -> we reproduce its exact left-folded shape
 * `(prev) && (next)`. */
static char *appendContractExprText(char *existing, const char *expr) {
    if (!expr || !*expr) return existing;
    if (!existing) return strdup(expr);
    size_t need = strlen(existing) + strlen(expr) + 16;
    char *combined = (char *)malloc(need);
    if (!combined) return existing;
    snprintf(combined, need, "(%s) && (%s)", existing, expr);
    free(existing);
    return combined;
}

/* ------------------------------------------------------------------ */
/* Parser state (AetherParser is in ast_internal.h)                   */
/* ------------------------------------------------------------------ */

/* Raw next token straight from the rea lexer (no `..` synthesis), honoring the
 * small FIFO buffer used to queue synthesized/look-ahead tokens. */
/* The shared Rea lexer reserves about 48 words. Aether uses fewer than half
 * of them; the rest -- Rea's class, exception, module and thread syntax, and
 * its lowercase type-name words -- have no meaning in Aether, yet leaked in as
 * keyword tokens: `let join: Int` failed with a bare "expected name", a field
 * named `word` or `text` was rejected, and the operator word `mul` silently
 * meant `*`. Demote every word Aether has no use for to an ordinary
 * identifier at the token boundary, so the parser only ever sees Aether's own
 * reserved set. The foreign statement keywords (`return`, `class`, `import`,
 * ...) are still caught -- by text, at statement start
 * (aetherForeignStatementKeyword) -- so the guide's SYN-001 hints survive. */
static void aetherDemoteForeignKeyword(ReaToken *t) {
    switch (t->type) {
        case REA_TOKEN_ALIAS: case REA_TOKEN_CASE: case REA_TOKEN_CATCH:
        case REA_TOKEN_CLASS: case REA_TOKEN_DEFAULT: case REA_TOKEN_DO:
        case REA_TOKEN_EXTENDS: case REA_TOKEN_IMPORT: case REA_TOKEN_JOIN:
        case REA_TOKEN_MATCH: case REA_TOKEN_MODULE: case REA_TOKEN_RETURN:
        case REA_TOKEN_SPAWN: case REA_TOKEN_SUPER: case REA_TOKEN_SWITCH:
        case REA_TOKEN_THROW: case REA_TOKEN_TRY:
        /* Lowercase type-name words: Aether spells its types capitalized
         * (Int/Real/Text/Bool), so these are plain names in Aether source. */
        case REA_TOKEN_INT: case REA_TOKEN_INT64: case REA_TOKEN_INT32:
        case REA_TOKEN_INT16: case REA_TOKEN_INT8:
        case REA_TOKEN_UINT64: case REA_TOKEN_UINT32: case REA_TOKEN_UINT16:
        case REA_TOKEN_UINT8:
        case REA_TOKEN_FLOAT: case REA_TOKEN_FLOAT32: case REA_TOKEN_LONG_DOUBLE:
        case REA_TOKEN_CHAR: case REA_TOKEN_BYTE: case REA_TOKEN_WORD:
        case REA_TOKEN_STR: case REA_TOKEN_TEXT: case REA_TOKEN_MSTREAM:
        case REA_TOKEN_VOID: case REA_TOKEN_BOOL:
            t->type = REA_TOKEN_IDENTIFIER;
            break;
        case REA_TOKEN_STAR:
            /* `mul` lexes as `*`; Aether's multiplication is only ever `*`. */
            if (t->length == 3 && strncmp(t->start, "mul", 3) == 0) t->type = REA_TOKEN_IDENTIFIER;
            break;
        case REA_TOKEN_MYSELF:
            /* `my` is Rea's short receiver; Aether's is `self`. Keep `myself`,
             * which the lowering itself emits. */
            if (t->length == 2) t->type = REA_TOKEN_IDENTIFIER;
            break;
        default:
            break;
    }
}

static ReaToken aetherRawNext(AetherParser *p) {
    if (p->queueCount > 0) {
        ReaToken t = p->queue[p->queueHead];
        p->queueHead = (p->queueHead + 1) % 3;
        p->queueCount--;
        return t;
    }
    ReaToken t = reaNextToken(&p->lexer);
    aetherDemoteForeignKeyword(&t);
    return t;
}

/* Append a token to the tail of the FIFO so it is returned (in order) before the
 * lexer is consulted again. */
static void aetherRawEnqueue(AetherParser *p, ReaToken t) {
    if (p->queueCount >= 3) return; /* never exceeded by the `..` logic below */
    int tail = (p->queueHead + p->queueCount) % 3;
    p->queue[tail] = t;
    p->queueCount++;
}

/* True if a NUMBER token's lexeme ends in a literal '.', i.e. the lexer folded
 * the first dot of a `..` into it (e.g. `0..5` -> NUMBER "0."). */
static bool numberFoldsTrailingDot(const ReaToken *t) {
    return t->type == REA_TOKEN_NUMBER && t->length > 0 && t->start[t->length - 1] == '.';
}

/* True if a NUMBER token's lexeme begins with a literal '.', i.e. the lexer
 * produced the high bound of a `<id>..N` range as NUMBER ".5". */
static bool numberFoldsLeadingDot(const ReaToken *t) {
    return t->type == REA_TOKEN_NUMBER && t->length > 0 && t->start[0] == '.';
}

/* True if a NUMBER token is exactly '.' followed by one or more digits and
 * nothing else (no second '.', no exponent) -- i.e. the rea lexer folded a
 * tuple field index like `t.0` into a single NUMBER(".0") token instead of
 * DOT + NUMBER("0") (there is no whitespace-independent way to tell the
 * lexer "digits after a dot on a receiver are a field index, not a real
 * literal" -- it has no lookback, see numberFoldsLeadingDot). parsePostfix
 * treats this token the same as DOT + identifier-like("N"), synthesizing a
 * `.itemN` field access. Deliberately narrower than numberFoldsLeadingDot:
 * `.0e5` or `.0.5` fall through to ordinary number-literal handling instead
 * of being misread as an index. */
static bool aetherIsTupleIndexToken(const ReaToken *t) {
    if (!t || t->type != REA_TOKEN_NUMBER || t->length < 2 || t->start[0] != '.') return false;
    for (size_t i = 1; i < t->length; i++) {
        if (!isdigit((unsigned char)t->start[i])) return false;
    }
    return true;
}

static ReaToken makeDotDot(const char *at, int line) {
    ReaToken d;
    d.type = AE_TOKEN_DOTDOT;
    d.start = at;
    d.length = 2;
    d.line = line;
    return d;
}

/* Aether tokenizer: recognize the `..` range operator the rea lexer cannot.
 *
 * The rea lexer destroys `..` in four shapes; we reconstruct a single
 * AE_TOKEN_DOTDOT token (queued so it surfaces between the trimmed bounds) so
 * the loop parser sees a clean  low  DOTDOT  high  stream:
 *   a..b : IDENT DOT DOT IDENT       -> DOTDOT replaces the two DOTs
 *   0..5 : NUMBER("0.") NUMBER(".5") -> trim trailing/leading dots, inject DOTDOT
 *   0..b : NUMBER("0.") DOT IDENT    -> trim trailing dot, inject DOTDOT
 *   a..5 : IDENT DOT NUMBER(".5")    -> trim leading dot, inject DOTDOT
 * Every other token passes through untouched. */
void aetherAdvance(AetherParser *p) {
    p->prevLine = p->current.line;
    p->prevStart = p->current.start;
    p->prevLength = p->current.length;
    ReaToken t = aetherRawNext(p);

    /* `<num>..` : current NUMBER folded a trailing '.'. */
    if (numberFoldsTrailingDot(&t)) {
        ReaToken next = aetherRawNext(p);
        if (next.type == REA_TOKEN_DOT) {                 /* 0..b */
            t.length -= 1;                                /* drop folded '.'      */
            aetherRawEnqueue(p, makeDotDot(t.start + t.length, t.line));
            p->current = t;
            return;
        }
        if (numberFoldsLeadingDot(&next)) {               /* 0..5 */
            t.length -= 1;                                /* low bound: "0"       */
            next.start += 1; next.length -= 1;            /* high bound: "5"      */
            aetherRawEnqueue(p, makeDotDot(t.start + t.length, t.line));
            aetherRawEnqueue(p, next);
            p->current = t;
            return;
        }
        aetherRawEnqueue(p, next);                        /* plain real literal   */
        p->current = t;
        return;
    }

    /* `<id>..` : current DOT is the first range dot. */
    if (t.type == REA_TOKEN_DOT) {
        ReaToken next = aetherRawNext(p);
        if (next.type == REA_TOKEN_DOT) {                 /* a..b */
            p->current = makeDotDot(t.start, t.line);
            return;
        }
        if (numberFoldsLeadingDot(&next)) {               /* a..5 */
            next.start += 1; next.length -= 1;
            aetherRawEnqueue(p, next);
            p->current = makeDotDot(t.start, t.line);
            return;
        }
        aetherRawEnqueue(p, next);                        /* plain member dot     */
        p->current = t;
        return;
    }

    p->current = t;
}

/* Initialize a parser over `source`, clearing the token FIFO and class context.
 * Does NOT prime `current` -- callers invoke aetherAdvance() once afterward. */
static void aetherParserInit(AetherParser *p, const char *source,
                             AetherBindingTable *bindings) {
    reaInitLexer(&p->lexer, source);
    p->queueHead = 0;
    p->queueCount = 0;
    p->currentFunctionType = TYPE_VOID;
    p->functionDepth = 0;
    p->hadError = false;
    p->currentClassName = NULL;
    p->currentMethodIndex = 0;
    p->currentModuleName = NULL;
    p->bindings = bindings;
    p->funcReturns = NULL;
    p->tuples = NULL;
    p->nextTupleTypeId = NULL;
    p->classFields = NULL;
    p->currentTupleSig = NULL;
    p->currentPostExpr = NULL;
    p->currentFunctionName = NULL;
    p->currentReturnTypeName = NULL;
    p->currentFunctionIsMethod = false;
    p->inMethodContract = false;
    p->forwardScan = false;
    p->lastFnWasExtension = false;
    p->pending.preExpr = NULL;
    p->pending.postExpr = NULL;
    p->pending.isPure = 0;
    p->pendingAnnotCount = 0;
    p->pendingAnnotName[0] = '\0';
    p->pendingAnnotLine = 0;
    p->pendingObjLits = NULL;
    p->pendingObjLitCount = 0;
    p->pendingObjLitCapacity = 0;
    p->nextObjLitId = 0;
    p->nextLoopId = 0;
    p->exprAfterOpText = NULL;
    p->exprAfterOpLen = 0;
    p->exprAfterOpAt = NULL;
    p->exprAfterOpLine = 0;
    p->stmtStartAt = NULL;
    p->detachedText = false;
}

/* Record that the operator token `op` was just consumed, so a missing right
 * operand can name it (aetherReportMissingExpr). Keyed on the start of the
 * token after the operator, so a stale record never matches. */
void aetherNoteOperator(AetherParser *p, const ReaToken *op) {
    p->exprAfterOpText = op->start;
    p->exprAfterOpLen = (int)op->length;
    p->exprAfterOpAt = p->current.start;
    p->exprAfterOpLine = op->line;
}

/* SYN-001 for an expression that is not there: `if x == {`, `ret x + ;`,
 * `println(x + )`, `f(a, , b)`. These used to return a NULL operand with no
 * diagnostic, and callers built a NULL AST child from it, so the program ran
 * with a missing branch, a nil return or a dropped argument and exit 0.
 * Reports once (no-op when an error is already recorded) and always sets
 * hadError. `context` (e.g. "after 'ret'") overrides the operator wording. */
void aetherReportMissingExpr(AetherParser *p, const char *context) {
    if (p->hadError) return;
    p->hadError = true;
    if (p->detachedText) return; /* parseExprFromText's caller reports */
    char found[48];
    if (p->current.type == REA_TOKEN_EOF || !p->current.start || p->current.length <= 0) {
        snprintf(found, sizeof(found), "end of input");
    } else {
        int n = p->current.length > 32 ? 32 : (int)p->current.length;
        snprintf(found, sizeof(found), "'%.*s'", n, p->current.start);
    }
    char msg[160];
    int line = p->current.line;
    if (context) {
        snprintf(msg, sizeof(msg), "expected an expression %s, found %s.", context, found);
    } else if (p->exprAfterOpText && p->exprAfterOpAt == p->current.start) {
        int on = p->exprAfterOpLen > 8 ? 8 : p->exprAfterOpLen;
        snprintf(msg, sizeof(msg), "expected an expression after '%.*s', found %s.",
                 on, p->exprAfterOpText, found);
        if (p->exprAfterOpLine > 0) line = p->exprAfterOpLine;
    } else {
        snprintf(msg, sizeof(msg), "expected an expression, found %s.", found);
    }
    reportAetherAstError(aetherSemanticGetSourcePath(), line, "parser", msg, NULL);
}

/* Queue a hoisted object-literal declaration (an i_val==1 AST_COMPOUND from
 * buildObjectInitDecl) for splice into the statement currently being parsed.
 * See the parseStatement wrapper below for where these get flushed. */
void pushPendingObjLit(AetherParser *p, AST *hoisted) {
    if (!hoisted) return;
    if (p->pendingObjLitCount == p->pendingObjLitCapacity) {
        int newCap = p->pendingObjLitCapacity ? p->pendingObjLitCapacity * 2 : 4;
        AST **grown = (AST **)realloc(p->pendingObjLits, sizeof(AST *) * (size_t)newCap);
        if (!grown) {
            freeAST(hoisted); /* can't queue it; drop rather than crash */
            return;
        }
        p->pendingObjLits = grown;
        p->pendingObjLitCapacity = newCap;
    }
    p->pendingObjLits[p->pendingObjLitCount++] = hoisted;
}

/* True if the current token's text equals `kw` exactly. Aether keywords come
 * through the Rea lexer as identifiers, so we compare the lexeme span. */
static bool tokTextIs(const ReaToken *t, const char *kw) {
    size_t klen = strlen(kw);
    return t->length == klen && strncmp(t->start, kw, klen) == 0;
}

bool isAetherKeyword(const ReaToken *t, const char *kw) {
    return t->type == REA_TOKEN_IDENTIFIER && tokTextIs(t, kw);
}

/* Aether's name positions accept exactly REA_TOKEN_IDENTIFIER. The shared Rea
 * lexer's foreign keywords (`join`, `match`, `class`, ...) and its lowercase
 * type-name words (`int`, `text`, `word`, ...) are demoted to plain
 * identifiers before the parser sees them (aetherDemoteForeignKeyword), so a
 * field named `word`, a local named `match` or a parameter named `text` is
 * just a name. Kept as a predicate so every name position reads the same. */
bool aetherTokenIsIdentifierLike(const ReaToken *t) {
    return t && t->type == REA_TOKEN_IDENTIFIER;
}

/* Aether keywords that reach the parser as identifiers (the Rea lexer does not
 * know them). A member or function named after one would parse, then shadow
 * the statement keyword inside every method body, so member-name positions
 * reject them with a reserved-word diagnostic. `in` and `step` are keywords
 * only inside a loop header and stay usable as ordinary names. */
static bool aetherIsAetherTextKeyword(const ReaToken *t) {
    static const char *const kws[] = {
        "fn", "let", "ret", "loop", "fx", "par", "self", "use", "and", "or", "not"
    };
    if (!t || t->type != REA_TOKEN_IDENTIFIER) return false;
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if (tokTextIs(t, kws[i])) return true;
    }
    return false;
}

/* Classify a token that turned up where an Aether *member name* (a `type` field
 * or a `fn` name) was expected but is a reserved word -- so the diagnostic can
 * name the collision instead of a bare "unexpected token" / "expected function
 * name". Returns a short human category, or NULL when the token is an ordinary
 * identifier (or punctuation / a literal / EOF) and the caller should keep its
 * generic message. Since the foreign-keyword demotion, the only reserved words
 * left are Aether's own: the keyword tokens the lexer shares with Aether
 * (new/for/if/while/...), Aether's text-level keywords (fn/let/ret/loop/...)
 * and the operator words div/mod/xor. */
static const char *aetherReservedWordCategory(const ReaToken *t) {
    if (!t || t->length == 0 || !t->start) return NULL;
    unsigned char c0 = (unsigned char)t->start[0];
    if (!(isalpha(c0) || c0 == '_')) return NULL; /* only word-shaped lexemes collide */
    switch (t->type) {
        case REA_TOKEN_IDENTIFIER:
            return aetherIsAetherTextKeyword(t) ? "reserved keyword" : NULL;
        /* word forms of arithmetic operators: div, mod(%), xor */
        case REA_TOKEN_INT_DIV: case REA_TOKEN_PERCENT: case REA_TOKEN_XOR:
            return "reserved operator word";
        default:
            return "reserved keyword";
    }
}

/* Emit a diagnostic that names a reserved-word/member-name collision -- the
 * broadest gap generative model testing found (a field or method named after a
 * reserved word or operator word, e.g. `word`, `mul`, `new`). `member` is
 * "field", "method", or "function". Returns true if `t` was a reserved word and
 * a naming diagnostic was printed; false if the token is not a reserved word and
 * the caller should fall back to its generic parse error. */
static bool reportReservedMemberName(const ReaToken *t, const char *member) {
    const char *cat = aetherReservedWordCategory(t);
    if (!cat) return false;

    char name[64];
    size_t n = (size_t)t->length;
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, t->start, n);
    name[n] = '\0';

    char detail[256];
    char hint[320];
    if (t->type == REA_TOKEN_NEW) {
        /* `new` is the object allocator; point at the (missing) constructor idiom. */
        snprintf(detail, sizeof(detail),
                 "'%s' is a reserved keyword (the object allocator) and cannot be used "
                 "as a %s name.", name, member);
        snprintf(hint, sizeof(hint),
                 "Aether has no constructor methods -- allocate with `new T()` then set "
                 "fields, or write a top-level factory `fn` with a non-reserved name.");
    } else {
        snprintf(detail, sizeof(detail),
                 "'%s' is a %s and cannot be used as a %s name.", name, cat, member);
        snprintf(hint, sizeof(hint),
                 "rename it (e.g. `%sValue`); Aether keywords (new/for/if/loop/ret/...) "
                 "and operator words (div/mod/xor/and/or/not) are not valid field or "
                 "method names.", name);
    }
    reportAetherAstError(aetherSemanticGetSourcePath(), t->line, "declaration", detail, hint);
    return true;
}

/* Map an Aether stdlib builtin name to its canonical Rea/pscal builtin (the
 * table began as a port of the retired rewriter's appendAetherBuiltinAlias()).
 * Returns the canonical name, or `name` unchanged if there is no alias
 * (e.g. println -> writeln -> AST_WRITELN). */
static const char *aliasBuiltinName(const char *name) {
    static const struct { const char *from; const char *to; } aliases[] = {
        { "task_spawn",      "thread_spawn_named" },
        { "task_queue",      "thread_pool_submit" },
        { "task_set_name",   "thread_set_name" },
        { "task_pause",      "thread_pause" },
        { "task_resume",     "thread_resume" },
        { "task_cancel",     "thread_cancel" },
        { "task_lookup",     "thread_lookup" },
        { "task_wait",       "WaitForThread" },
        { "task_status",     "thread_get_status" },
        { "task_result",     "thread_get_result" },
        { "task_stats",      "thread_stats" },
        { "task_stats_json", "ThreadStatsJson" },
        { "ai_chat",         "openaichatcompletions" },
        { "builtins_json",   "aetherbuiltinsjson" },
        { "builtin_info",    "aetherbuiltininfo" },
        { "println",         "writeln" },
        { "int_to_text",     "IntToStr" },
        { "sleep",           "delay" },
        { "print",           "write" },
        { "string_len",      "length" },
        { "len",             "length" },
        /* `exit(n)` ends the program with status n. Unaliased it is pscal's
         * Exit(value), which returns n from the enclosing function and lets
         * the program carry on (exit 0). */
        { "exit",            "halt" },
    };
    if (!name) return name;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(name, aliases[i].from) == 0) {
            return aliases[i].to;
        }
    }
    return name;
}

/* Copy the current token's lexeme into a freshly allocated pscal identifier
 * Token (mirrors rea copyCurrentTokenAsIdentifier). */
static Token *currentAsIdentifier(AetherParser *p) {
    size_t len = (size_t)p->current.length;
    char *lex = (char *)malloc(len + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, len);
    lex[len] = '\0';
    Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    return tok;
}

/* ------------------------------------------------------------------ */
/* Type-name mapping                                                   */
/* ------------------------------------------------------------------ */

/* Map an Aether type name (the lexeme span) to the Rea keyword-name the
 * rewriter would emit, plus the resulting VarType. Returns false if the name
 * is not a known builtin Aether type. This reproduces translate.c mapTypeName()
 * followed by rea mapType(), which is the output contract: the type node
 * token value and var_type must equal what the rewriter+parseRea produce. */
bool mapAetherType(const char *name, size_t len,
                   const char **outReaName, VarType *outType) {
    struct { const char *aether; const char *rea; VarType vt; } table[] = {
        { "Int",     "int",   TYPE_INT64 },
        { "Real",    "float", TYPE_DOUBLE },
        { "Text",    "str",   TYPE_UNICODE_STRING },
        { "Bool",    "bool",  TYPE_BOOLEAN },
        /* Accepted alternate spellings for the two scalars whose canonical
         * Aether name differs from what most languages call them. A model
         * writing without the guide in its prompt reaches for `Float` and
         * `String` constantly; both lower identically to Real/Text, so the
         * choice is between accepting them and spending a repair round on a
         * name that costs nothing to support. Kept adjacent to the canonical
         * rows so the two never drift apart. TYPE-002 catches the spellings
         * that are NOT accepted (`Double`, `Integer`, `Boolean`, ...) and
         * names the canonical type in the diagnostic. */
        { "Float",   "float", TYPE_DOUBLE },
        { "String",  "str",   TYPE_UNICODE_STRING },
        { "Void",    "void",  TYPE_VOID },
        /* TOON surface types lower exactly as translate.c mapTypeName: the TOON
         * literal is a string; doc/node handles are opaque integer handles. The
         * TOON handle/scalar *type* discipline is enforced by semantic.c on the
         * source text, so here we only need the codegen-compatible lowering. */
        { "TOON",    "str",   TYPE_UNICODE_STRING },
        { "ToonDoc", "int",   TYPE_INT64 },
        { "ToonNode","int",   TYPE_INT64 },
        /* Memory-stream handle (HTTP response bodies, SocketReceive, mstream*
         * builtins). Unlike the TOON handles this is NOT an integer in the VM:
         * it lowers to rea's `mstream` keyword / TYPE_MEMORYSTREAM so the
         * backend assigns the real MEMORY_STREAM value. */
        { "MStream", "mstream", TYPE_MEMORYSTREAM },
        /* Pascal-style file handle for the assign/reset/rewrite/append/read/
         * readln/write/writeln/close/eof/erase/rename builtins (real vm_builtins,
         * confirmed via builtins_json(true)). Lowers to rea's `text` keyword /
         * TYPE_FILE -- rea's own `text f; assign(f, path); ...` idiom (see
         * external/rea/examples/base/hangman5) already exercises this exact
         * VarType end to end, so File just gives Aether source the same
         * declaration surface. Note rea's `text` is this file handle, NOT a
         * string -- Aether's string type is "Text", spelled distinctly. */
        { "File",    "text",  TYPE_FILE },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t alen = strlen(table[i].aether);
        if (len == alen && strncmp(name, table[i].aether, alen) == 0) {
            *outReaName = table[i].rea;
            *outType = table[i].vt;
            return true;
        }
    }
    return false;
}

/* Release a node returned by lookupType, iff it is ours to release.
 *
 * lookupType is `rea_lookupType` here (FRONTEND_REA aliases it via
 * common/frontend_symbol_aliases.h), and that function has SPLIT OWNERSHIP:
 *   - a type-table hit returns the registry's own AST -- BORROWED. Freeing it
 *     would leave `TypeEntry.typeAST` dangling for every later lookup and for
 *     the table's teardown.
 *   - a miss on a builtin rea type name (int64, char, str, word, ... matched
 *     case-insensitively) fabricates a transient token-less AST_VARIABLE --
 *     OWNED by us, and leaked if we do not free it.
 *
 * The discriminator must therefore test BOTH the absent token and the node
 * type. "No token" alone is not sufficient in Aether: rea builds its record
 * entries as newASTNode(AST_RECORD_TYPE, classNameTok) specifically so its
 * table entries are token-bearing (see rea parser.c parseClassDecl), but
 * Aether registers its record types token-less -- both the user `type` path
 * and the synthetic tuple records. A `!token`-only check therefore matches
 * exactly Aether's own class/tuple entries and tries to free the live table
 * node.
 *
 * This discriminator is load-bearing, not merely tidy. It used to be that a
 * misfire here was inert: `freeAST` bailed out on isNodeInTypeTable() before
 * releasing anything, so the worst case was a poisoned `freed` flag. That is no
 * longer true. pscal-core's freeTypeTableASTNodes() now unlinks each entry
 * before freeing it, so type-table nodes are genuinely released at teardown --
 * which means a stray free of a live table entry is a real double-free/UAF, not
 * a no-op. Keep the type check. Mirrors the idiom in rea's parseVarDecl /
 * reaRefResolvesToClass. */
void releaseTransientTypeNode(AST *resolved) {
    if (resolved && !resolved->token && resolved->type == AST_VARIABLE) {
        freeAST(resolved);
    }
}

/* For semantic.c's TYPE-002 check: is `name` one of Aether's own builtin type
 * spellings (Int/Real/Text/Bool/Void, the accepted Float/String aliases, the
 * opaque handle types)? Case-sensitive, exactly like a declaration. */
int aetherAstIsBuiltinTypeName(const char *name) {
    const char *reaName = NULL;
    VarType vt = TYPE_VOID;
    return name && mapAetherType(name, strlen(name), &reaName, &vt);
}

/* Build the type node for a value-bearing type (non-Void), mirroring how rea's
 * parseVarDecl builds the type node:
 *   - builtin keyword type -> AST_TYPE_IDENTIFIER with the mapped VarType.
 *   - user type that resolves (via lookupType) to a record/class -> a POINTER:
 *     AST_POINTER_TYPE -> AST_TYPE_REFERENCE(TYPE_RECORD), var_type POINTER
 *     (object variables are pointers; this is what makes `c.method()` type-check
 *     and matches the rewriter's `Counter c = ...;` lowering byte for byte).
 *   - otherwise an AST_TYPE_REFERENCE with TYPE_UNKNOWN. */
AST *buildTypeNode(const char *name, size_t len, int line, VarType *outType) {
    const char *reaName = NULL;
    VarType vt = TYPE_VOID;
    if (mapAetherType(name, len, &reaName, &vt)) {
        Token *tok = newToken(TOKEN_IDENTIFIER, reaName, line, 0);
        AST *node = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(node, vt);
        *outType = vt;
        return node;
    }
    char *lex = (char *)malloc(len + 1);
    if (!lex) return NULL;
    memcpy(lex, name, len);
    lex[len] = '\0';

    /* Resolve user-defined types: a record/class becomes a pointer. */
    AST *resolved = lookupType(lex);
    bool treatAsPointer = false;
    if (resolved) {
        if (resolved->type == AST_RECORD_TYPE ||
            resolved->var_type == TYPE_RECORD ||
            resolved->var_type == TYPE_POINTER) {
            treatAsPointer = true;
        }
        releaseTransientTypeNode(resolved);
    }

    Token *tok = newToken(TOKEN_IDENTIFIER, lex, line, 0);
    free(lex);
    if (treatAsPointer) {
        AST *refNode = newASTNode(AST_TYPE_REFERENCE, tok);
        setTypeAST(refNode, TYPE_RECORD);
        AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
        setTypeAST(ptrNode, TYPE_POINTER);
        setRight(ptrNode, refNode);
        *outType = TYPE_POINTER;
        return ptrNode;
    }
    AST *node = newASTNode(AST_TYPE_REFERENCE, tok);
    setTypeAST(node, TYPE_UNKNOWN);
    *outType = TYPE_UNKNOWN;
    return node;
}

/* Like buildTypeNode, but for an Aether type *name* that may carry trailing
 * `[]` array-suffix markers (e.g. "Int[]", the binding-table convention
 * parseTypeWithArraySuffix produces for an explicit `: Int[]` annotation --
 * see its comment). buildTypeNode itself has no notion of this suffix; called
 * directly on a name like "Int[]" it does a literal (and failing) `lookupType
 * ("Int[]")`, surfacing as "identifier 'Int[]' not in scope" -- the bug this
 * function exists to close. Strips one trailing "[]" at a time, recursing on
 * the base name and wrapping each level in an AST_ARRAY_TYPE, exactly the way
 * parseTypeWithArraySuffix wraps each `[` it consumes from the token stream.
 * This is the path `let`'s *inferred*-type case needs: inferLetTypeName can
 * only hand back a type *name* string (there's no token stream to walk for an
 * inferred type), so the explicit path's incremental token-stream wrapping
 * doesn't apply -- this reconstructs the same AST shape from the name alone. */
AST *buildTypeNodeFromName(const char *name, size_t len, int line, VarType *outType) {
    if (len >= 2 && name[len - 2] == '[' && name[len - 1] == ']') {
        AST *baseNode = buildTypeNodeFromName(name, len - 2, line, outType);
        if (!baseNode) return NULL;
        AST *arrType = newASTNode(AST_ARRAY_TYPE, NULL);
        setTypeAST(arrType, TYPE_ARRAY);
        setRight(arrType, baseNode);
        *outType = TYPE_ARRAY;
        return arrType;
    }
    return buildTypeNode(name, len, line, outType);
}

/* Parse a type-name token plus any trailing `[]` array suffixes, mirroring how
 * rea's parseVarDecl builds an open array type: each `[]` wraps the base type in
 * an AST_ARRAY_TYPE (var_type TYPE_ARRAY, right = base, no AST_SUBRANGE children
 * for the open dimension, exactly as rea parseArrayType does with allowOpen).
 * The current token must be the (already-current) base type name; on return the
 * lexer is positioned just past the last `]`. *outType is the resulting VarType
 * (TYPE_ARRAY when any suffix was seen). If `outAetherName` is non-NULL it is set
 * to a malloc'd Aether type-name string with `[]` appended per dimension (e.g.
 * "Int[]"), matching the rewriter's binding-table convention so `.len`/array
 * inference resolve. Returns NULL on allocation failure. */
static AST *parseTypeWithArraySuffix(AetherParser *p, VarType *outType,
                                     char **outAetherName) {
    /* Capture the base Aether type name for the binding form. */
    size_t baseLen = (size_t)p->current.length;
    char *aetherName = (char *)malloc(baseLen + 1);
    if (!aetherName) { if (outAetherName) *outAetherName = NULL; return NULL; }
    memcpy(aetherName, p->current.start, baseLen);
    aetherName[baseLen] = '\0';

    VarType vt = TYPE_UNKNOWN;
    AST *typeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &vt);
    if (!typeNode) { free(aetherName); if (outAetherName) *outAetherName = NULL; return NULL; }
    aetherAdvance(p); /* consume the base type name */

    while (p->current.type == REA_TOKEN_LEFT_BRACKET) {
        aetherAdvance(p); /* consume '[' */
        /* Only the open dimension `[]` is part of Aether's surface type syntax.
         * A `[N]` fixed-size suffix (e.g. `Int[3]`) is a hard error: the `[` is
         * already consumed and cannot be pushed back, so silently bailing here
         * used to leave the stream misaligned and surface an unrelated
         * downstream diagnostic. Report SYN-001 and consume through the
         * matching `]` so this is the only error the user sees. */
        if (p->current.type != REA_TOKEN_RIGHT_BRACKET) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "fixed-size array types are not supported.",
                    "use `Int[]` (dynamic array) and size it at runtime.");
            p->hadError = true;
            while (p->current.type != REA_TOKEN_RIGHT_BRACKET &&
                   p->current.type != REA_TOKEN_EOF) {
                aetherAdvance(p);
            }
            if (p->current.type == REA_TOKEN_RIGHT_BRACKET) aetherAdvance(p);
            break;
        }
        aetherAdvance(p); /* consume ']' */
        AST *arrType = newASTNode(AST_ARRAY_TYPE, NULL);
        setTypeAST(arrType, TYPE_ARRAY);
        setRight(arrType, typeNode);
        typeNode = arrType;
        vt = TYPE_ARRAY;
        /* Extend the Aether name with "[]". */
        size_t nlen = strlen(aetherName);
        char *grown = (char *)realloc(aetherName, nlen + 3);
        if (!grown) { free(aetherName); freeAST(typeNode); if (outAetherName) *outAetherName = NULL; return NULL; }
        aetherName = grown;
        aetherName[nlen] = '[';
        aetherName[nlen + 1] = ']';
        aetherName[nlen + 2] = '\0';
    }

    if (outType) *outType = vt;
    if (outAetherName) *outAetherName = aetherName; else free(aetherName);
    return typeNode;
}

/* ------------------------------------------------------------------ */
/* String unescaping (verbatim from rea parser.c reaUnescapeString)    */
/* ------------------------------------------------------------------ */

static char *aetherUnescapeString(const char *src, size_t len, size_t *out_len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            char n = src[++i];
            switch (n) {
                case 'n': buf[j++] = '\n'; break;
                case 'r': buf[j++] = '\r'; break;
                case 't': buf[j++] = '\t'; break;
                case '\\': buf[j++] = '\\'; break;
                case 0x27: buf[j++] = 0x27; break;
                case '"': buf[j++] = '"'; break;
                case 'x':
                case 'X': {
                    int val = 0;
                    size_t digits = 0;
                    while (i + 1 < len && digits < 2) {
                        char h = src[i + 1];
                        if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')) {
                            i++;
                            digits++;
                            val = val * 16 + (h >= '0' && h <= '9' ? h - '0' : (h & 0x5f) - 'A' + 10);
                        } else {
                            break;
                        }
                    }
                    if (digits > 0) {
                        buf[j++] = (char)val;
                    } else {
                        buf[j++] = '\\';
                        buf[j++] = n;
                    }
                    break;
                }
                default:
                    if (n >= '0' && n <= '7') {
                        int val = n - '0';
                        size_t digits = 1;
                        while (i + 1 < len && digits < 3 && src[i + 1] >= '0' && src[i + 1] <= '7') {
                            val = (val << 3) + (src[++i] - '0');
                            digits++;
                        }
                        buf[j++] = (char)val;
                    } else {
                        buf[j++] = '\\';
                        buf[j++] = n;
                    }
                    break;
            }
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    if (out_len) *out_len = j;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Procedure-table registration (mirrors rea parseFunctionDecl tail)   */
/* ------------------------------------------------------------------ */

static HashTable *aetherEnsureProcedureTable(void) {
    if (!procedure_table) {
        procedure_table = createHashTable();
    }
    if (!current_procedure_table) {
        current_procedure_table = procedure_table;
    }
    return current_procedure_table ? current_procedure_table : procedure_table;
}

/* Register a parsed function/procedure declaration so that calls resolve and
 * semantic analysis can find it -- exactly as rea's parseFunctionDecl does
 * (minus the class/module machinery, which Milestone 1 does not cover). */
static void registerFunctionSymbol(AST *func, const char *name, VarType vtype, bool hasBody,
                                    bool isMethod) {
    char lower_name[MAX_SYMBOL_LENGTH];
    strncpy(lower_name, name, sizeof(lower_name) - 1);
    lower_name[sizeof(lower_name) - 1] = '\0';
    for (int i = 0; lower_name[i]; i++) {
        lower_name[i] = (char)tolower((unsigned char)lower_name[i]);
    }

    HashTable *target_table = current_procedure_table ? current_procedure_table : procedure_table;
    if (!target_table) {
        target_table = aetherEnsureProcedureTable();
    }

    Symbol *sym = target_table ? hashTableLookup(target_table, lower_name) : NULL;
    if (sym && sym->is_alias && sym->real_symbol) {
        sym = sym->real_symbol;
    }
    if (!sym) {
        sym = (Symbol *)calloc(1, sizeof(Symbol));
        if (sym) {
            sym->name = strdup(lower_name);
            if (target_table) {
                hashTableInsert(target_table, sym);
            }
        }
    }
    bool sym_is_new = false;
    if (sym && !sym->type_def) {
        /* Freshly allocated above (no prior type_def): treat as new for aliasing. */
        sym_is_new = (strcmp(sym->name, lower_name) == 0);
    }
    if (sym) {
        sym->type = vtype;
        if (sym->type_def) {
            freeAST(sym->type_def);
        }
        sym->type_def = copyAST(func);
        if (!hasBody) {
            sym->is_defined = false;
        }
    }

    /* For a class method `Class.method`, register a bare-name alias `method` so
     * that `obj.method(...)` resolves -- exactly as rea parseFunctionDecl does
     * (rea gates this on p->currentClassName; mirror that here with `isMethod`
     * rather than just checking for a dot in the name). A module-qualified
     * name ("ModuleName.funcname") also contains a dot but must NOT get this
     * treatment: an unscoped bare alias here is shared by every module, so a
     * second module declaring a same-named private helper would silently
     * reuse (and never update) the first module's alias, making the second
     * module's private helper permanently unreachable by its own bare name. */
    if (isMethod && sym && sym_is_new && sym->name) {
        const char *dot = strrchr(sym->name, '.');
        const char *bare = (dot && *(dot + 1)) ? dot + 1 : NULL;
        if (bare && target_table && !hashTableLookup(target_table, bare)) {
            Symbol *alias = (Symbol *)calloc(1, sizeof(Symbol));
            if (alias) {
                alias->name = strdup(bare);
                alias->is_alias = true;
                alias->real_symbol = sym;
                alias->type = vtype;
                alias->type_def = copyAST(sym->type_def);
                hashTableInsert(target_table, alias);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static AST *parseFnDecl(AetherParser *p);
static AST *parseTypeDecl(AetherParser *p);
static AST *parseConstDeclTop(AetherParser *p);

static AST *parseExprFromText(AetherParser *p, const char *text, int line,
                             bool inMethodContract);

/* ------------------------------------------------------------------ */
/* Primary / call expressions (mirrors rea parseFactor primary cases)  */
/* ------------------------------------------------------------------ */

/* Is the current string token closed by its own quote? The shared lexer ends a
 * `"` or `'` literal at a newline (or end of input) when no closing quote comes,
 * and its token then has no closing quote, so stripping one byte from each end
 * used to eat the last character (`"abc` gave ab) or a `;` and a backslash
 * (`"C:\data\";`). The last byte must be the opening quote, preceded by an even
 * number of backslashes (an odd number escapes it). */
static bool aetherStringTokenIsTerminated(const ReaToken *t) {
    if (!t || !t->start || t->length < 2) return false;
    char quote = t->start[0];
    if (t->start[t->length - 1] != quote) return false;
    size_t backslashes = 0;
    for (size_t i = t->length - 1; i > 1 && t->start[i - 1] == '\\'; i--) backslashes++;
    return (backslashes % 2) == 0;
}

static AST *parseStringLiteral(AetherParser *p) {
    int startLine = p->current.line;
    size_t totalLen = 0;
    size_t capacity = 0;
    char *buffer = NULL;
    bool haveSegment = false;
    bool charCandidate = false;

    while (p->current.type == REA_TOKEN_STRING) {
        size_t tokenLen = p->current.length;
        if (!aetherStringTokenIsTerminated(&p->current)) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "unterminated string literal: no closing quote on this line.",
                    "Text literals cannot span lines; write \\n for a newline, and "
                    "\\\\ for a literal backslash (`\\\"` escapes the quote).");
            p->hadError = true;
            free(buffer);
            return NULL;
        }
        size_t innerLen = tokenLen - 2;
        size_t unescapedLen = 0;
        char *segment = aetherUnescapeString(p->current.start + 1, innerLen, &unescapedLen);
        if (!segment) { free(buffer); return NULL; }

        size_t required = totalLen + unescapedLen + 1;
        if (required > capacity) {
            size_t newCap = capacity ? capacity : 16;
            while (required > newCap) newCap *= 2;
            char *resized = (char *)realloc(buffer, newCap);
            if (!resized) { free(buffer); free(segment); return NULL; }
            buffer = resized;
            capacity = newCap;
        }
        if (unescapedLen > 0) memcpy(buffer + totalLen, segment, unescapedLen);
        totalLen += unescapedLen;
        free(segment);

        if (!haveSegment) {
            charCandidate = (p->current.start[0] == '\'' && unescapedLen == 1);
            haveSegment = true;
        } else {
            charCandidate = false;
        }
        aetherAdvance(p);
    }

    if (!haveSegment) { free(buffer); return NULL; }
    if (capacity == 0) {
        buffer = (char *)malloc(1);
        if (!buffer) return NULL;
        capacity = 1;
    }
    buffer[totalLen] = '\0';
    if (totalLen != 1) charCandidate = false;

    Token *tok = (Token *)malloc(sizeof(Token));
    if (!tok) { free(buffer); return NULL; }
    tok->type = TOKEN_STRING_CONST;
    tok->value = buffer;
    tok->length = totalLen;
    tok->line = startLine;
    tok->column = 0;
    tok->is_char_code = false;
    tok->char_code_value = 0;

    AST *node = newASTNode(AST_STRING, tok);
    node->i_val = (int)totalLen;
    setTypeAST(node, charCandidate ? TYPE_CHAR : inferStringLiteralType(buffer, totalLen));
    return node;
}

/* Parse one expression optionally followed by Pascal-style write formatting
 * specifiers `:width[:precision]`, mirroring rea parseWriteArgument: the result
 * is wrapped in AST_FORMATTED_EXPR(token=STRING "width,prec", left=expr). Only
 * valid as a write-builtin argument (println/print -> writeln/write). The token
 * line is the expression's start line, matching rea. */
static AST *parseWriteArg(AetherParser *p) {
    int expr_line = p->current.line;
    AST *expr = parseExpr(p);
    if (!expr) return NULL;
    if (p->current.type != REA_TOKEN_COLON) return expr;
    aetherAdvance(p); /* consume ':' */
    if (p->current.type != REA_TOKEN_NUMBER) {
        /* The ':' has been consumed; silently returning here used to leave the
         * stream misaligned and produce an unrelated downstream error. */
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected a number after ':' in print format spec.",
                "write value:width or value:width:precision (e.g. println(x:8:2)).");
        p->hadError = true;
        return expr;
    }
    char *wlex = (char *)malloc(p->current.length + 1);
    if (!wlex) return expr;
    memcpy(wlex, p->current.start, p->current.length);
    wlex[p->current.length] = '\0';
    int width = atoi(wlex);
    free(wlex);
    aetherAdvance(p);
    int prec = -1;
    if (p->current.type == REA_TOKEN_COLON) {
        aetherAdvance(p);
        if (p->current.type == REA_TOKEN_NUMBER) {
            char *plex = (char *)malloc(p->current.length + 1);
            if (plex) {
                memcpy(plex, p->current.start, p->current.length);
                plex[p->current.length] = '\0';
                prec = atoi(plex);
                free(plex);
            }
            aetherAdvance(p);
        } else {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected a number after ':' in print format spec.",
                    "write value:width or value:width:precision (e.g. println(x:8:2)).");
            p->hadError = true;
        }
    }
    char fmtbuf[32];
    snprintf(fmtbuf, sizeof(fmtbuf), "%d,%d", width, prec);
    Token *fmtTok = newToken(TOKEN_STRING_CONST, fmtbuf, expr_line, 0);
    AST *fmtNode = newASTNode(AST_FORMATTED_EXPR, fmtTok);
    setLeft(fmtNode, expr);
    setTypeAST(fmtNode, TYPE_UNKNOWN);
    return fmtNode;
}

/* Parse an argument list assuming '(' is the current token. Returns an
 * AST_COMPOUND whose children are the argument expressions (caller moves them
 * onto the call node, matching rea). When `isWrite` is set, each argument is
 * parsed as a write argument so `expr:w:p` format specifiers are honored (the
 * caller passes this for the write builtins, matching rea's isWriteBuiltin). */
static AST *parseArgListEx(AetherParser *p, bool isWrite) {
    int openLine = p->current.line;
    aetherAdvance(p); /* consume '(' */
    AST *args = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *arg = isWrite ? parseWriteArg(p) : parseExpr(p);
        if (!arg) { aetherReportMissingExpr(p, NULL); break; }
        addChild(args, arg);
        if (p->current.type == REA_TOKEN_COMMA) {
            aetherAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    } else if (!p->hadError) {
        char msg[96];
        snprintf(msg, sizeof(msg), "expected ')' to close argument list (opened at line %d).", openLine);
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser", msg, NULL);
        p->hadError = true;
    }
    return args;
}

static AST *parseArgList(AetherParser *p) {
    return parseArgListEx(p, false);
}

static void moveArgsOntoCall(AST *call, AST *args) {
    if (args && args->child_count > 0) {
        call->children = args->children;
        call->child_count = args->child_count;
        call->child_capacity = args->child_capacity;
        for (int i = 0; i < call->child_count; i++) {
            if (call->children[i]) call->children[i]->parent = call;
        }
        args->children = NULL;
        args->child_count = 0;
        args->child_capacity = 0;
    }
    if (args) freeAST(args);
}

/* Copy a name token from the current lexeme (identifier-like). */
Token *copyNameToken(AetherParser *p) {
    size_t len = (size_t)p->current.length;
    char *lex = (char *)malloc(len + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, len);
    lex[len] = '\0';
    Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    return tok;
}

/* Parse a record-literal initializer `{ field: value, ... }` (or the paren form
 * `( field: value, ... )`, which the rewriter treats identically) assuming the
 * opening delimiter is current. `closeTok` is the matching close token. Returns
 * AST_COMPOUND of AST_ASSIGN(left=AST_VARIABLE field, right=value,
 * token=TOKEN_ASSIGN ":"), mirroring rea's `new Class { ... }` field-init shape. */
static AST *parseRecordInitDelimited(AetherParser *p, ReaTokenType closeTok) {
    aetherAdvance(p); /* consume opening delimiter */
    AST *inits = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != closeTok && p->current.type != REA_TOKEN_EOF) {
        if (!aetherTokenIsIdentifierLike(&p->current)) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "Expected field name in record initializer.", NULL);
            p->hadError = true;
            break;
        }
        Token *fieldTok = copyNameToken(p);
        if (!fieldTok) break;
        aetherAdvance(p); /* consume field name */
        if (p->current.type != REA_TOKEN_COLON) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "Expected ':' after field name in record initializer.", NULL);
            p->hadError = true;
            freeToken(fieldTok);
            break;
        }
        Token *assignTok = newToken(TOKEN_ASSIGN, ":", fieldTok->line, 0);
        aetherAdvance(p); /* consume ':' */
        AST *value = parseExpr(p);
        if (!value) {
            freeToken(fieldTok);
            if (assignTok) freeToken(assignTok);
            break;
        }
        AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
        AST *fieldAssign = newASTNode(AST_ASSIGN, assignTok);
        setLeft(fieldAssign, fieldVar);
        setRight(fieldAssign, value);
        addChild(inits, fieldAssign);
        if (p->current.type == REA_TOKEN_COMMA) {
            aetherAdvance(p);
            continue;
        }
        break;
    }
    if (p->current.type == closeTok) {
        aetherAdvance(p);
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "Expected closing delimiter for record initializer.", NULL);
        p->hadError = true;
    }
    return inits;
}

AST *parseRecordInitBlock(AetherParser *p) {
    return parseRecordInitDelimited(p, REA_TOKEN_RIGHT_BRACE);
}

/* Apply postfix `.field` / `.method(args)` / `[index]` chains to `base`,
 * mirroring rea parseFactor's member-access loop (the bare-identifier branch:
 * no name mangling for an ordinary receiver -- the bare method name resolves via
 * the alias rea registers for each class method). `myself`/`self` receivers DO
 * get mangled to ClassName.method, matching rea. */
/* Resolve the recorded Aether type name of a bare-variable postfix receiver,
 * for tuple index bounds-checking (`let t = pair(); t.0`) -- the same
 * binding-table lookup parsePostfix already uses above for `.len` and
 * method-call receiver resolution. Returns NULL when the type was never
 * recorded (e.g. an untyped/unknown receiver); callers must treat NULL as
 * "arity unknown", not "not a tuple". Only variable receivers reach here:
 * chaining `.N` directly onto a call result is rejected earlier in
 * parsePostfix (see the comment there for why). */
static const char *aetherPostfixReceiverTypeName(AetherParser *p, AST *node) {
    if (!node || node->type != AST_VARIABLE || !node->token || !node->token->value) return NULL;
    return bindingTableGet(p->bindings, node->token->value, strlen(node->token->value));
}

static AST *parsePostfix(AetherParser *p, AST *base) {
    AST *node = base;
    while (p->current.type == REA_TOKEN_DOT || p->current.type == REA_TOKEN_LEFT_BRACKET ||
           aetherIsTupleIndexToken(&p->current)) {
        if (aetherIsTupleIndexToken(&p->current)) {
            /* Tuple field index: `t.0`, `t.1`, ... on a tuple-typed variable.
             * The rea lexer folds the dot and digits into one NUMBER token
             * (see aetherIsTupleIndexToken); there is no separate DOT token
             * to consume. Lower to the exact same AST_FIELD_ACCESS(recv,
             * "item<N>") shape destructuring already builds
             * (parseLetTupleDestructure), so semantic analysis and codegen
             * need no tuple-specific handling.
             *
             * Only a bare variable receiver is supported: chaining directly
             * onto a call result (`pair().0`) parses and passes semantic
             * analysis (resolveExprClass in rea's shared semantic.c has no
             * case for AST_PROCEDURE_CALL receivers), but pscal-core's
             * codegen (getRecordTypeFromExpr in compiler.c) also has no path
             * from a call expression to its record type, so it fails deep in
             * codegen with a confusing "Unknown field 'item0'" -- worse than
             * a clean rejection. Fixing that needs changes to how ordinary
             * (non-tuple) call expressions carry their return type through
             * codegen generally, which is out of scope here; reject early
             * with an actionable message instead. `let t = pair(); t.0;`
             * (now legal -- see the removed direct-bind rejection in
             * parseLetDeclAfterKeyword) is the supported workaround. */
            int idxLine = p->current.line;
            const char *digits = p->current.start + 1;
            size_t digitLen = p->current.length - 1;
            long index = 0;
            bool indexOverflowed = (digitLen > 9); /* generously more digits than any real tuple has */
            if (!indexOverflowed) {
                for (size_t i = 0; i < digitLen; i++) index = index * 10 + (digits[i] - '0');
            }

            if (node->type != AST_VARIABLE) {
                char detail[160];
                snprintf(detail, sizeof(detail),
                         "tuple index .%.*s access is only supported on a variable, not directly on a call result.",
                         (int)digitLen, digits);
                reportAetherAstError(aetherSemanticGetSourcePath(), idxLine, "tuple", detail,
                                     "bind the call first, for example `let t = pair(); t.0;`.");
                p->hadError = true;
                aetherAdvance(p); /* consume the folded '.N' token so parsing can continue */
                continue;
            }

            const char *recvTypeName = aetherPostfixReceiverTypeName(p, node);
            int tupleTypeId = 0;
            const AetherTupleSig *sig = NULL;
            if (recvTypeName && aetherParseSyntheticTupleTypeId(recvTypeName, &tupleTypeId)) {
                sig = tupleTableGetByTypeId(p->tuples, tupleTypeId);
            }
            if (sig && (indexOverflowed || (size_t)index >= sig->itemCount)) {
                char detail[160];
                char hint[96];
                snprintf(detail, sizeof(detail),
                         "tuple index .%.*s is out of range (tuple has %zu element%s).",
                         (int)digitLen, digits, sig->itemCount, sig->itemCount == 1 ? "" : "s");
                snprintf(hint, sizeof(hint), "valid indices are .0 through .%zu.",
                         sig->itemCount - 1);
                reportAetherAstError(aetherSemanticGetSourcePath(), idxLine, "tuple", detail, hint);
                p->hadError = true;
            }

            char fieldName[32];
            if (indexOverflowed) {
                snprintf(fieldName, sizeof(fieldName), "item%.*s", (int)digitLen, digits);
            } else {
                snprintf(fieldName, sizeof(fieldName), "item%ld", index);
            }
            Token *nameTok = newToken(TOKEN_IDENTIFIER, fieldName, idxLine, 0);
            aetherAdvance(p); /* consume the folded '.N' token */
            AST *fieldVar = newASTNode(AST_VARIABLE, nameTok);
            AST *fa = newASTNode(AST_FIELD_ACCESS, nameTok);
            setLeft(fa, node);
            setRight(fa, fieldVar);
            node = fa;
            continue;
        }
        if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
            /* array index: base[expr] -> AST_ARRAY_ACCESS (rea parseArrayAccess). */
            int openLine = p->current.line;
            aetherAdvance(p); /* consume '[' */
            if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
                /* `xs[]` has no index. It used to build an ARRAY_ACCESS with a
                 * NULL index child, which read whatever operand was pushed
                 * before it (a plausible value, exit 0) or crashed in a loop. */
                aetherAdvance(p); /* consume ']' */
                bool assigns = (p->current.type == REA_TOKEN_EQUAL);
                if (!p->hadError) {
                    reportAetherAstError(aetherSemanticGetSourcePath(), openLine, "parser",
                            "empty index `[]`: an index expression is required.",
                            assigns ? "to append, write `xs = xs + [v];`; to set an element, "
                                      "write `xs[i] = v;`."
                                    : "write `xs[i]` with an Int index (0-based).");
                }
                p->hadError = true;
                freeAST(node);
                return NULL;
            }
            AST *index = parseExpr(p);
            if (!index && p->current.type != AE_TOKEN_DOTDOT) {
                aetherReportMissingExpr(p, "as the index");
                freeAST(node);
                return NULL;
            }
            if (p->current.type == AE_TOKEN_DOTDOT) {
                /* Slice sugar: base[lo..hi]. NOT a first-class Range value --
                 * docs/ideas_and_todo.md's array-slicing entry explicitly
                 * endorses sugar scoped to indexing brackets over introducing
                 * one, matching the no-closures decision's spirit (avoid a
                 * construct that can float around ambiguously). Lowered
                 * below to a hoisted temp-array decl + copy loop. */
                ReaToken dots = p->current;
                aetherAdvance(p); /* consume '..' */
                aetherNoteOperator(p, &dots);
                /* High bound via parseAdd, not parseExpr, mirroring
                 * parseLoopRange's documented rationale: full parseExpr would
                 * over-consume into &&/comparison operators (`xs[a..b && c]`
                 * silently becoming `xs[a..(b && c)]`). The low bound above
                 * doesn't need the same guard: '..' isn't a valid expression
                 * continuation for any parse rule, so parseExpr(p) for `a`
                 * already stops cleanly at it. */
                AST *hi = parseAdd(p);
                if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
                    aetherAdvance(p);
                } else if (!p->hadError) {
                    char msg[104];
                    snprintf(msg, sizeof(msg),
                            "expected ']' to close slice expression (opened at line %d).", openLine);
                    reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser", msg, NULL);
                    p->hadError = true;
                }
                if (!index || !hi || p->hadError) {
                    if (index) freeAST(index);
                    if (hi) freeAST(hi);
                    freeAST(node);
                    return NULL;
                }
                node = buildArraySlice(p, node, index, hi, openLine);
                if (!node) { p->hadError = true; return NULL; }
                continue;
            }
            if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
                aetherAdvance(p);
            } else if (!p->hadError) {
                char msg[96];
                snprintf(msg, sizeof(msg), "expected ']' to close index expression (opened at line %d).", openLine);
                reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser", msg, NULL);
                p->hadError = true;
            }
            /* ARR-002: a second index applied to a variable declared with only
             * one `[]` (aetherCheckArrayRankIndex, ast_checks.c). */
            if (aetherCheckArrayRankIndex(p, node, openLine)) {
                freeAST(index);
                freeAST(node);
                return NULL;
            }
            AST *acc = newASTNode(AST_ARRAY_ACCESS, NULL);
            setLeft(acc, node);
            addChild(acc, index);
            setTypeAST(acc, TYPE_UNKNOWN);
            node = acc;
            continue;
        }
        /* DOT */
        aetherAdvance(p); /* consume '.' */
        if (!aetherTokenIsIdentifierLike(&p->current)) break;
        Token *nameTok = copyNameToken(p);
        if (!nameTok) break;
        aetherAdvance(p); /* consume member name */
        /* `.len` property (not a call) -> length(<receiver>). The rewriter lowers
         * `<chain>.len` to `length(<chain>)` for both Text (via string_len) and
         * array receivers (translate.c ~5137); both alias to `length`, so the AST
         * is the same regardless of receiver type. Only the property form (no
         * following '(') is rewritten; `x.len(...)` stays a method call.
         *
         * EXCEPT when the receiver is a user record/class instance: there `.len`
         * is a field read (the rewriter leaves `rec.len` as a field access). Detect
         * a bare variable whose declared type is a user type and skip the lowering
         * so it falls through to the field-access path below. */
        bool recvIsUserRecord = false;
        if (node->type == AST_VARIABLE && node->token && node->token->value) {
            const char *rty = bindingTableGet(p->bindings, node->token->value,
                                              strlen(node->token->value));
            if (rty) {
                VarType rvt = TYPE_UNKNOWN; const char *rrn = NULL;
                if (!mapAetherType(rty, strlen(rty), &rrn, &rvt)) {
                    AST *rtyNode = lookupType(rty);
                    if (rtyNode) {
                        recvIsUserRecord = true;
                        releaseTransientTypeNode(rtyNode);
                    }
                }
            }
        }
        if (!recvIsUserRecord && nameTok->value &&
            (strcmp(nameTok->value, "len") == 0 ||
             (strcmp(nameTok->value, "length") == 0 &&
              p->current.type == REA_TOKEN_LEFT_PAREN))) {
            /* `.len`, `.len()` and `.length()` are all the `length` builtin alias.
             * The builtin pre-pass rewrites `len(` -> `length(`, so the method-call
             * form arrives here as `length`; translate.c lowers all of these to
             * length(receiver). They must NOT mangle to <Type>.len|length (len/length
             * is a builtin, not a user method): once params/let-arrays carry a type the
             * mangled call `Int[].length` was undefined at runtime. The bare `.length`
             * property (no `(`) is left as a field access so a record field named
             * `length` is untouched. Swallow any arg list and emit length(receiver). */
            if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                AST *discard = parseArgList(p);
                if (discard) freeAST(discard);
            }
            freeToken(nameTok);
            Token *lenTok = newToken(TOKEN_IDENTIFIER, "length", node->token ? node->token->line : p->current.line, 0);
            AST *call = newASTNode(AST_PROCEDURE_CALL, lenTok);
            addChild(call, node);
            setTypeAST(call, TYPE_INTEGER);
            node = call;
            continue;
        }
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            /* method call recv.method(args). */
            const char *cls = NULL;
            if (node->type == AST_VARIABLE && node->token && node->token->value &&
                (strcasecmp(node->token->value, "myself") == 0 ||
                 strcasecmp(node->token->value, "my") == 0)) {
                cls = p->currentClassName;
            } else if (node->type == AST_NEW && node->token && node->token->value) {
                cls = node->token->value;
            } else if (node->type == AST_VARIABLE && node->token && node->token->value) {
                /* Bare variable receiver: resolve its declared type from the
                 * binding table and mangle to Type.method, exactly as the
                 * rewriter does (it composes <receiver-type>.<method>). */
                cls = bindingTableGet(p->bindings, node->token->value,
                                      strlen(node->token->value));
            } else if (node->type == AST_PROCEDURE_CALL && node->token && node->token->value) {
                /* Chained call receiver (`f.self_ref().mkdir(...)`): `node` is
                 * itself a previously-resolved call whose (possibly-mangled)
                 * name was recorded in funcReturns -> Aether return type when
                 * its own declaration was parsed (forward-scanned before this
                 * pass runs, so declaration order doesn't matter). Look that up
                 * to get the receiver type for THIS call, so the chain resolves
                 * recursively no matter how many calls deep it goes -- without
                 * this, a chained call fell back to an unmangled bare name and
                 * could false-positive against a same-named builtin (FX-001). */
                cls = bindingTableGet(p->funcReturns, node->token->value,
                                      strlen(node->token->value));
            }
            /* Only mangle for user class/record receivers. A builtin-typed
             * receiver (Int/Real/Text/Bool/...) has no user methods, so e.g.
             * `pct.toInt()` must stay an unmangled call -- `Real.toInt` would be an
             * undefined global; the rewriter leaves `pct.toInt()` as written. */
            bool clsIsBuiltin = false;
            if (cls) {
                VarType cvt = TYPE_UNKNOWN; const char *crn = NULL;
                clsIsBuiltin = mapAetherType(cls, strlen(cls), &crn, &cvt);
            }
            if (cls && !clsIsBuiltin) {
                size_t ln = strlen(cls) + 1 + strlen(nameTok->value) + 1;
                char *m = (char *)malloc(ln);
                if (m) {
                    snprintf(m, ln, "%s.%s", cls, nameTok->value);
                    free(nameTok->value);
                    nameTok->value = m;
                    nameTok->length = strlen(m);
                }
            }
            AST *args = parseArgList(p);
            AST *call = newASTNode(AST_PROCEDURE_CALL, nameTok);
            setLeft(call, node);
            addChild(call, node);
            if (args && args->child_count > 0) {
                for (int i = 0; i < args->child_count; i++) {
                    addChild(call, args->children[i]);
                    args->children[i] = NULL;
                }
                args->child_count = 0;
            }
            if (args) freeAST(args);
            setTypeAST(call, TYPE_UNKNOWN);
            node = call;
        } else {
            /* field access recv.field -> AST_FIELD_ACCESS(token=field, left=base,
             * right=AST_VARIABLE(field)). */
            AST *fieldVar = newASTNode(AST_VARIABLE, nameTok);
            AST *fa = newASTNode(AST_FIELD_ACCESS, nameTok);
            setLeft(fa, node);
            setRight(fa, fieldVar);
            node = fa;
        }
    }
    return node;
}

/* new ClassName [ (args) ] [ { field: value, ... } ]  ->  AST_NEW
 * (token=ClassName, children=ctor args, extra=record-init compound, POINTER),
 * mirroring rea parseFactor's REA_TOKEN_NEW handling. */
static AST *parseNew(AetherParser *p) {
    aetherAdvance(p); /* consume 'new' */
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected a class name after 'new'.", NULL);
        p->hadError = true;
        return NULL;
    }
    Token *clsTok = copyNameToken(p);
    if (!clsTok) return NULL;
    aetherAdvance(p); /* consume class name */
    AST *node = newASTNode(AST_NEW, clsTok);
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        AST *args = parseArgList(p);
        moveArgsOntoCall(node, args);
    }
    setTypeAST(node, TYPE_POINTER);
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        AST *inits = parseRecordInitBlock(p);
        setExtra(node, inits);
    }
    return parsePostfix(p, node);
}

/* if c { a } else { b }  used in VALUE position  ->  AST_TERNARY, exactly the
 * shape rea's parseConditional builds for `((c) ? (a) : (b))` (the text the
 * rewriter's rewriteInlineIfExpression emits). token = TOKEN_IF "?",
 * left=cond, right=then, extra=else; type via resolveConditionalType. */
static AST *parseIfExpr(AetherParser *p) {
    int line = p->current.line;
    aetherAdvance(p); /* consume 'if' */
    AST *cond = parseExpr(p);
    if (!cond) { aetherReportMissingExpr(p, "as the if condition"); return NULL; }
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '{' after if-expression condition.", NULL);
        p->hadError = true;
        freeAST(cond);
        return NULL;
    }
    aetherAdvance(p); /* consume '{' */
    AST *thenExpr = parseExpr(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        aetherAdvance(p);
    } else if (!p->hadError) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '}' to close if-expression branch.", NULL);
        p->hadError = true;
        freeAST(cond);
        if (thenExpr) freeAST(thenExpr);
        return NULL;
    }
    if (!thenExpr) { aetherReportMissingExpr(p, "in the if branch"); freeAST(cond); return NULL; }
    if (p->current.type != REA_TOKEN_ELSE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "if-expression requires an 'else' branch.", NULL);
        p->hadError = true;
        freeAST(cond); freeAST(thenExpr);
        return NULL;
    }
    aetherAdvance(p); /* consume 'else' */
    AST *elseExpr = NULL;
    if (p->current.type == REA_TOKEN_IF) {
        /* Chained 'else if' in value position: recurse exactly like
         * parseIfStmt's statement-position chain, so
         * `if a {x} else if b {y} else {z}` works as a single AST_TERNARY
         * tree (each nested else-if is itself a fully-typed AST_TERNARY,
         * so resolveConditionalType below needs no special-casing). */
        elseExpr = parseIfExpr(p);
        if (!elseExpr) { p->hadError = true; freeAST(cond); freeAST(thenExpr); return NULL; }
    } else {
        if (p->current.type != REA_TOKEN_LEFT_BRACE) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected '{' or 'if' after 'else' in if-expression.", NULL);
            p->hadError = true;
            freeAST(cond); freeAST(thenExpr);
            return NULL;
        }
        aetherAdvance(p); /* consume '{' */
        elseExpr = parseExpr(p);
        if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
        if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
            aetherAdvance(p);
        } else if (!p->hadError) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected '}' to close if-expression branch.", NULL);
            p->hadError = true;
            freeAST(cond); freeAST(thenExpr);
            if (elseExpr) freeAST(elseExpr);
            return NULL;
        }
        if (!elseExpr) {
            aetherReportMissingExpr(p, "in the else branch");
            freeAST(cond); freeAST(thenExpr);
            return NULL;
        }
    }

    Token *tok = newToken(TOKEN_IF, "?", line, 0);
    AST *node = newASTNode(AST_TERNARY, tok);
    setLeft(node, cond);
    setRight(node, thenExpr);
    setExtra(node, elseExpr);
    setTypeAST(node, resolveConditionalType(thenExpr, elseExpr));
    return node;
}

static AST *parsePrimary(AetherParser *p) {
    /* if-expression in value position: if c { a } else { b }. */
    if (p->current.type == REA_TOKEN_IF) {
        return parseIfExpr(p);
    }
    /* new T(...) / new T { ... } object construction. */
    if (p->current.type == REA_TOKEN_NEW || isAetherKeyword(&p->current, "new")) {
        return parseNew(p);
    }
    /* Unary minus */
    if (p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_MINUS, "-", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, right->var_type);
        return node;
    }
    /* Unary not: `!`, or the word `not` (same precedence; `and`/`or` are the
     * word forms of `&&`/`||`, see parseLogicalAnd/parseLogicalOr). */
    if (p->current.type == REA_TOKEN_BANG || isAetherKeyword(&p->current, "not")) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_NOT, "!", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, TYPE_BOOLEAN);
        return node;
    }
    /* Parenthesized expression */
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        int openLine = p->current.line;
        aetherAdvance(p);
        AST *expr = parseExpr(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            aetherAdvance(p);
        } else if (!p->hadError) {
            char msg[104];
            snprintf(msg, sizeof(msg), "expected ')' to close parenthesized expression (opened at line %d).", openLine);
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser", msg, NULL);
            p->hadError = true;
        }
        return expr;
    }
    /* Array literal `[a, b, c]` (and the empty `[]`). Mirrors rea parseFactor's
     * REA_TOKEN_LEFT_BRACKET branch: AST_ARRAY_LITERAL (var_type TYPE_ARRAY) with
     * one child per element. The empty literal yields a childless node (rea keeps
     * it when the brackets close with no elements, which is the `let xs: T[] = []`
     * shape). A trailing comma before `]` is allowed, matching rea. */
    if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
        aetherAdvance(p); /* consume '[' */
        AST *literal = newASTNode(AST_ARRAY_LITERAL, NULL);
        setTypeAST(literal, TYPE_ARRAY);
        while (p->current.type != REA_TOKEN_RIGHT_BRACKET &&
               p->current.type != REA_TOKEN_EOF) {
            AST *element = parseExpr(p);
            if (!element) {
                aetherReportMissingExpr(p, "as an array element");
                freeAST(literal);
                return NULL;
            }
            addChild(literal, element);
            if (p->current.type == REA_TOKEN_COMMA) {
                aetherAdvance(p);
                if (p->current.type == REA_TOKEN_RIGHT_BRACKET) break;
                continue;
            }
            break;
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
            aetherAdvance(p);
        } else {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "Expected ']' to close array literal.", NULL);
            p->hadError = true;
        }
        return parsePostfix(p, literal);
    }
    /* Numeric literal */
    if (p->current.type == REA_TOKEN_NUMBER) {
        size_t len = p->current.length;
        const char *start = p->current.start;
        TokenType ttype = TOKEN_INTEGER_CONST;
        VarType vtype = TYPE_INT64;
        if (len > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
            start += 2;
            len -= 2;
            ttype = TOKEN_HEX_CONST;
        } else {
            for (size_t i = 0; i < len; i++) {
                if (start[i] == '.' || start[i] == 'e' || start[i] == 'E') {
                    ttype = TOKEN_REAL_CONST;
                    vtype = TYPE_DOUBLE;
                    break;
                }
            }
        }
        char *lex = (char *)malloc(len + 1);
        if (!lex) return NULL;
        size_t lj = 0;
        for (size_t li = 0; li < len; li++) {
            if (start[li] != '_') lex[lj++] = start[li]; // strip `_` digit separators
        }
        lex[lj] = '\0';
        Token *tok = newToken(ttype, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_NUMBER, tok);
        setTypeAST(node, vtype);
        aetherAdvance(p);
        return node;
    }
    /* String literal (with adjacent-literal concatenation, like rea) */
    if (p->current.type == REA_TOKEN_STRING) {
        return parseStringLiteral(p);
    }
    /* nil literal -> AST_NIL (rea parseFactor REA_TOKEN_NIL). In an `==`/`!=`
     * comparison against an opaque TOON handle (ToonDoc/ToonNode) it is rewritten
     * to integer -1 by parseEquality, mirroring the rewriter's
     * rewriteAetherOpaqueNilComparisons. */
    if (p->current.type == REA_TOKEN_NIL) {
        Token *tok = newToken(TOKEN_NIL, "nil", p->current.line, 0);
        aetherAdvance(p);
        AST *node = newASTNode(AST_NIL, tok);
        setTypeAST(node, TYPE_NIL);
        return node;
    }
    /* Boolean literals */
    if (p->current.type == REA_TOKEN_TRUE || p->current.type == REA_TOKEN_FALSE) {
        TokenType tt = (p->current.type == REA_TOKEN_TRUE) ? TOKEN_TRUE : TOKEN_FALSE;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(tt, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_BOOLEAN, tok);
        setTypeAST(node, TYPE_BOOLEAN);
        node->i_val = (tt == TOKEN_TRUE) ? 1 : 0;
        aetherAdvance(p);
        return node;
    }
    /* `myself` keyword (rea) or `self`/`myself` identifier (Aether) inside a
     * method -> AST_VARIABLE("myself", POINTER), the receiver. The rewriter
     * rewrites `self` to `myself` in method scope (translate.c). */
    if (p->current.type == REA_TOKEN_MYSELF ||
        (p->currentClassName &&
         (isAetherKeyword(&p->current, "self") || isAetherKeyword(&p->current, "myself")))) {
        Token *tok = newToken(TOKEN_IDENTIFIER, "myself", p->current.line, 0);
        aetherAdvance(p);
        AST *node = newASTNode(AST_VARIABLE, tok);
        setTypeAST(node, TYPE_POINTER);
        return parsePostfix(p, node);
    }
    /* Identifier: bare variable or call f(args). A lowercase type-keyword token
     * (`text`, `int`, ...) reaching here is a variable/function reference (the
     * rewriter treats such words as identifiers), so accept it too. */
    if (aetherTokenIsIdentifierLike(&p->current)) {
        Token *tok = currentAsIdentifier(p);
        if (!tok) return NULL;
        int idLine = p->current.line;
        aetherAdvance(p); /* consume identifier */

        /* Bare object literal `T { f: v, ... }` used as a general expression
         * (array element, call argument, nested operand, ...) rather than
         * directly after `let x: T =` (that position is handled earlier, in
         * the let-declaration parser, and never reaches parsePrimary). Only
         * treated as a literal when the identifier actually resolves to a
         * record type -- this can't be confused with e.g. a bare variable
         * immediately followed by an unrelated `{` because that shape isn't
         * legal Aether anywhere else. Desugars via the same
         * new T() + field-assignment lowering the let-position form already
         * uses, but since there's no enclosing `let` to hang the field
         * assignments on here, they're hoisted onto a synthesized temp and
         * queued for splice into the statement currently being parsed (see
         * the parseStatement wrapper) -- this expression position then just
         * becomes a reference to that temp. */
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            VarType litVarType = TYPE_VOID;
            AST *litTypeNode = buildTypeNode(tok->value ? tok->value : "",
                                              tok->value ? strlen(tok->value) : 0,
                                              idLine, &litVarType);
            if (litTypeNode && litVarType == TYPE_POINTER) {
                AST *inits = parseRecordInitDelimited(p, REA_TOKEN_RIGHT_BRACE);
                AST *lit = newASTNode(AST_NEW, tok);
                setTypeAST(lit, TYPE_POINTER);
                setExtra(lit, inits);

                char tempName[40];
                snprintf(tempName, sizeof(tempName), "__aether_lit_%d", p->nextObjLitId++);
                Token *tempTok = newToken(TOKEN_IDENTIFIER, tempName, idLine, 0);
                AST *hoisted = buildObjectInitDecl(tempTok, litTypeNode, litVarType,
                                                   tok->value, lit, idLine);
                pushPendingObjLit(p, hoisted);
                bindingTableSet(p->bindings, tempName, tok->value);

                Token *refTok = newToken(TOKEN_IDENTIFIER, tempName, idLine, 0);
                AST *ref = newASTNode(AST_VARIABLE, refTok);
                setTypeAST(ref, litVarType);
                return parsePostfix(p, ref);
            }
            if (litTypeNode) freeAST(litTypeNode);
        }

        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            /* Only names that are immediately called get the stdlib alias
             * treatment, matching the rewriter (it rewrites `name(` spans).
             * When an alias fires, the surface spelling is kept aside so the
             * semantic pass can quote the name the user actually wrote
             * (e.g. FX-001 must say 'println', not 'writeln'). */
            char *surfaceAlias = NULL;
            const char *raw = tok->value ? tok->value : "";
            /* A user's own top-level `fn NAME` shadows an aliased builtin: the
             * text pre-pass leaves such a name verbatim (505cd44), so aliasing
             * it here would call the builtin instead of the user's function. */
            const char *canonical = aetherAstIsTopLevelUserFunction(raw)
                                        ? raw : aliasBuiltinName(raw);
            if (canonical != raw && strcmp(canonical, raw) != 0) {
                surfaceAlias = strdup(raw);
                freeToken(tok);
                tok = newToken(TOKEN_IDENTIFIER, canonical, idLine, 0);
                if (!tok) {
                    free(surfaceAlias);
                    return NULL;
                }
            }
            const char *tokValue = tok->value ? tok->value : "";
            bool isWrite = (strcasecmp(tokValue, "writeln") == 0 ||
                            strcasecmp(tokValue, "write") == 0);
            AST *args = parseArgListEx(p, isWrite);

            /* Extension-method call rewrite: `f(recv, rest...)` -> `recv.f(rest...)`
             * (the rewriter's appendAetherExtensionCallRewrite / UFCS) when the
             * first arg's type T has a registered extension method T.f. Produces the
             * same AST_PROCEDURE_CALL(token=f, left=recv, children=[recv, rest...])
             * shape rea builds for `recv.f(rest)`. */
            if (!isWrite && args && args->child_count >= 1 && tok->value) {
                AST *recv = args->children[0];
                char *recvType = inferLetTypeName(p, recv);
                if (recvType) {
                    size_t qn = strlen(recvType) + 1 + strlen(tok->value) + 1;
                    char *q = (char *)malloc(qn);
                    if (q) {
                        snprintf(q, qn, "%s.%s", recvType, tok->value);
                        bool isExt = (bindingTableGet(p->funcReturns, q, strlen(q)) != NULL);
                        free(q);
                        if (isExt) {
                            /* Build recv.f(rest...): the call's children are
                             * [recv, rest...] (recv duplicated into left), exactly
                             * as parsePostfix builds a method call. */
                            AST *mcall = newASTNode(AST_PROCEDURE_CALL, tok);
                            setLeft(mcall, recv);
                            for (int i = 0; i < args->child_count; i++) {
                                addChild(mcall, args->children[i]);
                                args->children[i] = NULL;
                            }
                            args->child_count = 0;
                            freeAST(args);
                            free(recvType);
                            setTypeAST(mcall, TYPE_UNKNOWN);
                            if (surfaceAlias) {
                                aetherAstRegisterCallSurfaceName(mcall, surfaceAlias);
                                free(surfaceAlias);
                            }
                            return parsePostfix(p, mcall);
                        }
                    }
                    free(recvType);
                }
            }

            AST *call;
            if (strcasecmp(tokValue, "writeln") == 0) {
                /* Keep the (canonical) token: the AST effect/purity checks and
                 * compiler getLine() read the true call line from it. rea
                 * builds these with a NULL token, but nothing keys on that. */
                call = newASTNode(AST_WRITELN, tok);
                freeToken(tok);
            } else if (strcasecmp(tokValue, "write") == 0) {
                call = newASTNode(AST_WRITE, tok);
                freeToken(tok);
            } else {
                call = newASTNode(AST_PROCEDURE_CALL, tok);
            }
            if (surfaceAlias) {
                aetherAstRegisterCallSurfaceName(call, surfaceAlias);
                free(surfaceAlias);
            }
            moveArgsOntoCall(call, args);
            setTypeAST(call, TYPE_UNKNOWN);
            /* `copy(arr, lo, n)` on an array. `copy` is the *string* substring
             * builtin; handed an array it failed at runtime with the uncoded
             * "Copy expects (String/Char, Integer, Integer)." -- a message that
             * never mentions that Aether does have a subarray form, the slice
             * sugar `arr[lo..hi]`. Name the right spelling instead.
             *
             * Note the bases now agree: `arr[a..b]`, `s[a..b]` and copy()'s
             * `start` are all 0-based, so the translation below is a pure
             * respelling with no index fixup. */
            if (call->type == AST_PROCEDURE_CALL && call->token &&
                call->token->value && strcasecmp(call->token->value, "copy") == 0 &&
                call->child_count >= 1 && call->children[0]) {
                char *baseTypeName = inferLetTypeName(p, call->children[0]);
                if (baseTypeName && aetherTypeNameIsArray(baseTypeName)) {
                    reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "array-copy",
                            "copy() is the substring builtin for Text; it does not "
                            "take an array.",
                            "use the slice form for a subarray: `arr[lo..hi]` "
                            "(half-open and 0-based, so copy(arr, start, count) "
                            "becomes arr[start..start + count]).");
                    p->hadError = true;
                    free(baseTypeName);
                    freeAST(call);
                    return NULL;
                }
                free(baseTypeName);
            }
            return parsePostfix(p, call);
        }
        /* Inside a method's contract expression, a bare field reference lowers to
         * `myself.<field>` -- exactly as the rewriter's rewriteMethodScopedExpr
         * does (only when it is a current-class field and not a known binding). */
        if (p->inMethodContract && tok->value &&
            p->classFields &&
            !bindingTableGet(p->bindings, tok->value, strlen(tok->value)) &&
            fieldNameListHas(p->classFields, tok->value, strlen(tok->value))) {
            Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", idLine, 0);
            AST *recv = newASTNode(AST_VARIABLE, selfTok);
            setTypeAST(recv, TYPE_POINTER);
            AST *fieldVar = newASTNode(AST_VARIABLE, tok);
            AST *fa = newASTNode(AST_FIELD_ACCESS, tok);
            setLeft(fa, recv);
            setRight(fa, fieldVar);
            return parsePostfix(p, fa);
        }
        AST *node = newASTNode(AST_VARIABLE, tok);
        setTypeAST(node, TYPE_UNKNOWN);
        return parsePostfix(p, node);
    }
    /* No expression starts at this token. Report it (SYN-001) unless the token
     * opened an expression statement, where parseBlock's "expected a
     * statement" is the better message. */
    if (!(p->stmtStartAt && p->current.start == p->stmtStartAt)) {
        aetherReportMissingExpr(p, NULL);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Binary operator ladder (mirrors rea precedence climbing)            */
/* ------------------------------------------------------------------ */

static AST *parseMul(AetherParser *p) {
    AST *node = parsePrimary(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_STAR ||
           p->current.type == REA_TOKEN_SLASH ||
           p->current.type == REA_TOKEN_INT_DIV ||
           p->current.type == REA_TOKEN_PERCENT) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        VarType lt = node->var_type, rt = right->var_type;
        TokenType tt;
        const char *lex;
        switch (op.type) {
            case REA_TOKEN_STAR:    tt = TOKEN_MUL;     lex = "*";   break;
            case REA_TOKEN_SLASH:   tt = TOKEN_SLASH;   lex = "/";   break;
            case REA_TOKEN_INT_DIV: tt = TOKEN_INT_DIV; lex = "div"; break;
            default:                tt = TOKEN_MOD;     lex = "mod"; break;
        }
        Token *tok = newToken(tt, lex, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        bool leftReal = isRealType(lt), rightReal = isRealType(rt);
        bool integerOnlyOp = (tt == TOKEN_INT_DIV || tt == TOKEN_MOD);
        bool forceReal = (tt == TOKEN_SLASH) || (!integerOnlyOp && (leftReal || rightReal));
        VarType res = forceReal ? promoteRealBinaryType(lt, rt) : promoteIntegralBinaryType(lt, rt);
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

AST *parseAdd(AetherParser *p) {
    AST *node = parseMul(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseMul(p);
        if (!right) return NULL;
        TokenType tt = (op.type == REA_TOKEN_PLUS) ? TOKEN_PLUS : TOKEN_MINUS;
        Token *tok = newToken(tt, (tt == TOKEN_PLUS) ? "+" : "-", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type, rt = right->var_type;
        VarType res;
        bool leftReal = isRealType(lt), rightReal = isRealType(rt);
        if (tt == TOKEN_PLUS && (isPascalStringType(lt) || isPascalStringType(rt) ||
                                 isPascalCharType(lt) || isPascalCharType(rt))) {
            res = inferBinaryOpType(lt, rt);
        } else if (leftReal || rightReal) {
            res = promoteRealBinaryType(lt, rt);
        } else {
            res = promoteIntegralBinaryType(lt, rt);
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

/* Shift/bitwise-and/xor/or (below) mirror rea's parseShift / parseBitwiseAnd /
 * parseBitwiseXor / parseBitwiseOr verbatim (external/rea/src/rea/parser.c),
 * consistent with the "Type promotion helpers (verbatim from rea parser.c)"
 * reuse above. Deliberately not restricted to Int operands: an identifier
 * reference is typed TYPE_UNKNOWN until a later pass resolves it (see
 * AST_VARIABLE in parsePrimary), so a parse-time Int-only check would reject
 * `x & y` for any plain variable `x`/`y` -- the real type check already lives
 * in the VM opcode handlers (vm.c `case AND: case OR: case XOR:` and `case
 * SHL: case SHR:`), which accept Int (bitwise) or Bool (eager, non-short-
 * circuit logical -- distinct from `&&`/`||`'s short-circuit evaluation) and
 * reject everything else at runtime. */
static AST *parseShift(AetherParser *p) {
    AST *node = parseAdd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_SHIFT_LEFT || p->current.type == REA_TOKEN_SHIFT_RIGHT) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseAdd(p);
        if (!right) return NULL;
        TokenType tt = (op.type == REA_TOKEN_SHIFT_LEFT) ? TOKEN_SHL : TOKEN_SHR;
        const char *lex = (tt == TOKEN_SHL) ? "<<" : ">>";
        Token *tok = newToken(tt, lex, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type, rt = right->var_type;
        VarType res = (lt == TYPE_INT64 || rt == TYPE_INT64) ? TYPE_INT64 : TYPE_INT32;
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseComparison(AetherParser *p) {
    AST *node = parseShift(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL ||
           p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseShift(p);
        if (!right) return NULL;
        TokenType tt;
        const char *lex;
        switch (op.type) {
            case REA_TOKEN_GREATER:       tt = TOKEN_GREATER;       lex = ">";  break;
            case REA_TOKEN_GREATER_EQUAL: tt = TOKEN_GREATER_EQUAL; lex = ">="; break;
            case REA_TOKEN_LESS:          tt = TOKEN_LESS;          lex = "<";  break;
            default:                      tt = TOKEN_LESS_EQUAL;    lex = "<="; break;
        }
        Token *tok = newToken(tt, lex, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

/* True if `node`'s inferred Aether type is an opaque TOON handle (ToonDoc/
 * ToonNode), the only types against which `== nil` lowers to `== -1`. */
static bool aetherOperandIsOpaqueHandle(AetherParser *p, AST *node) {
    if (!node) return false;
    char *tn = inferLetTypeName(p, node);
    bool opaque = tn && (strcmp(tn, "ToonDoc") == 0 || strcmp(tn, "ToonNode") == 0);
    free(tn);
    return opaque;
}

/* Replace an AST_NIL node with integer `-1` (the rewriter's opaque-nil lowering),
 * built as AST_UNARY_OP(-, NUMBER 1) -- the shape rea parses from the text `-1`. */
static AST *aetherNilToMinusOne(AST *nilNode) {
    int line = (nilNode && nilNode->token) ? nilNode->token->line : 0;
    if (nilNode) freeAST(nilNode);
    Token *oneTok = newToken(TOKEN_INTEGER_CONST, "1", line, 0);
    AST *one = newASTNode(AST_NUMBER, oneTok);
    setTypeAST(one, TYPE_INT64);
    one->i_val = 1;
    Token *minusTok = newToken(TOKEN_MINUS, "-", line, 0);
    AST *neg = newASTNode(AST_UNARY_OP, minusTok);
    setLeft(neg, one);
    setTypeAST(neg, TYPE_INT64);
    return neg;
}

static AST *parseEquality(AetherParser *p) {
    AST *node = parseComparison(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_EQUAL_EQUAL || p->current.type == REA_TOKEN_BANG_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseComparison(p);
        if (!right) return NULL;
        /* Opaque-handle nil comparison: `handle == nil` / `nil == handle` ->
         * `handle == -1` (rewriteAetherOpaqueNilComparisons). Only when exactly one
         * side is nil and the other is a ToonDoc/ToonNode-typed operand. */
        bool leftNil = (node->type == AST_NIL);
        bool rightNil = (right->type == AST_NIL);
        if (leftNil ^ rightNil) {
            if (rightNil && aetherOperandIsOpaqueHandle(p, node)) {
                right = aetherNilToMinusOne(right);
            } else if (leftNil && aetherOperandIsOpaqueHandle(p, right)) {
                node = aetherNilToMinusOne(node);
            }
        }
        TokenType tt = (op.type == REA_TOKEN_EQUAL_EQUAL) ? TOKEN_EQUAL : TOKEN_NOT_EQUAL;
        Token *tok = newToken(tt, (tt == TOKEN_EQUAL) ? "==" : "!=", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

/* PREC-001: `flags & mask != 0` parses as `flags & (mask != 0)`.
 *
 * `&`, `|` and `^` all bind LOOSER than the comparison operators, so a mask
 * test written without parentheses silently changes both the grouping and the
 * result TYPE -- it yields an Int, not a Bool, and no error is raised:
 *
 *     (flags & mask) != 0   ->  true   (Bool, what was meant)
 *      flags & mask != 0    ->  1      (Int)
 *
 * A permission check written the second way prints 1 and looks like it works.
 *
 * Not warned unconditionally, because `&` and `|` double as EAGER boolean
 * operators on Bool operands, where `ready & (n == 0)` is a legitimate
 * non-short-circuiting conjunction. The warning therefore fires only when the
 * left operand is provably Int -- an integer literal, or a variable whose
 * declared type is exactly `Int` -- which is the bitmask case and not the
 * boolean one. Anything whose type is not visible here is left alone. */
static int aetherNodeIsComparison(const AST *node) {
    const char *op;
    if (!node || node->type != AST_BINARY_OP || !node->token || !node->token->value) {
        return 0;
    }
    op = node->token->value;
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<") == 0  || strcmp(op, "<=") == 0 ||
           strcmp(op, ">") == 0  || strcmp(op, ">=") == 0;
}

static int aetherNodeIsDeclaredInt(AetherParser *p, const AST *node) {
    if (!node) {
        return 0;
    }
    if (node->type == AST_NUMBER) {
        return node->var_type == TYPE_INT64 || node->var_type == TYPE_INT32 ||
               node->var_type == TYPE_INTEGER;
    }
    if (node->type == AST_VARIABLE && node->token && node->token->value && p->bindings) {
        const char *t = bindingTableGet(p->bindings, node->token->value,
                                        strlen(node->token->value));
        return t && strcmp(t, "Int") == 0;
    }
    return 0;
}

static void aetherWarnBitwisePrecedence(AetherParser *p, const char *opText,
                                        const AST *left, const AST *right, int line) {
    char detail[288];
    char hint[288];
    if (!aetherNodeIsComparison(right) || !aetherNodeIsDeclaredInt(p, left)) {
        return;
    }
    snprintf(detail, sizeof(detail),
             "'%s' binds looser than '%s', so this parses as `a %s (b %s c)` and "
             "produces an Int, not a Bool.",
             opText, right->token->value, opText, right->token->value);
    snprintf(hint, sizeof(hint),
             "parenthesize the mask: `(a %s b) %s c`. As written the comparison "
             "runs first and its result is combined bitwise, which compiles and "
             "prints a number.",
             opText, right->token->value);
    reportAetherAstWarning(aetherSemanticGetSourcePath(), line, "precedence", detail, hint);
}

static AST *parseBitwiseAnd(AetherParser *p) {
    AST *node = parseEquality(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseEquality(p);
        if (!right) return NULL;
        aetherWarnBitwisePrecedence(p, "&", node, right, op.line);
        Token *tok = newToken(TOKEN_AND, "&", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type, rt = right->var_type;
        VarType res = (lt == TYPE_INT64 || rt == TYPE_INT64) ? TYPE_INT64 : TYPE_INT32;
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseBitwiseXor(AetherParser *p) {
    AST *node = parseBitwiseAnd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_XOR) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseBitwiseAnd(p);
        if (!right) return NULL;
        const char *lexeme = (strncmp(op.start, "xor", op.length) == 0) ? "xor" : "^";
        aetherWarnBitwisePrecedence(p, lexeme, node, right, op.line);
        Token *tok = newToken(TOKEN_XOR, lexeme, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type, rt = right->var_type;
        VarType res;
        if (lt == TYPE_BOOLEAN && rt == TYPE_BOOLEAN) {
            res = TYPE_BOOLEAN;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseBitwiseOr(AetherParser *p) {
    AST *node = parseBitwiseXor(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_OR) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseBitwiseXor(p);
        if (!right) return NULL;
        aetherWarnBitwisePrecedence(p, "|", node, right, op.line);
        Token *tok = newToken(TOKEN_OR, "|", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type, rt = right->var_type;
        VarType res = (lt == TYPE_INT64 || rt == TYPE_INT64) ? TYPE_INT64 : TYPE_INT32;
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseLogicalAnd(AetherParser *p) {
    AST *node = parseBitwiseOr(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND_AND || isAetherKeyword(&p->current, "and")) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseBitwiseOr(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_AND, "&&", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseLogicalOr(AetherParser *p) {
    AST *node = parseLogicalAnd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_OR_OR || isAetherKeyword(&p->current, "or")) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *right = parseLogicalAnd(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_OR, "||", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

/* Ternary conditional `c ? a : b` (mirrors rea parseConditional). Binds looser
 * than `||` but tighter than assignment. Produces AST_TERNARY(token=TOKEN_IF "?",
 * left=cond, right=then, extra=else) -- the same shape parseIfExpr builds and the
 * shape the builtin pre-pass's `(cond ? a : b)` lowering expects. The branches are
 * full expressions (rea uses parseAssignment), so recurse through parseExpr. */
static AST *parseConditional(AetherParser *p) {
    AST *cond = parseLogicalOr(p);
    if (!cond) return NULL;
    if (p->current.type != REA_TOKEN_QUESTION) return cond;
    ReaToken question = p->current;
    aetherAdvance(p); /* consume '?' */
    aetherNoteOperator(p, &question);
    AST *thenBranch = parseExpr(p);
    if (!thenBranch) { p->hadError = true; freeAST(cond); return NULL; }
    if (p->current.type != REA_TOKEN_COLON) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "Expected ':' in conditional expression.", NULL);
        p->hadError = true;
        freeAST(cond); freeAST(thenBranch);
        return NULL;
    }
    ReaToken colon = p->current;
    aetherAdvance(p); /* consume ':' */
    aetherNoteOperator(p, &colon);
    AST *elseBranch = parseExpr(p);
    if (!elseBranch) { p->hadError = true; freeAST(cond); freeAST(thenBranch); return NULL; }
    Token *tok = newToken(TOKEN_IF, "?", question.line, 0);
    AST *node = newASTNode(AST_TERNARY, tok);
    setLeft(node, cond);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);
    setTypeAST(node, resolveConditionalType(thenBranch, elseBranch));
    return node;
}

/* Assignment is right-associative and only valid with an lvalue on the left
 * (mirrors rea parseAssignment). Produces AST_ASSIGN. */
AST *parseExpr(AetherParser *p) {
    AST *left = parseConditional(p);
    if (!left) return NULL;
    if ((left->type == AST_VARIABLE || left->type == AST_FIELD_ACCESS ||
         left->type == AST_ARRAY_ACCESS) &&
        (p->current.type == REA_TOKEN_EQUAL ||
         p->current.type == REA_TOKEN_PLUS_EQUAL ||
         p->current.type == REA_TOKEN_MINUS_EQUAL ||
         p->current.type == REA_TOKEN_STAR_EQUAL ||
         p->current.type == REA_TOKEN_SLASH_EQUAL ||
         p->current.type == REA_TOKEN_PERCENT_EQUAL)) {
        ReaToken op = p->current;
        aetherAdvance(p);
        aetherNoteOperator(p, &op);
        AST *value = parseExpr(p);
        if (!value) { aetherReportMissingExpr(p, NULL); return NULL; }
        /* Mirror rea parseAssignment: a compound assignment `x OP= v` lowers to an
         * AST_ASSIGN whose token carries the arithmetic op (TOKEN_PLUS/...); the
         * backend reads the lvalue, applies OP with the value, and stores -- so the
         * lvalue is evaluated once. Plain `=` keeps TOKEN_ASSIGN. */
        TokenType assignType = TOKEN_ASSIGN;
        const char *assignLex = "=";
        if (op.type == REA_TOKEN_PLUS_EQUAL)         { assignType = TOKEN_PLUS;  assignLex = "+"; }
        else if (op.type == REA_TOKEN_MINUS_EQUAL)   { assignType = TOKEN_MINUS; assignLex = "-"; }
        else if (op.type == REA_TOKEN_STAR_EQUAL)    { assignType = TOKEN_MUL;   assignLex = "*"; }
        else if (op.type == REA_TOKEN_SLASH_EQUAL)   { assignType = TOKEN_SLASH; assignLex = "/"; }
        else if (op.type == REA_TOKEN_PERCENT_EQUAL) { assignType = TOKEN_MOD;   assignLex = "%"; }
        Token *assignTok = newToken(assignType, assignLex, op.line, 0);
        AST *node = newASTNode(AST_ASSIGN, assignTok);
        setLeft(node, left);
        setRight(node, value);
        setTypeAST(node, left->var_type);
        return node;
    }
    return left;
}

/* ------------------------------------------------------------------ */
/* Contract-expression sub-parser + guard builder (MILESTONE 3)        */
/* ------------------------------------------------------------------ */

/* Parse a standalone expression from a NUL-terminated text buffer (a contract
 * expression). A fresh lexer/parser is spun up that *shares* the parent's class
 * context, bindings, function-return table, tuple table and class field list, so
 * `self`/field references, builtin aliases and `result` all lower identically to
 * how they would in the function body. `line` stamps the produced nodes' source
 * line (the @pre/@post directive's line). With `inMethodContract` set, bare field
 * names lower to `myself.<field>`. Errors propagate via p->hadError. */
/* Recursively stamp every node's token line to `line`. A detached text-based
 * parse (parseExprFromText below) runs a fresh sub-lexer over a standalone
 * buffer, which starts its own line counter at 1 -- so every node the sub-parse
 * builds carries that fake line unless corrected. A too-early line on a
 * variable reference silently corrupts codegen: the compiler's
 * declared-after-use heuristic (CompilerLocal.decl_node's line vs. the
 * reference's line, in compileRValue's AST_VARIABLE case) treats a reference
 * whose line looks earlier than its local's declaration as out-of-scope and
 * falls back to a global lookup, producing "Undefined global variable" for a
 * perfectly valid local (this is exactly what broke tuple-return @post guards
 * referencing the per-ret temp record: only "result" was ever exercised here
 * before, and it is immune because it is registered without a decl_node). */
static void aetherStampTreeLine(AST *node, int line) {
    if (!node) return;
    if (node->token) node->token->line = line;
    aetherStampTreeLine(node->left, line);
    aetherStampTreeLine(node->right, line);
    aetherStampTreeLine(node->extra, line);
    for (int i = 0; i < node->child_count; i++) {
        aetherStampTreeLine(node->children[i], line);
    }
}

static AST *parseExprFromText(AetherParser *p, const char *text, int line,
                             bool inMethodContract) {
    if (!text) return NULL;
    AetherParser sub;
    aetherParserInit(&sub, text, p->bindings);
    sub.currentFunctionType = p->currentFunctionType;
    sub.functionDepth = p->functionDepth;
    sub.currentClassName = p->currentClassName;
    sub.funcReturns = p->funcReturns;
    sub.tuples = p->tuples;
    sub.nextTupleTypeId = p->nextTupleTypeId;
    sub.classFields = p->classFields;
    sub.inMethodContract = inMethodContract;
    sub.detachedText = true;
    aetherAdvance(&sub);
    AST *expr = parseExpr(&sub);
    if (!expr || sub.hadError) {
        if (expr) freeAST(expr);
        p->hadError = true;
        return NULL;
    }
    /* Stamp the directive line on the whole tree (not just the root) so a
     * contract error reports the @pre/@post line, not column 0 -- and so
     * variable references inside the guard carry the real source line (see
     * aetherStampTreeLine above). */
    aetherStampTreeLine(expr, line);
    return expr;
}

/* ------------------------------------------------------------------ */
/* Contract predicate operand type-checking (ANN-001)                  */
/* ------------------------------------------------------------------ */

/* Broad comparability class of an Aether type *name*. Used to reject an
 * ill-typed contract comparison (e.g. `@post result > 0` where `result` is an
 * array) at compile time rather than letting it lower to a runtime assert that
 * crashes pscal-core's VM with "Operands not comparable". UNKNOWN means "cannot
 * judge" -- the checker never rejects when either side is UNKNOWN, so inference
 * gaps stay permissive instead of producing false-positive compile errors. */
typedef enum {
    AETHER_CMP_UNKNOWN = 0,
    AETHER_CMP_SCALAR,      /* Int / Real / Bool / Text -- pscal comparable scalars */
    AETHER_CMP_COLLECTION   /* any `T[]` array                                       */
} AetherCmpClass;

static AetherCmpClass aetherCmpClassOfName(const char *name) {
    if (!name) return AETHER_CMP_UNKNOWN;
    size_t n = strlen(name);
    if (n >= 2 && name[n - 2] == '[' && name[n - 1] == ']') return AETHER_CMP_COLLECTION;
    if (strcmp(name, "Int") == 0 || strcmp(name, "Real") == 0 ||
        strcmp(name, "Bool") == 0 || strcmp(name, "Text") == 0 ||
        strcmp(name, "str") == 0)
        return AETHER_CMP_SCALAR;
    /* class / record / opaque handle (ToonDoc, ToonNode, ...) -> don't judge. */
    return AETHER_CMP_UNKNOWN;
}

/* Aether type *name* of a contract-predicate operand, for the comparability
 * check. A bare `result` in a `@post` resolves to the function's return-type
 * name (inferLetTypeName cannot see it -- `result` is never entered in the
 * binding table); every other operand defers to inferLetTypeName. Caller frees. */
static char *contractOperandTypeName(AetherParser *p, AST *node, bool isPost) {
    if (!node) return NULL;
    if (isPost && node->type == AST_VARIABLE && node->token && node->token->value &&
        strcmp(node->token->value, "result") == 0 && p->currentReturnTypeName) {
        return strdup(p->currentReturnTypeName);
    }
    return inferLetTypeName(p, node);
}

/* Recursively scan a parsed contract predicate for a comparison whose operand
 * types can never compare at runtime -- exactly one side an array, the other a
 * scalar. Reports an ANN-001 diagnostic and sets p->hadError on the first such
 * mismatch. `kind` is "pre"/"post" (only "post" resolves `result`); `line` is
 * the directive's source line (contract sub-nodes are lexed off a detached text
 * buffer, so their own token lines are not meaningful). Returns true if a
 * mismatch was reported. */
static bool checkContractComparisons(AetherParser *p, AST *node,
                                     const char *kind, int line) {
    if (!node || p->forwardScan) return false;
    if (node->type == AST_BINARY_OP && node->token &&
        (node->token->type == TOKEN_GREATER || node->token->type == TOKEN_GREATER_EQUAL ||
         node->token->type == TOKEN_LESS || node->token->type == TOKEN_LESS_EQUAL ||
         node->token->type == TOKEN_EQUAL || node->token->type == TOKEN_NOT_EQUAL)) {
        bool isPost = (kind && strcmp(kind, "post") == 0);
        char *lt = contractOperandTypeName(p, node->left, isPost);
        char *rt = contractOperandTypeName(p, node->right, isPost);
        AetherCmpClass lc = aetherCmpClassOfName(lt);
        AetherCmpClass rc = aetherCmpClassOfName(rt);
        /* Reject only the unambiguous array-vs-scalar case. Array-vs-array and
         * anything UNKNOWN are left permissive to avoid false positives. */
        bool mismatch = (lc == AETHER_CMP_COLLECTION && rc == AETHER_CMP_SCALAR) ||
                        (lc == AETHER_CMP_SCALAR && rc == AETHER_CMP_COLLECTION);
        if (mismatch) {
            const char *op = node->token->value ? node->token->value : "?";
            const char *arrName = (lc == AETHER_CMP_COLLECTION) ? lt : rt;
            const char *sclName = (lc == AETHER_CMP_COLLECTION) ? rt : lt;
            AST *arrNode = (lc == AETHER_CMP_COLLECTION) ? node->left : node->right;
            const char *arrDisp = (arrNode && arrNode->type == AST_VARIABLE &&
                                   arrNode->token && arrNode->token->value)
                                  ? arrNode->token->value : NULL;
            char detail[320];
            snprintf(detail, sizeof(detail),
                     "@%s predicate compares an array (`%s`) to a scalar (`%s`) with `%s`; "
                     "arrays and scalars are not comparable.",
                     kind ? kind : "post", arrName ? arrName : "T[]",
                     sclName ? sclName : "scalar", op);
            char hint[256];
            if (arrDisp) {
                snprintf(hint, sizeof(hint),
                         "use a collection predicate, for example `length(%s) %s 0`.",
                         arrDisp, op);
            } else {
                snprintf(hint, sizeof(hint),
                         "use a collection predicate on a length/element, for example `length(...) %s 0`.",
                         op);
            }
            reportAetherAstError(aetherSemanticGetSourcePath(), line, "contract",
                                 detail, hint);
            p->hadError = true;
            free(lt);
            free(rt);
            return true;
        }
        free(lt);
        free(rt);
    }
    /* Recurse through combined predicates (`&&`/`||`) and any nested operands. */
    if (checkContractComparisons(p, node->left, kind, line)) return true;
    if (checkContractComparisons(p, node->right, kind, line)) return true;
    if (checkContractComparisons(p, node->extra, kind, line)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (checkContractComparisons(p, node->children[i], kind, line)) return true;
    }
    return false;
}

/* Build the runtime contract guard the rewriter emits as text:
 *     if (!(EXPR)) { writeln("Aether @KIND failed in FN"); halt(1); }
 * as an AST_IF whose condition is AST_UNARY_OP(NOT, EXPR), then-branch a
 * COMPOUND[ AST_WRITELN(message), halt(1) ]. `exprText` is the (already combined
 * + scoped) contract expression; it is parsed via parseExprFromText. Returns the
 * AST_IF, or NULL on error (p->hadError set). */
AST *buildContractGuard(AetherParser *p, const char *exprText,
                       const char *kind, const char *fnName, int line) {
    if (!exprText || !*exprText) return NULL;
    AST *cond = parseExprFromText(p, exprText, line, p->currentFunctionIsMethod);
    if (!cond) return NULL;

    /* Reject a contract predicate that compares an array to a scalar (e.g.
     * `@post result > 0` on a `T[]` return) at compile time -- otherwise it
     * lowers to a runtime assert that crashes with "Operands not comparable". */
    if (checkContractComparisons(p, cond, kind, line)) {
        freeAST(cond);
        return NULL;
    }

    /* NOT(cond) */
    Token *notTok = newToken(TOKEN_NOT, "!", line, 0);
    AST *notNode = newASTNode(AST_UNARY_OP, notTok);
    setLeft(notNode, cond);
    setTypeAST(notNode, TYPE_BOOLEAN);

    /* writeln("Aether @KIND failed in FN") */
    size_t mlen = strlen("Aether @") + strlen(kind ? kind : "") +
                  strlen(" failed in ") + strlen(fnName ? fnName : "") + 1;
    char *msg = (char *)malloc(mlen);
    if (!msg) { freeAST(notNode); p->hadError = true; return NULL; }
    snprintf(msg, mlen, "Aether @%s failed in %s", kind ? kind : "", fnName ? fnName : "");
    Token *strTok = (Token *)malloc(sizeof(Token));
    if (!strTok) { free(msg); freeAST(notNode); p->hadError = true; return NULL; }
    strTok->type = TOKEN_STRING_CONST;
    strTok->value = msg;
    strTok->length = strlen(msg);
    strTok->line = line;
    strTok->column = 0;
    strTok->is_char_code = false;
    strTok->char_code_value = 0;
    AST *strNode = newASTNode(AST_STRING, strTok);
    strNode->i_val = (int)strlen(msg);
    setTypeAST(strNode, TYPE_STRING);
    AST *writelnNode = newASTNode(AST_WRITELN, NULL);
    addChild(writelnNode, strNode);
    setTypeAST(writelnNode, TYPE_VOID);

    /* halt(1) */
    Token *haltTok = newToken(TOKEN_IDENTIFIER, "halt", line, 0);
    AST *haltCall = newASTNode(AST_PROCEDURE_CALL, haltTok);
    Token *oneTok = newToken(TOKEN_INTEGER_CONST, "1", line, 0);
    AST *oneNode = newASTNode(AST_NUMBER, oneTok);
    setTypeAST(oneNode, TYPE_INT64);
    addChild(haltCall, oneNode);
    setTypeAST(haltCall, TYPE_VOID);

    AST *thenBlock = newASTNode(AST_COMPOUND, NULL);
    addChild(thenBlock, writelnNode);
    addChild(thenBlock, haltCall);
    /* The guard's failure path calls writeln/halt: compiler machinery, not
     * user code. Exempt it from the AST effect/purity checks. (Only the
     * then-block -- the user's contract expression itself stays checked.) */
    aetherAstRegisterSynthesizedSubtree(thenBlock);

    AST *ifNode = newASTNode(AST_IF, NULL);
    setLeft(ifNode, notNode);
    setRight(ifNode, thenBlock);
    return ifNode;
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

/* let/const NAME [ : Type ] [ = expr ] ;
 *
 * Produces AST_VAR_DECL identical to rea's `Type name = init;` form:
 *   child[0] = AST_VARIABLE(name, var_type),
 *   left  = initializer expr (or NULL),
 *   right = type node,
 *   var_type = mapped type.
 *
 * Three shapes are handled to match the rewriter:
 *   - explicit type:        `let x: T = e;`   -> typed AST_VAR_DECL
 *   - object literal:       `let x: T = T{..}`-> new T() + field assignments
 *   - inferred (no type):   `let x = e;`      -> type inferred from `e`
 * Block-level `const` is handled by parseConstDeclTop (AST_CONST_DECL), matching
 * the rewriter which lowers a local `const` to a Rea `const`, not a typed var. */
/* May this initializer evaluate to a Real? A literal, variable, call or
 * element whose inferred type is Real, a `/` (Int / Int is Real outside a typed
 * sink), or + - * and unary minus over such an operand. Conservative the other
 * way: `div`, `mod`, comparisons and anything not inferable say no. */
static bool aetherLetInitMayBeReal(AetherParser *p, AST *e, int depth) {
    if (!e || depth > 8) return false;
    if (e->type == AST_BINARY_OP && e->token && e->token->value) {
        const char *op = e->token->value;
        if (strcmp(op, "/") == 0) return true;
        if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0) {
            return aetherLetInitMayBeReal(p, e->left, depth + 1) ||
                   aetherLetInitMayBeReal(p, e->right, depth + 1);
        }
        return false;
    }
    if (e->type == AST_UNARY_OP) return aetherLetInitMayBeReal(p, e->left, depth + 1);
    if (e->type == AST_PROCEDURE_CALL && e->token && e->token->value) {
        const char *fn = e->token->value;
        if (aetherIsAlwaysRealBuiltin(fn)) return true;
        if (strcmp(fn, "random") == 0 && e->child_count == 0) return true;
    }
    char *t = inferLetTypeName(p, e);
    bool isReal = t && strcmp(t, "Real") == 0;
    free(t);
    return isReal;
}

static bool aetherVarTypeIsIntFamily(VarType t) {
    return t == TYPE_INT64 || t == TYPE_INT32 || t == TYPE_INTEGER || t == TYPE_INT16 ||
           t == TYPE_INT8 || t == TYPE_BYTE || t == TYPE_WORD;
}

static AST *parseLetDeclAfterKeyword(AetherParser *p, int kwLine) {
    /* `let` has already been consumed by the caller (which peeked for `(`). */
    /* Optional `mut` modifier: Rea bindings are mutable already, so accept and
     * ignore it, matching the rewriter (which tolerates `let mut x ...`). */
    if (isAetherKeyword(&p->current, "mut")) {
        aetherAdvance(p); /* consume 'mut' */
    }
    if (!aetherTokenIsIdentifierLike(&p->current)) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected name after 'let'.", NULL);
        p->hadError = true;
        return NULL;
    }
    Token *nameTok = currentAsIdentifier(p);
    if (!nameTok) return NULL;
    aetherAdvance(p); /* consume name */

    AST *typeNode = NULL;
    VarType vtype = TYPE_UNKNOWN;
    char *declaredTypeName = NULL; /* Aether type name for binding + obj-init    */
    bool explicitType = false;
    if (p->current.type == REA_TOKEN_COLON) {
        explicitType = true;
        aetherAdvance(p); /* consume ':' */
        if (p->current.type == REA_TOKEN_EOF || p->current.type == REA_TOKEN_EQUAL ||
            p->current.type == REA_TOKEN_SEMICOLON) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected type after ':'.", NULL);
            p->hadError = true;
            freeToken(nameTok);
            return NULL;
        }
        typeNode = parseTypeWithArraySuffix(p, &vtype, &declaredTypeName);
        if (!typeNode) { freeToken(nameTok); free(declaredTypeName); return NULL; }
    }

    AST *init = NULL;
    if (p->current.type == REA_TOKEN_EQUAL) {
        ReaToken eq = p->current;
        aetherAdvance(p); /* consume '=' */
        aetherNoteOperator(p, &eq);

        /* Inline object-method: `let x = T { f: v, ... }.method(args);`. The
         * rewriter hoists the inline object literal into a temp
         * `__aether_obj_<serial>`, assigns its fields, then binds x to
         * `temp.method(args)` (translate.c). Detect `IDENT { ... } .` (a known type
         * name, balanced braces, then a dot) and build that temp/splice. */
        if (p->current.type == REA_TOKEN_IDENTIFIER) {
            /* Confirm IDENT is a user type and is followed by `{ ... } .`. */
            char *probeName = (char *)malloc(p->current.length + 1);
            bool looksInlineObj = false;
            if (probeName) {
                memcpy(probeName, p->current.start, p->current.length);
                probeName[p->current.length] = '\0';
            }
            VarType pvt = TYPE_UNKNOWN; const char *prn = NULL;
            bool isBuiltinTy = probeName && mapAetherType(probeName, strlen(probeName), &prn, &pvt);
            AST *resolvedTy = (probeName && !isBuiltinTy) ? lookupType(probeName) : NULL;
            bool isUserType = (resolvedTy != NULL);
            releaseTransientTypeNode(resolvedTy);
            if (isUserType) {
                ReaToken save = p->current;
                int savedHead = p->queueHead, savedCount = p->queueCount;
                ReaToken q0 = p->queue[0], q1 = p->queue[1], q2 = p->queue[2];
                ReaLexer savedLexer = p->lexer;
                aetherAdvance(p); /* past type name */
                if (p->current.type == REA_TOKEN_LEFT_BRACE) {
                    int depth = 0;
                    while (p->current.type != REA_TOKEN_EOF) {
                        if (p->current.type == REA_TOKEN_LEFT_BRACE) depth++;
                        else if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
                            depth--;
                            if (depth == 0) { aetherAdvance(p); break; }
                        }
                        aetherAdvance(p);
                    }
                    looksInlineObj = (depth == 0 && p->current.type == REA_TOKEN_DOT);
                }
                /* restore to the type name */
                p->lexer = savedLexer;
                p->queueHead = savedHead; p->queueCount = savedCount;
                p->queue[0] = q0; p->queue[1] = q1; p->queue[2] = q2;
                p->current = save;
            }
            if (looksInlineObj && probeName) {
                /* Build the temp object: __aether_obj_<serial> = new T(); field = v; */
                Token *clsTok = copyNameToken(p);
                aetherAdvance(p); /* consume type name */
                VarType objVt = TYPE_UNKNOWN;
                AST *objTypeNode = buildTypeNode(probeName, strlen(probeName), kwLine, &objVt);
                AST *newNode = newASTNode(AST_NEW, clsTok);
                setTypeAST(newNode, TYPE_POINTER);
                AST *objInits = parseRecordInitBlock(p);

                char tempName[64];
                snprintf(tempName, sizeof(tempName), "__aether_obj_%d", p->nextObjLitId++);
                /* Register the temp's type so its method call resolves. */
                bindingTableSet(p->bindings, tempName, probeName);

                AST *splice = newASTNode(AST_COMPOUND, NULL);
                splice->i_val = 1;
                Token *tvTok = newToken(TOKEN_IDENTIFIER, tempName, kwLine, 0);
                AST *tv = newASTNode(AST_VARIABLE, tvTok);
                setTypeAST(tv, objVt);
                AST *tdecl = newASTNode(AST_VAR_DECL, NULL);
                addChild(tdecl, tv);
                setLeft(tdecl, newNode);
                setRight(tdecl, objTypeNode);
                setTypeAST(tdecl, objVt);
                addChild(splice, tdecl);
                if (objInits) {
                    for (int i = 0; i < objInits->child_count; i++) {
                        AST *fa = objInits->children[i];
                        if (!fa || fa->type != AST_ASSIGN || !fa->left || !fa->left->token) continue;
                        Token *rTok = newToken(TOKEN_IDENTIFIER, tempName, kwLine, 0);
                        AST *r = newASTNode(AST_VARIABLE, rTok);
                        setTypeAST(r, objVt);
                        Token *fTok = newToken(TOKEN_IDENTIFIER, fa->left->token->value, kwLine, 0);
                        AST *fv = newASTNode(AST_VARIABLE, fTok);
                        AST *facc = newASTNode(AST_FIELD_ACCESS, fTok);
                        setLeft(facc, r);
                        setRight(facc, fv);
                        Token *aTok = newToken(TOKEN_ASSIGN, "=", kwLine, 0);
                        AST *as = newASTNode(AST_ASSIGN, aTok);
                        setLeft(as, facc);
                        setRight(as, fa->right);
                        fa->right = NULL;
                        setTypeAST(as, as->right ? as->right->var_type : TYPE_UNKNOWN);
                        addChild(splice, as);
                    }
                    freeAST(objInits);
                }
                /* Now parse `.method(args)` (and any further postfix) on the temp. */
                Token *recvTok = newToken(TOKEN_IDENTIFIER, tempName, kwLine, 0);
                AST *recvVar = newASTNode(AST_VARIABLE, recvTok);
                setTypeAST(recvVar, objVt);
                AST *callChain = parsePostfix(p, recvVar);
                if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);

                /* Infer x's type from the resulting expression / method return. */
                char *inferred = inferLetTypeName(p, callChain);
                AST *xTypeNode = NULL; VarType xvt = TYPE_UNKNOWN;
                if (inferred) {
                    /* Same record->POINTER resolution as the plain inferred-let
                     * path below: route through buildTypeNode so a method chain
                     * that returns a record/class type (e.g. `let x =
                     * Foo{}.makeBar();`) gets an AST_POINTER_TYPE receiver rather
                     * than a bare TYPE_UNKNOWN reference. Builtin and unknown-type
                     * cases are handled exactly as the old inline code did. */
                    xTypeNode = buildTypeNode(inferred, strlen(inferred), kwLine, &xvt);
                    bindingTableSet(p->bindings, nameTok->value, inferred);
                    free(inferred);
                }
                AST *xVar = newASTNode(AST_VARIABLE, nameTok);
                setTypeAST(xVar, xvt);
                AST *xDecl = newASTNode(AST_VAR_DECL, NULL);
                addChild(xDecl, xVar);
                setLeft(xDecl, callChain);
                setRight(xDecl, xTypeNode);
                setTypeAST(xDecl, xvt);
                addChild(splice, xDecl);

                aetherAlignSpliceLines(splice, tdecl);
                free(probeName);
                free(declaredTypeName);
                if (typeNode) freeAST(typeNode);
                return splice;
            }
            free(probeName);
        }

        /* Detect a bare object literal `T { ... }` or paren form `T( f: v, ... )`:
         * an identifier matching the declared type immediately followed by '{'
         * (always an init) or '(' whose first token pair is `name :` (named
         * field init -- distinguishes it from a plain constructor call). The
         * rewriter treats both as object-init only when the type name matches the
         * declared type, so require an explicit type. */
        if (explicitType && p->current.type == REA_TOKEN_IDENTIFIER &&
            declaredTypeName &&
            (size_t)p->current.length == strlen(declaredTypeName) &&
            strncmp(p->current.start, declaredTypeName, p->current.length) == 0) {
            /* Saved so a non-literal (`let n: Int = Int(x);`, `let p: P = P.make();`)
             * re-parses from the type name as an ordinary expression. */
            ReaToken nameSave = p->current;
            int nameSavedHead = p->queueHead, nameSavedCount = p->queueCount;
            ReaToken nq0 = p->queue[0], nq1 = p->queue[1], nq2 = p->queue[2];
            ReaLexer nameSavedLexer = p->lexer;
            Token *clsTok = copyNameToken(p);
            int litLine = p->current.line;
            aetherAdvance(p); /* consume type name */

            ReaTokenType closeTok = REA_TOKEN_EOF;
            bool isObjectLiteral = false;
            if (p->current.type == REA_TOKEN_LEFT_BRACE) {
                isObjectLiteral = true;
                closeTok = REA_TOKEN_RIGHT_BRACE;
            } else if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                /* Peek two tokens: IDENT ':' marks a named-field paren init. */
                ReaToken save = p->current;
                int savedHead = p->queueHead, savedCount = p->queueCount;
                ReaToken q0 = p->queue[0], q1 = p->queue[1], q2 = p->queue[2];
                ReaLexer savedLexer = p->lexer;
                aetherAdvance(p); /* consume '(' */
                bool named = (p->current.type == REA_TOKEN_IDENTIFIER);
                if (named) {
                    aetherAdvance(p); /* consume field name */
                    named = (p->current.type == REA_TOKEN_COLON);
                }
                /* restore to just-after-type-name (current = '(') */
                p->lexer = savedLexer;
                p->queueHead = savedHead; p->queueCount = savedCount;
                p->queue[0] = q0; p->queue[1] = q1; p->queue[2] = q2;
                p->current = save;
                if (named) { isObjectLiteral = true; closeTok = REA_TOKEN_RIGHT_PAREN; }
            }

            if (isObjectLiteral) {
                AST *lit = newASTNode(AST_NEW, clsTok);
                setTypeAST(lit, TYPE_POINTER);
                AST *inits = parseRecordInitDelimited(p, closeTok);
                setExtra(lit, inits);
                if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
                if (declaredTypeName)
                    bindingTableSet(p->bindings,
                                    nameTok->value, declaredTypeName);
                AST *objDecl = buildObjectInitDecl(nameTok, typeNode, vtype,
                                                   declaredTypeName, lit, litLine);
                free(declaredTypeName);
                return objDecl;
            }
            /* Not an object literal after all: rewind to the type name and parse
             * the whole initializer as an expression. Continuing from a bare
             * AST_VARIABLE for the type name made `Int(x)` a call on a variable
             * named Int (SCOPE-001 'Int' not in scope) and dropped any operator
             * after it. */
            freeToken(clsTok);
            p->lexer = nameSavedLexer;
            p->queueHead = nameSavedHead; p->queueCount = nameSavedCount;
            p->queue[0] = nq0; p->queue[1] = nq1; p->queue[2] = nq2;
            p->current = nameSave;
            init = parseExpr(p);
        } else {
            init = parseExpr(p);
        }
        if (!init) {
            aetherReportMissingExpr(p, NULL);
            freeToken(nameTok);
            if (typeNode) freeAST(typeNode);
            free(declaredTypeName);
            return NULL;
        }
    }
    if (aetherTailRejectJuxtaposed(p)) {
        freeToken(nameTok);
        if (typeNode) freeAST(typeNode);
        free(declaredTypeName);
        if (init) freeAST(init);
        return NULL;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }

    /* Array-append initializer: `let x: T[] = src + [items...];` / `let x =
     * src + [items...];`, for an array literal of any length (0, 1, or more).
     * Rewrite `init` in place to just `src` (a plain array-copy initializer,
     * which the normal declaration path below already handles correctly --
     * arrays are value types) and remember `items`/`appendItemCount`/
     * `appendLine` so a setlength+indexed-assign pair per item can be spliced
     * in after the declaration once `decl` is built. Without this, `src +
     * [items...]` compiles as a literal `ARRAY + ARRAY_LITERAL` binary op the
     * VM has no operator for ("Runtime Error: Operands must be numbers for
     * arithmetic operation '+'... Got ARRAY and ARRAY") -- the only shape that
     * ever worked was the single-element self-reassignment statement `xs = xs
     * + [v];`, handled separately below in parseStmt (and, before this
     * generalization, only for exactly one element there too). */
    /* Both shapes above, for a chain of ANY length: `let x = a + [1] + ys + f();`.
     * The collector peels the whole left spine, leaving `init` as the innermost
     * operand (a plain array expression the ordinary declaration path handles)
     * and returning the rest in apply order. Ordinary Text/Int/Real `+` is left
     * alone -- the walk stops at the first right operand that is not manifestly
     * array-valued.
     *
     * Previously these were two single-shot blocks that peeled exactly ONE
     * operand each, so `a + b` worked and `a + b + c` left an array `+` as the
     * initializer, which reached the VM as "Operands must be numbers for
     * arithmetic operation '+' ... Got ARRAY and ARRAY". */
    AetherConcatOperand *chainOps = NULL;
    int chainOpCount = 0;
    if (!aetherCollectConcatChain(p, &init, &chainOps, &chainOpCount)) {
        freeAST(init);
        free(declaredTypeName);
        p->hadError = true;
        return NULL;
    }
    int appendLine = chainOpCount > 0 ? chainOps[0].line : 0;

    /* Binding a tuple-return call to a single name (`let v = pair();`) used to
     * be rejected outright here, forcing `let (a, b) = pair();` destructuring
     * as the only way to consume a tuple. That restriction predated tuple
     * .0/.1/.2 field-index access (see parsePostfix): without a way to read a
     * single element later, a bound-but-undestructured tuple was useless, so
     * disallowing it early gave a clearer error than "unused variable" ever
     * would have. Now that `.N` field access exists, `v` is a normal
     * variable of the synthesized tuple record type (bindingTableSet below
     * records that type name, same as any other let), and later `v.0`/`v.1`
     * read its slots with compile-time bounds checking. */

    /* Inferred type: derive from the initializer, like the rewriter. */
    if (!explicitType) {
        if (!init) {
            char detail[192];
            snprintf(detail, sizeof(detail), "'%s' requires a type or an initializer.",
                     nameTok->value ? nameTok->value : "let");
            reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "declaration",
                    detail,
                    "add an explicit type (`let x: Int;`) or an initializer (`let x = 0;`).");
            p->hadError = true;
            freeToken(nameTok);
            return NULL;
        }
        char *inferred = inferLetTypeName(p, init);
        if (!inferred) {
            const char *vn = nameTok->value ? nameTok->value : "";
            char detail[256];
            char hint[256];
            /* An initializer that calls a name which does not exist lands here
             * too, because there is no return type to infer from. Reporting
             * that as "cannot infer the type of 'x'" with a hint to add an
             * annotation actively sends the reader the wrong way: annotating
             * makes the *real* error (SCOPE-001, unknown callee) appear
             * instead, so the two codes look like unrelated problems and the
             * hint reads as having caused the second one. One model chasing
             * this concluded the compiler was non-deterministic and burned a
             * dozen turns on it (docs/ideas_and_todo.md, 2026-07-26). Name the
             * unknown callee up front when we can prove it is unknown --
             * neither a declared top-level function nor a registered builtin. */
            if (init->type == AST_PROCEDURE_CALL && init->token && init->token->value) {
                const char *callee = init->token->value;
                const char *canonical = aliasBuiltinName(callee);
                if (!canonical) {
                    canonical = callee;
                }
                if (!aetherAstIsTopLevelUserFunction(callee) &&
                    !aetherAstIsTopLevelUserFunction(canonical) &&
                    getVmBuiltinID(canonical) < 0 &&
                    (!p->funcReturns ||
                     !bindingTableGet(p->funcReturns, callee, strlen(callee)))) {
                    char scopeDetail[256];
                    snprintf(scopeDetail, sizeof(scopeDetail),
                             "identifier '%s' not in scope.", callee);
                    reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "scope",
                            scopeDetail,
                            "this helper does not exist -- check the name against the "
                            "guide's builtin list, or define it before use.");
                    p->hadError = true;
                    freeToken(nameTok);
                    freeAST(init);
                    return NULL;
                }
            }
            snprintf(detail, sizeof(detail),
                     "cannot infer the type of '%s' from its initializer.", vn);
            snprintf(hint, sizeof(hint),
                     "add an explicit type, for example `let %s: Int = ...;`.", vn);
            reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "declaration",
                                 detail, hint);
            p->hadError = true;
            freeToken(nameTok);
            freeAST(init);
            return NULL;
        }
        /* Build the declared type node from the inferred name through the same
         * helper the explicit `: T` path uses (parseTypeWithArraySuffix ->
         * buildTypeNode). Critically, buildTypeNode resolves a user record/class
         * name to an AST_POINTER_TYPE -> AST_TYPE_REFERENCE(RECORD) with var_type
         * POINTER (object variables are pointers). The prior inline construction
         * emitted a bare AST_TYPE_REFERENCE at TYPE_UNKNOWN for every user type,
         * so an inferred `let c = new C();` left `c` untyped and a later
         * statement-level `c.inc();` (a `-> Void` method) tripped pscal-core's
         * "argument 1 to 'c.inc' expects type POINTER but got VOID" -- whereas the
         * annotated `let c: C = new C();` type-checked. buildTypeNode handles the
         * builtin-keyword case (AST_TYPE_IDENTIFIER) and the unknown-type fallback
         * (bare AST_TYPE_REFERENCE, TYPE_UNKNOWN) exactly as the old code did. */
        typeNode = buildTypeNodeFromName(inferred, strlen(inferred), kwLine, &vtype);
        if (!typeNode) {
            free(inferred);
            freeToken(nameTok);
            freeAST(init);
            p->hadError = true;
            return NULL;
        }
        declaredTypeName = inferred; /* take ownership for binding below */
    }

    if (declaredTypeName)
        bindingTableSet(p->bindings, nameTok->value, declaredTypeName);
    bool declIsArrayType = declaredTypeName && aetherTypeNameIsArray(declaredTypeName);
    free(declaredTypeName);

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);
    if (explicitType) {
        aetherAstRegisterExplicitTypedDecl(decl);
    }
    if (p->functionDepth == 0 && chainOpCount == 0 && explicitType && init &&
        aetherVarTypeIsIntFamily(vtype) &&
        aetherLetInitMayBeReal(p, init, 0)) {
        /* Main-program `let n: Int = <Real>;` -> `let n: Int; n = <Real>;`. A
         * main-program binding is a global (or a main-block slot) whose
         * initializing store is strict and aborted with an uncoded "Type
         * mismatch. Cannot assign REAL to INT64"; the assignment store
         * truncates, as a fn-local `let` does. NARROW-001 still warns, now on
         * the assignment. No trunc() wrapper: it would build an INT32. */
        decl->left = NULL;
        Token *nTok = newToken(TOKEN_IDENTIFIER, nameTok->value, kwLine, 0);
        AST *target = newASTNode(AST_VARIABLE, nTok);
        setTypeAST(target, vtype);
        Token *aTok = newToken(TOKEN_ASSIGN, "=", kwLine, 0);
        AST *assign = newASTNode(AST_ASSIGN, aTok);
        setLeft(assign, target);
        setRight(assign, init);
        setTypeAST(assign, vtype);
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        outer->i_val = 1; /* splice into the surrounding block */
        addChild(outer, decl);
        addChild(outer, assign);
        return outer;
    }
    if (chainOpCount > 0) {
        /* `let x: T[] = a + b + ...;` -- `decl` above declares `x` as a copy of
         * the chain's innermost operand (init was rewritten further up); splice
         * one operand's worth of steps after it, in order, appending to `x` in
         * place. Works for any chain length. */
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        outer->i_val = 1; /* splice into the surrounding block, like buildArrayAppend's */
        addChild(outer, decl);

        /* Every step past the first setlength un-aliases `x` from its source for
         * free. A chain made only of empty literals (`src + []`) emits none, so
         * it still needs the explicit un-alias the single-operand path used to
         * add -- otherwise `x[0] = 9` would silently mutate `src`. */
        bool emitsAnyStep = false;
        for (int i = 0; i < chainOpCount; i++) {
            if (chainOps[i].other || chainOps[i].itemCount > 0) { emitsAnyStep = true; break; }
        }
        if (!emitsAnyStep && aetherArrayInitMayAlias(init)) {
            addChild(outer, buildArrayUnaliasStmt(var, appendLine));
        }

        for (int i = 0; i < chainOpCount; i++) {
            if (!aetherEmitConcatOperand(p, outer, var, &chainOps[i])) {
                aetherFreeConcatOperands(chainOps, chainOpCount, true);
                freeAST(outer);
                return NULL;
            }
        }
        aetherFreeConcatOperands(chainOps, chainOpCount, false);
        aetherAlignSpliceLines(outer, decl);
        return outer;
    }
    if (declIsArrayType && aetherArrayInitMayAlias(init)) {
        /* `let x: T[] = src;` (or `= f();`) -- Aether arrays are value types
         * on assignment, but the VM's dynamic arrays are reference types, so
         * the plain declaration copy above may alias `src`'s storage (a
         * concat-/setlength-built source; literal-built static arrays already
         * copy-on-write). Splice the un-alias step so both construction
         * styles behave identically -- without it, `x[0] = 99` after this
         * silently mutates `src` too. */
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        outer->i_val = 1; /* splice into the surrounding block */
        addChild(outer, decl);
        addChild(outer, buildArrayUnaliasStmt(var, kwLine));
        aetherAlignSpliceLines(outer, decl);
        return outer;
    }
    return decl;
}

/* if cond { then } [else { else }]  ->  AST_IF (mirrors rea parseIf). The
 * effect wrapper rules don't apply here; this is the statement form. */
static AST *parseIfStmt(AetherParser *p) {
    aetherAdvance(p); /* consume 'if' */
    /* Parens around the condition are optional -- a redundant C-style
     * `if (cond) { }` spelling is allowed -- but a leading '(' does not
     * necessarily wrap the *whole* condition (e.g. `if (a) && !c { }`).
     * Rather than special-casing a leading paren as if it always closes
     * the condition, just hand the whole condition to parseExpr, exactly
     * as parseLoop does for its condition form; the general expression
     * grammar already parses balanced parens anywhere within it. */
    AST *condition = parseExpr(p);
    if (!condition) {
        /* `if x == { ... }` used to build an IF with a NULL condition that ran
         * neither branch, exit 0. */
        aetherReportMissingExpr(p, "as the if condition");
        return NULL;
    }
    AST *thenBranch = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        thenBranch = parseBlock(p);
    } else {
        thenBranch = parseStatement(p);
    }
    AST *elseBranch = NULL;
    if (p->current.type == REA_TOKEN_ELSE) {
        aetherAdvance(p);
        if (p->current.type == REA_TOKEN_IF) {
            elseBranch = parseIfStmt(p);
        } else if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            elseBranch = parseBlock(p);
        } else {
            elseBranch = parseStatement(p);
        }
    }
    AST *node = newASTNode(AST_IF, NULL);
    setLeft(node, condition);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);
    return node;
}

/* `loop` dispatcher. The rewriter recognizes three `loop` shapes and lowers them
 * to (translate.c):
 *   - `loop NAME in LOW..HIGH { }` -> C-for -> while  (parseLoopRange)
 *   - `loop while EXPR { }`        -> `while (EXPR) { }`  (redundant spelling)
 *   - `loop EXPR { }`              -> `while (EXPR) { }`
 *   - `loop { }`                   -> `while (true) { }`
 * Detection: `loop {` is infinite; `loop IDENT in ...` is the range form (peek
 * one token past the identifier for `in`); anything else is a condition. */
static AST *parseLoop(AetherParser *p) {
    aetherAdvance(p); /* consume 'loop' */

    /* `loop while EXPR { }` -- a redundant spelling models emit; the rewriter
     * swallows the `while` filler and lowers it to a plain `while (EXPR) { }`.
     * Consume the optional `while` and fall through to the forms below. */
    if (p->current.type == REA_TOKEN_WHILE || isAetherKeyword(&p->current, "while")) {
        aetherAdvance(p); /* consume 'while' */
    }

    /* Infinite loop: `loop { }` -> while (true). */
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        Token *trueTok = newToken(TOKEN_TRUE, "true", p->current.line, 0);
        AST *cond = newASTNode(AST_BOOLEAN, trueTok);
        setTypeAST(cond, TYPE_BOOLEAN);
        cond->i_val = 1;
        AST *body = parseBlock(p);
        return buildWhile(cond, body);
    }

    /* Range form: `loop NAME in ...`. Peek the token after a leading identifier
     * for the `in` keyword without consuming the stream permanently. */
    if (aetherTokenIsIdentifierLike(&p->current)) {
        ReaToken save = p->current;
        int savedHead = p->queueHead, savedCount = p->queueCount;
        ReaToken q0 = p->queue[0], q1 = p->queue[1], q2 = p->queue[2];
        ReaLexer savedLexer = p->lexer;
        aetherAdvance(p); /* tentatively consume the identifier */
        bool isRange = isAetherKeyword(&p->current, "in");
        /* restore to the identifier */
        p->lexer = savedLexer;
        p->queueHead = savedHead; p->queueCount = savedCount;
        p->queue[0] = q0; p->queue[1] = q1; p->queue[2] = q2;
        p->current = save;
        if (isRange) {
            return parseLoopRange(p);
        }
    }

    /* Condition form: `loop EXPR { }` -> while (EXPR) { }. */
    AST *cond = parseExpr(p);
    if (!cond) { p->hadError = true; return NULL; }
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '{' to open loop body.", NULL);
        p->hadError = true;
        freeAST(cond);
        return NULL;
    }
    AST *body = parseBlock(p);
    return buildWhile(cond, body);
}

/* `for NAME in LOW..HIGH { }` -- a spelling of the range loop. The rewriter
 * lowers `for` ranges identically to `loop` ranges. */
static AST *parseForLoop(AetherParser *p) {
    aetherAdvance(p); /* consume 'for' */
    return parseLoopRange(p);
}

/* `while EXPR { }` -- a spelling of the condition loop; the rewriter lowers it to
 * `while (EXPR) { }`, identical to `loop EXPR`. */
static AST *parseWhileLoop(AetherParser *p) {
    aetherAdvance(p); /* consume 'while' */
    AST *cond = parseExpr(p);
    if (!cond) { p->hadError = true; return NULL; }
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '{' to open while body.", NULL);
        p->hadError = true;
        freeAST(cond);
        return NULL;
    }
    AST *body = parseBlock(p);
    return buildWhile(cond, body);
}

/* Peek the token after `current` without consuming it (the same save/restore
 * the range-loop and object-literal detectors use). */
static ReaTokenType aetherPeekNextTokenType(AetherParser *p) {
    ReaToken save = p->current;
    int savedHead = p->queueHead, savedCount = p->queueCount;
    ReaToken q0 = p->queue[0], q1 = p->queue[1], q2 = p->queue[2];
    ReaLexer savedLexer = p->lexer;
    aetherAdvance(p);
    ReaTokenType next = p->current.type;
    p->lexer = savedLexer;
    p->queueHead = savedHead; p->queueCount = savedCount;
    p->queue[0] = q0; p->queue[1] = q1; p->queue[2] = q2;
    p->current = save;
    return next;
}

/* Keywords from other languages at statement start. They lex as ordinary
 * identifiers (aetherDemoteForeignKeyword), so without this check
 * `return n * n;` would parse as two expression statements and fail late with
 * a misleading SCOPE-001 on `return`. Name the Aether form instead. Words
 * that are plausible variable names (`match`, `case`, `do`, ...) count as
 * foreign syntax only when the next token cannot continue an expression
 * statement -- `match = true;` and `case(x)` stay ordinary. */
typedef struct {
    const char *word;
    const char *hint;
    bool always;
} AetherForeignKeyword;

static const AetherForeignKeyword *aetherForeignStatementKeyword(const ReaToken *t) {
    static const AetherForeignKeyword table[] = {
        { "return",    "Aether returns with `ret`: write `ret value;`, or bare `ret;` in a Void function.", true },
        { "var",       "declare bindings with `let name: Type = value;` -- a `let` is already mutable.", true },
        { "def",       "declare functions with `fn name(arg: Type) -> ReturnType { ... }`.", true },
        { "func",      "declare functions with `fn name(arg: Type) -> ReturnType { ... }`.", true },
        { "function",  "declare functions with `fn name(arg: Type) -> ReturnType { ... }`.", true },
        { "class",     "records are `type Name { field: Type; fn method() -> T { ... } }`; there is no class keyword.", true },
        { "struct",    "records are `type Name { field: Type; }`; there is no struct keyword.", true },
        { "interface", "there are no interfaces or traits; use a `type` with methods.", true },
        { "trait",     "there are no interfaces or traits; use a `type` with methods.", true },
        { "enum",      "there is no enum type; use `const` values or a `Text` label.", true },
        { "import",    "imports are written `use \"module_name\";`.", true },
        { "include",   "imports are written `use \"module_name\";`.", true },
        { "require",   "imports are written `use \"module_name\";`.", true },
        { "elif",      "spell it `else if`.", true },
        { "foreach",   "iterate with `loop item in items { ... }`.", true },
        { "match",     "there is no match or switch statement; use an `if ... else if ... else` chain.", false },
        { "switch",    "there is no match or switch statement; use an `if ... else if ... else` chain.", false },
        { "case",      "there is no match or switch statement; use an `if ... else if ... else` chain.", false },
        { "try",       "Aether has no exceptions; return a status value and check it at the call site.", false },
        { "catch",     "Aether has no exceptions; return a status value and check it at the call site.", false },
        { "throw",     "Aether has no exceptions; return a status value and check it at the call site.", false },
        { "raise",     "Aether has no exceptions; return a status value and check it at the call site.", false },
        { "until",     "loops are `loop cond { }`, `loop i in a..b { }`, `loop item in items { }` or `loop { ... break; }`.", false },
        { "repeat",    "loops are `loop cond { }`, `loop i in a..b { }`, `loop item in items { }` or `loop { ... break; }`.", false },
        { "do",        "loops are `loop cond { }`, `loop i in a..b { }`, `loop item in items { }` or `loop { ... break; }`.", false },
    };
    if (!t || t->type != REA_TOKEN_IDENTIFIER) return NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (tokTextIs(t, table[i].word)) return &table[i];
    }
    return NULL;
}

/* True when `next` can continue an expression statement that starts with an
 * identifier: `name = ...`, `name(...)`, `name.field`, `name[i]`, `name;` and
 * the compound assignments. Anything else after a foreign keyword means the
 * word was meant as syntax. */
static bool aetherNextContinuesIdentifierStatement(ReaTokenType next) {
    switch (next) {
        case REA_TOKEN_EQUAL: case REA_TOKEN_LEFT_PAREN: case REA_TOKEN_DOT:
        case REA_TOKEN_LEFT_BRACKET: case REA_TOKEN_SEMICOLON:
        case REA_TOKEN_PLUS_EQUAL: case REA_TOKEN_MINUS_EQUAL:
        case REA_TOKEN_STAR_EQUAL: case REA_TOKEN_SLASH_EQUAL:
        case REA_TOKEN_PERCENT_EQUAL: case REA_TOKEN_PLUS_PLUS:
        case REA_TOKEN_MINUS_MINUS:
            return true;
        default:
            return false;
    }
}

/* D39 census arms (AETHER_EXPERIMENT=tail=..., src/aether/experiment.h). */
static void reportAetherAstErrorWithCode(int line, const char *code, const char *detail,
                                         const char *hint) {
    const char *path = aetherSemanticGetSourcePath();
    aetherDiagf("%s:%d: [%s] Aether parser error: %s\n", path ? path : "<aether>",
                line > 0 ? line : 1, code, detail);
    if (hint && *hint) aetherDiagf("hint: %s\n", hint);
    aetherReportGuideHelp(code);
}

/* A keyword that opens a statement or declaration: after an expression on
 * the same line it means a missing `;` or a foreign modifier (`pure fn`), not
 * a deleted operator, and those keep their own diagnostics. */
static bool aetherCurrentIsStatementWord(const AetherParser *p) {
    static const char *const kStatementWords[] = {
        "fn", "let", "const", "type", "mod", "use", "import", "if", "else", "loop",
        "while", "for", "ret", "return", "fx", "par", "break", "continue", "export",
    };
    for (size_t i = 0; i < sizeof(kStatementWords) / sizeof(kStatementWords[0]); i++) {
        if (isAetherKeyword(&p->current, kStatementWords[i])) return true;
    }
    return p->current.type == REA_TOKEN_IF || p->current.type == REA_TOKEN_WHILE ||
           p->current.type == REA_TOKEN_FOR || p->current.type == REA_TOKEN_BREAK ||
           p->current.type == REA_TOKEN_CONTINUE;
}

bool aetherTailRejectJuxtaposed(AetherParser *p) {
    if (aetherExperiment()->tail != AETHER_TAIL_REJECT || p->forwardScan) return false;
    if (p->current.type == REA_TOKEN_SEMICOLON || p->current.type == REA_TOKEN_RIGHT_BRACE ||
        p->current.type == REA_TOKEN_EOF || p->current.line != p->prevLine)
        return false;
    if (aetherCurrentIsStatementWord(p)) return false;
    char detail[200];
    snprintf(detail, sizeof(detail), "missing operator or `=` between `%.*s` and `%.*s`?",
             p->prevLength > 40 ? 40 : p->prevLength, p->prevStart ? p->prevStart : "",
             p->current.length > 40 ? 40 : (int)p->current.length, p->current.start);
    reportAetherAstErrorWithCode(p->current.line, "SYN-001", detail,
                                 "two expressions on one line need an operator between them, "
                                 "or a `;` if they are separate statements.");
    p->hadError = true;
    return true;
}

/* An expression whose only effect is its value: discarding it is a mistake. */
static bool aetherIsDiscardableValue(const AST *e) {
    if (!e) return false;
    switch (e->type) {
        case AST_VARIABLE: case AST_NUMBER: case AST_STRING: case AST_BOOLEAN: case AST_NIL:
        case AST_BINARY_OP: case AST_UNARY_OP: case AST_TERNARY: case AST_ARRAY_ACCESS:
        case AST_FIELD_ACCESS: case AST_ARRAY_LITERAL: case AST_FORMATTED_EXPR:
            return true;
        default:
            return false;
    }
}

/* tail=reject: a value computed and thrown away. */
static bool aetherTailRejectDiscarded(AetherParser *p, const AST *expr, int line) {
    if (aetherExperiment()->tail != AETHER_TAIL_REJECT || p->forwardScan ||
        !aetherIsDiscardableValue(expr))
        return false;
    /* `pure fn ...`: a foreign modifier word, which keeps today's diagnostic. */
    if (expr->type == AST_VARIABLE && p->current.line == p->prevLine &&
        aetherCurrentIsStatementWord(p))
        return false;
    if (p->functionDepth > 0 && p->currentFunctionType != TYPE_VOID) {
        reportAetherAstErrorWithCode(line, "FLOW-001",
            "value computed and discarded; Aether has no implicit return.",
            "write `ret <expr>;` (a branch value: `ret if c { a } else { b };`).");
    } else {
        reportAetherAstErrorWithCode(line, "SYN-001", "this expression has no effect.",
            "assign it (`x = ...;`), print it inside fx, or delete it.");
    }
    p->hadError = true;
    return true;
}

/* tail=ret (D39 (b)): the final bare value of a non-Void body, or of each
 * branch of a final if/else, becomes `ret <value>;` (Rust-style). Not applied
 * under @post or a tuple return, whose `ret` lowering is more than a RETURN. */
static int aetherTailRetIn(AetherParser *p, AST *stmt, AST **slot);

static int aetherTailRetBlock(AetherParser *p, AST *block) {
    if (!block || block->child_count == 0) return 0;
    AST **slot = &block->children[block->child_count - 1];
    return aetherTailRetIn(p, *slot, slot);
}

static int aetherTailRetIn(AetherParser *p, AST *stmt, AST **slot) {
    if (!stmt) return 0;
    if (stmt->type == AST_COMPOUND) return aetherTailRetBlock(p, stmt);
    if (stmt->type == AST_IF) {
        if (!stmt->extra) return 0; /* without else, not a value */
        int n = aetherTailRetIn(p, stmt->right, &stmt->right);
        return n + aetherTailRetIn(p, stmt->extra, &stmt->extra);
    }
    AST *value = NULL;
    if (stmt->type == AST_EXPR_STMT && aetherIsDiscardableValue(stmt->left)) value = stmt->left;
    if (stmt->type == AST_EXPR_STMT && stmt->left && stmt->left->type == AST_PROCEDURE_CALL &&
        stmt->left->token && stmt->left->token->value && p->funcReturns) {
        const char *rt = bindingTableGet(p->funcReturns, stmt->left->token->value,
                                         strlen(stmt->left->token->value));
        if (rt && strcmp(rt, "Void") != 0) value = stmt->left;
    }
    if (!value) return 0;
    Token *rtok = newToken(TOKEN_RETURN, "return", value->token ? value->token->line : 0, 0);
    AST *ret = newASTNode(AST_RETURN, rtok);
    stmt->left = NULL;
    setLeft(ret, value);
    setTypeAST(ret, value->var_type);
    ret->parent = stmt->parent;
    *slot = ret;
    freeAST(stmt);
    return 1;
}

static AST *parseStatementInner(AetherParser *p) {
    /* Empty statement `;` -- consume it and return a no-op block. Without this, a
     * stray/trailing `;` (e.g. `fx {…};`, or a bare `;`) would fall to the
     * expression-statement path, parseExpr would return NULL, and parseBlock's
     * `if (!stmt) break` would silently truncate the rest of the block (no output). */
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
        return newASTNode(AST_COMPOUND, NULL);
    }
    /* fx { ... } -- effect wrapper. The marker is erased from the AST shape
     * (we return the inner block directly, an AST_COMPOUND), but the block is
     * REGISTERED as an effect region so the semantic pass can enforce the
     * FX-001 effect fence and the @pure/fx exclusion on the real AST. */
    if (isAetherKeyword(&p->current, "fx")) {
        int fxLine = p->current.line;
        aetherAdvance(p); /* consume 'fx' */
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            AST *fxBlock = parseBlock(p);
            if (fxBlock) aetherAstRegisterFxBlock(fxBlock, fxLine);
            return fxBlock;
        }
        /* `fx` with no following block: a no-op block. */
        return newASTNode(AST_COMPOUND, NULL);
    }
    if (isAetherKeyword(&p->current, "let")) {
        int kwLine = p->current.line;
        aetherAdvance(p); /* consume 'let' */
        /* `let (a, b) = call();` -> tuple destructuring. */
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            return parseLetTupleDestructure(p, kwLine);
        }
        return parseLetDeclAfterKeyword(p, kwLine);
    }
    if (p->current.type == REA_TOKEN_CONST || isAetherKeyword(&p->current, "const")) {
        return parseConstDeclTop(p); /* AST_CONST_DECL; depth-aware folding */
    }
    if (p->current.type == REA_TOKEN_TYPE || isAetherKeyword(&p->current, "type")) {
        /* Nested `type` declaration inside a function body. parseTypeDecl returns
         * the same is_global_scope bundle [type-decl, methods...] the top-level path
         * emits; returning it here lets a model declare a local record + methods
         * (the rewriter maps `type`->`class`, which Rea also accepts in a body). */
        return parseTypeDecl(p);
    }
    if (isAetherKeyword(&p->current, "ret")) {
        return parseRet(p);
    }
    if (isAetherKeyword(&p->current, "loop")) {
        return parseLoop(p); /* range / condition / infinite forms */
    }
    if (p->current.type == REA_TOKEN_FOR || isAetherKeyword(&p->current, "for")) {
        return parseForLoop(p); /* `for i in a..b` */
    }
    if (p->current.type == REA_TOKEN_WHILE || isAetherKeyword(&p->current, "while")) {
        return parseWhileLoop(p); /* `while cond { }` -> while loop */
    }
    if (isAetherKeyword(&p->current, "par")) {
        return parseParBlock(p); /* `par { ... }` -> spawn/join block */
    }
    if (p->current.type == REA_TOKEN_BREAK || isAetherKeyword(&p->current, "break")) {
        aetherAdvance(p); /* consume 'break' */
        if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
        return newASTNode(AST_BREAK, NULL); /* rea parseBreak shape */
    }
    if (p->current.type == REA_TOKEN_CONTINUE || isAetherKeyword(&p->current, "continue")) {
        aetherAdvance(p); /* consume 'continue' */
        if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
        return newASTNode(AST_CONTINUE, NULL); /* rea parseContinue shape */
    }
    if (p->current.type == REA_TOKEN_IF) {
        return parseIfStmt(p);
    }
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        return parseBlock(p);
    }
    /* Foreign-language statement keywords: name the Aether form (SYN-001). */
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        const AetherForeignKeyword *fk = aetherForeignStatementKeyword(&p->current);
        if (fk && (fk->always ||
                   !aetherNextContinuesIdentifierStatement(aetherPeekNextTokenType(p)))) {
            char detail[128];
            snprintf(detail, sizeof(detail), "'%s' is not Aether syntax.", fk->word);
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                                 detail, fk->hint);
            p->hadError = true;
            return NULL;
        }
    }
    /* Expression statement or assignment. */
    p->stmtStartAt = p->current.start;
    int exprLine = p->current.line;
    AST *expr = parseExpr(p);
    if (!expr) return NULL;
    if (aetherTailRejectJuxtaposed(p) || aetherTailRejectDiscarded(p, expr, exprLine)) {
        freeAST(expr);
        return NULL;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }
    if (expr->type == AST_ASSIGN) {
        /* Array append: `target = src + [items...]` (an array literal of any
         * length). Lower it to the rewriter's setlength + indexed-assign
         * expansion -- one pair per item -- whether `src` is `target` itself
         * (the original self-reassignment idiom, `xs = xs + [v];`) or a
         * different array (`target = src + [v];` / `let target: T[] = src +
         * [v];` below) -- any shape but a single-element literal previously
         * fell through to a raw `ARRAY + ARRAY_LITERAL` binary op the VM has
         * no operator for ("Operands must be numbers for arithmetic operation
         * '+'... Got ARRAY and ARRAY"). `target` must still be an assignable
         * lvalue chain; `src` can be any expression of the right array type. */
        AST *target = expr->left;
        AST *rhs = expr->right;

        /* Chained concat in assignment position: `d = [0] + d + [3];`. The two
         * single-operand branches below each peel exactly one operand, so a
         * chain left a raw array `+` for the VM ("Got ARRAY and ARRAY") -- the
         * same defect the declaration and `ret` paths had. Handled here only
         * when there are 2+ operands; a single operand falls through to the
         * original, well-tested branches. */
        if (target && rhs && rhs->type == AST_BINARY_OP && rhs->token &&
            rhs->token->type == TOKEN_PLUS && aetherIsLValueChain(target) &&
            aetherConcatChainLength(p, rhs) >= 2) {
            AST *probe = rhs;
            AetherConcatOperand *ops = NULL;
            int opCount = 0;
            if (aetherCollectConcatChain(p, &probe, &ops, &opCount) && opCount >= 2) {
                int line = expr->token ? expr->token->line : p->current.line;
                AST *outer = newASTNode(AST_COMPOUND, NULL);
                outer->i_val = 1;

                /* Any operand that READS the destination must be captured before
                 * the copy below overwrites it -- see aetherExprReadsLValue. The
                 * base is safe: `target = base` reads it before assigning. */
                for (int i = 0; i < opCount; i++) {
                    if (!ops[i].other || !aetherExprReadsLValue(ops[i].other, target)) continue;
                    VarType preVt = TYPE_UNKNOWN;
                    AST *preTypeNode = buildTypeNodeFromName(ops[i].otherTypeName,
                                                             strlen(ops[i].otherTypeName),
                                                             line, &preVt);
                    if (!preTypeNode) continue;
                    static int chainPreSerial = 0;
                    char preName[64];
                    snprintf(preName, sizeof(preName), "__aether_concat_cpre_%d_%d",
                             line, ++chainPreSerial);
                    AST *preDecl = newASTNode(AST_VAR_DECL, NULL);
                    addChild(preDecl, buildVarRef(preName, preVt, line));
                    setLeft(preDecl, ops[i].other);
                    setRight(preDecl, preTypeNode);
                    setTypeAST(preDecl, preVt);
                    addChild(outer, preDecl);
                    ops[i].other = buildVarRef(preName, preVt, line);
                }

                if (!aetherLValueEqual(target, probe)) {
                    Token *aTok = newToken(TOKEN_ASSIGN, "=", line, 0);
                    AST *copyAssign = newASTNode(AST_ASSIGN, aTok);
                    setLeft(copyAssign, copyAST(target));
                    setRight(copyAssign, probe);
                    setTypeAST(copyAssign, target->var_type);
                    AST *copyStmt = newASTNode(AST_EXPR_STMT, copyAssign->token);
                    setLeft(copyStmt, copyAssign);
                    addChild(outer, copyStmt);
                } else {
                    freeAST(probe);
                }

                bool ok = true;
                for (int i = 0; i < opCount && ok; i++) {
                    ok = aetherEmitConcatOperand(p, outer, target, &ops[i]);
                }
                if (!ok) {
                    aetherFreeConcatOperands(ops, opCount, true);
                    freeAST(outer);
                    freeAST(expr);
                    return NULL;
                }
                aetherFreeConcatOperands(ops, opCount, false);
                expr->right = NULL;
                freeAST(expr); /* target was copied; the `+` shells are gone */
                return outer;
            }
            /* Guarded by aetherConcatChainLength >= 2 above, so this is only
             * reached on an allocation failure inside the collector. */
            aetherFreeConcatOperands(ops, opCount, true);
            freeAST(probe);
            expr->right = NULL;
            freeAST(expr);
            p->hadError = true;
            return NULL;
        }

        if (target && rhs && rhs->type == AST_BINARY_OP && rhs->token &&
            rhs->token->type == TOKEN_PLUS &&
            rhs->right && rhs->right->type == AST_ARRAY_LITERAL &&
            aetherIsLValueChain(target)) {
            int line = expr->token ? expr->token->line : p->current.line;
            /* Move the elements out of the literal so they survive the free. */
            int itemCount = rhs->right->child_count;
            AST **items = itemCount > 0 ? (AST **)malloc(sizeof(AST *) * (size_t)itemCount) : NULL;
            for (int i = 0; i < itemCount; i++) {
                items[i] = rhs->right->children[i];
                rhs->right->children[i] = NULL;
                if (items[i]) items[i]->parent = NULL;
            }
            rhs->right->child_count = 0;
            AST *result = buildArrayAppend(expr, target, items, itemCount, line, rhs->left);
            free(items);
            return result;
        }
        /* General concatenation: `target = src + other;` where `other` is an
         * array-*valued expression*, not a literal (e.g. `ys = ys + two;`).
         * Only fire when `other`'s inferred type is manifestly an array
         * ("[]"-suffixed) -- this must not intercept ordinary Text/Int/Real
         * `+` (string concatenation, arithmetic), which already work as raw
         * VM ops and must keep working unchanged. */
        if (target && rhs && rhs->type == AST_BINARY_OP && rhs->token &&
            rhs->token->type == TOKEN_PLUS &&
            rhs->right && rhs->right->type != AST_ARRAY_LITERAL &&
            aetherIsLValueChain(target)) {
            char *otherTypeName = inferLetTypeName(p, rhs->right);
            if (otherTypeName && aetherTypeNameIsArray(otherTypeName)) {
                int line = expr->token ? expr->token->line : p->current.line;
                AST *other = rhs->right;
                rhs->right = NULL;
                other->parent = NULL;
                return buildArrayConcat(p, expr, target, other, otherTypeName, line, rhs->left);
            }
            free(otherTypeName);
        }
        /* Plain array assignment: `target = src;` where `target` is
         * array-typed and `src` may share storage (see buildArrayUnaliasStmt).
         * Same value-semantics guarantee as the `let` path: splice the
         * un-alias step after the assignment so a later `target[i] = v`
         * cannot silently mutate `src` (or vice versa). */
        if (target && rhs && expr->token && expr->token->type == TOKEN_ASSIGN &&
            aetherIsLValueChain(target) && aetherArrayInitMayAlias(rhs)) {
            /* Array-typed? Prefer the LHS's inferred type; when it can't be
             * named at parse time (a field access -- there is no parser-side
             * field-type table), fall back to the RHS: a well-typed program
             * never assigns an array-valued RHS to a non-array lvalue. */
            char *lhsTypeName = inferLetTypeName(p, target);
            bool isArrayAssign;
            if (lhsTypeName) {
                isArrayAssign = aetherTypeNameIsArray(lhsTypeName);
            } else {
                char *rhsTypeName = inferLetTypeName(p, rhs);
                isArrayAssign = rhsTypeName && aetherTypeNameIsArray(rhsTypeName);
                free(rhsTypeName);
            }
            free(lhsTypeName);
            if (isArrayAssign) {
                int line = expr->token->line;
                AST *outer = newASTNode(AST_COMPOUND, NULL);
                outer->i_val = 1; /* splice into the surrounding block */
                addChild(outer, expr);
                addChild(outer, buildArrayUnaliasStmt(target, line));
                return outer;
            }
        }
        return expr; /* assignments act as statements directly */
    }
    AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
    setLeft(stmt, expr);
    return stmt;
}

/* Wraps parseStatementInner to flush any object literals hoisted while
 * parsing this statement's expressions (see the parsePrimary bare-object-
 * literal branch). Uses a mark/release pattern on p->pendingObjLits rather
 * than assuming the list is empty on entry: parseStatementInner recurses
 * into parseStatement for nested single-statement bodies (an unbraced `if`/
 * `loop` body), and each such nested call already flushes its own hoists
 * before returning here, so in practice the mark is always the count at
 * entry -- but marking explicitly keeps this correct regardless of that
 * nesting shape rather than relying on it.
 *
 * When hoists exist, the result is an AST_COMPOUND (i_val==1) containing the
 * flattened hoisted var-decl/field-assign statements followed by the real
 * statement, in that order -- parseBlock already knows how to splice an
 * i_val==1 compound's children in as siblings (used for the let-position
 * object-literal expansion), so this composes with zero changes there. */
AST *parseStatement(AetherParser *p) {
    int mark = p->pendingObjLitCount;
    AST *stmt = parseStatementInner(p);
    if (p->pendingObjLitCount <= mark) {
        return stmt;
    }
    AST *wrapper = newASTNode(AST_COMPOUND, NULL);
    wrapper->i_val = 1;
    for (int i = mark; i < p->pendingObjLitCount; i++) {
        AST *hoisted = p->pendingObjLits[i];
        if (!hoisted) continue;
        if (hoisted->type == AST_COMPOUND && hoisted->i_val == 1) {
            for (int j = 0; j < hoisted->child_count; j++) {
                if (hoisted->children[j]) addChild(wrapper, hoisted->children[j]);
                hoisted->children[j] = NULL;
            }
            hoisted->child_count = 0;
            freeAST(hoisted);
        } else {
            addChild(wrapper, hoisted);
        }
        p->pendingObjLits[i] = NULL;
    }
    p->pendingObjLitCount = mark;
    /* `stmt` may itself be a declaration-group splice (i_val==1) -- e.g. the
     * let-position concat/append/object-init expansions, which put the new
     * variable's decl inside that compound. Flatten it in rather than nesting
     * it, exactly as the hoists above are flattened: parseBlock only splices
     * ONE level, so a splice compound nested under this wrapper would survive
     * as a real block and scope the declared variable away from the following
     * statements. That is what broke `let r: Int[] = a[1..3] + b;` -- the slice
     * hoist forced this wrapper to exist, the concat splice became its child,
     * and every later use of `r` reported "[SCOPE-001] identifier 'r' not in
     * scope". Either half alone is fine; only the combination nests. */
    if (stmt) {
        if (stmt->type == AST_COMPOUND && stmt->i_val == 1) {
            for (int i = 0; i < stmt->child_count; i++) {
                if (stmt->children[i]) addChild(wrapper, stmt->children[i]);
                stmt->children[i] = NULL;
            }
            stmt->child_count = 0;
            freeAST(stmt);
        } else {
            addChild(wrapper, stmt);
        }
    }
    return wrapper;
}

AST *parseBlock(AetherParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    int openLine = p->current.line;
    aetherAdvance(p); /* consume '{' */
    AST *block = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) {
            /* A NULL statement with no reported error means the parser stalled
             * on a token it cannot start a statement with. Silently breaking
             * here used to truncate the rest of the block; make it a hard
             * SYN-001 instead. */
            if (!p->hadError) {
                reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                        "unexpected token in block; expected a statement.", NULL);
                p->hadError = true;
            }
            break;
        }
        /* Splice a declaration-group wrapper (object-init expansion, i_val==1) so
         * its var-decl + field assignments become siblings of this block -- the
         * flat shape the rewriter emits, keeping the new variable in scope for
         * later statements. */
        if (stmt->type == AST_COMPOUND && stmt->i_val == 1) {
            for (int i = 0; i < stmt->child_count; i++) {
                if (stmt->children[i]) addChild(block, stmt->children[i]);
                stmt->children[i] = NULL;
            }
            stmt->child_count = 0;
            freeAST(stmt);
            continue;
        }
        addChild(block, stmt);
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        aetherAdvance(p);
    } else if (!p->hadError) {
        char msg[96];
        snprintf(msg, sizeof(msg), "expected '}' to close block (opened at line %d).", openLine);
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser", msg, NULL);
        p->hadError = true;
    }
    return block;
}

/* ------------------------------------------------------------------ */
/* Function declarations                                               */
/* ------------------------------------------------------------------ */

static void aetherFreePending(AetherPendingContracts *pending) {
    if (!pending) return;
    free(pending->preExpr);
    free(pending->postExpr);
    pending->preExpr = NULL;
    pending->postExpr = NULL;
    pending->isPure = 0;
}

/* Parse a parenthesized type list `(T, U, ...)` (a tuple type) from the raw text
 * span [start,end). On success fills `*outItems` with malloc'd Aether type-name
 * strings (caller frees) and returns true. Returns false if the span is not a
 * tuple type (e.g. a scalar like `Int`, or `()`), mirroring translate.c
 * parseTupleTypeList: requires a leading '(' and at least one ',' inside. */
static bool parseTupleTypeList(const char *start, const char *end,
                              char ***outItems, size_t *outCount) {
    if (!start || !end || end <= start) return false;
    while (start < end && isspace((unsigned char)*start)) start++;
    const char *tail = end;
    while (tail > start && isspace((unsigned char)tail[-1])) tail--;
    if (tail - start < 2 || *start != '(' || tail[-1] != ')') return false;
    const char *inner = start + 1;
    const char *innerEnd = tail - 1;

    char **items = NULL;
    size_t count = 0, cap = 0;
    const char *cursor = inner;
    int depth = 0;
    bool sawComma = false;
    const char *segStart = cursor;
    while (cursor <= innerEnd) {
        char c = (cursor < innerEnd) ? *cursor : ',';
        if (cursor < innerEnd && (c == '(' || c == '[')) depth++;
        else if (cursor < innerEnd && (c == ')' || c == ']')) depth--;
        if ((cursor == innerEnd) || (c == ',' && depth == 0)) {
            if (cursor < innerEnd) sawComma = true;
            const char *s = segStart, *e = cursor;
            while (s < e && isspace((unsigned char)*s)) s++;
            while (e > s && isspace((unsigned char)e[-1])) e--;
            if (e <= s) { /* empty segment -> not a valid tuple type */
                for (size_t i = 0; i < count; i++) free(items[i]);
                free(items);
                return false;
            }
            if (count == cap) {
                size_t nc = cap ? cap * 2 : 4;
                char **ni = (char **)realloc(items, nc * sizeof(char *));
                if (!ni) { for (size_t i = 0; i < count; i++) free(items[i]); free(items); return false; }
                items = ni; cap = nc;
            }
            char *seg = (char *)malloc((size_t)(e - s) + 1);
            if (!seg) { for (size_t i = 0; i < count; i++) free(items[i]); free(items); return false; }
            memcpy(seg, s, (size_t)(e - s));
            seg[e - s] = '\0';
            items[count++] = seg;
            segStart = cursor + 1;
        }
        cursor++;
    }
    if (!sawComma || count < 2) {
        for (size_t i = 0; i < count; i++) free(items[i]);
        free(items);
        return false;
    }
    *outItems = items;
    *outCount = count;
    return true;
}

/* Advance the lexer past the remainder of the physical line that contains
 * `p->current` (used after capturing an `@`-annotation's raw expression text).
 * Resets the token FIFO and re-primes `current` on the next line. */
static void aetherResyncToNextLine(AetherParser *p) {
    const char *src = p->lexer.source;
    /* p->current.start points into the source; walk to the next newline. */
    const char *at = p->current.start;
    /* Guard: if start is NULL (synthetic), fall back to lexer pos. */
    if (!at) at = src + p->lexer.pos;
    const char *nl = at;
    while (*nl && *nl != '\n') nl++;
    size_t newPos = (size_t)((*nl == '\n') ? (nl + 1 - src) : (nl - src));
    p->lexer.pos = newPos;
    if (*nl == '\n') p->lexer.line++;
    p->queueHead = 0;
    p->queueCount = 0;
    aetherAdvance(p);
}

/* Return a pointer to the start of an unquoted `//` line comment within
 * [start, lineEnd), or lineEnd if there is none. String ("...") and char
 * ('...') literals -- honoring backslash escapes -- are skipped so a `//`
 * inside a literal (e.g. `result != "http://x"`) is not mistaken for a
 * comment. This mirrors the literal/comment handling in semantic.c's
 * sanitizeAetherSourceForSemanticScan, keeping the parser's raw contract-text
 * capture in step with the comment-sanitized text the semantic layer scans. */
static const char *aetherAnnotationExprEnd(const char *start, const char *lineEnd) {
    const char *c = start;
    while (c < lineEnd) {
        if (*c == '"' || *c == '\'') {
            char quote = *c++;
            while (c < lineEnd && *c != quote) {
                c += (*c == '\\' && c + 1 < lineEnd) ? 2 : 1;
            }
            if (c < lineEnd) c++; /* consume the closing quote */
            continue;
        }
        if (*c == '/' && c + 1 < lineEnd && c[1] == '/') {
            return c;
        }
        c++;
    }
    return lineEnd;
}

/* Collect a run of `@pre`/`@post`/`@pure`/`@cost` annotation lines that precede a
 * `fn`/method decl into `p->pending`. The shared Rea lexer yields `@` as
 * REA_TOKEN_UNKNOWN("@"); we read the directive identifier, then capture the rest
 * of the physical line as the raw contract expression (alias/method-scope/tuple
 * rewriting happens when the guard is built). `@pure`/`@cost` carry no codegen --
 * the semantic layer validates them on the source text -- so we just skip them.
 * Detached/empty/misplaced annotations are diagnosed by semantic.c; here we are
 * permissive so the parser produces an AST and the text-based checks fire. */
static void collectPendingAnnotations(AetherParser *p) {
    p->pendingAnnotCount = 0;
    while (p->current.type == REA_TOKEN_UNKNOWN &&
           p->current.length == 1 && p->current.start && p->current.start[0] == '@') {
        const char *lineStart = p->current.start;          /* at the '@' */
        const char *lineEnd = lineStart;
        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        /* Identify directive: skip '@', read the keyword. */
        const char *d = lineStart + 1;
        const char *dEnd = d;
        while (dEnd < lineEnd && (isalnum((unsigned char)*dEnd) || *dEnd == '_')) dEnd++;
        size_t dlen = (size_t)(dEnd - d);
        /* Record the first annotation of this run for a possible detached-annotation
         * diagnostic (ANN-001), emitted by the caller if no `fn` follows. */
        if (p->pendingAnnotCount == 0) {
            size_t n = dlen < sizeof(p->pendingAnnotName) - 1 ? dlen : sizeof(p->pendingAnnotName) - 1;
            memcpy(p->pendingAnnotName, d, n);
            p->pendingAnnotName[n] = '\0';
            p->pendingAnnotLine = p->current.line;
        }
        p->pendingAnnotCount++;
        /* Raw expression = trimmed remainder of the line after the directive,
         * stopping at a trailing `//` line comment so comment text is never
         * lowered into the contract guard. A `//` inside a string literal is
         * preserved. */
        const char *exprStart = dEnd;
        while (exprStart < lineEnd && isspace((unsigned char)*exprStart)) exprStart++;
        const char *exprEnd = aetherAnnotationExprEnd(exprStart, lineEnd);
        while (exprEnd > exprStart && isspace((unsigned char)exprEnd[-1])) exprEnd--;
        char *exprText = NULL;
        if (exprEnd > exprStart) {
            exprText = (char *)malloc((size_t)(exprEnd - exprStart) + 1);
            if (exprText) {
                memcpy(exprText, exprStart, (size_t)(exprEnd - exprStart));
                exprText[exprEnd - exprStart] = '\0';
            }
        }
        if (dlen == 3 && strncmp(d, "pre", 3) == 0) {
            p->pending.preExpr = appendContractExprText(p->pending.preExpr, exprText);
        } else if (dlen == 4 && strncmp(d, "post", 4) == 0) {
            p->pending.postExpr = appendContractExprText(p->pending.postExpr, exprText);
        } else if (dlen == 4 && strncmp(d, "pure", 4) == 0) {
            /* @pure has no codegen, but the fact is recorded on the upcoming fn
             * decl so the AST purity checks (ANN-001) know which functions are
             * pure. (@cost remains presence-only.) */
            p->pending.isPure = 1;
        }
        free(exprText);
        aetherResyncToNextLine(p);
    }
}

/* fn NAME ( [name: Type, ...] ) [ -> RetType ] { body }
 *
 * Mirrors rea parseFunctionDecl: a function with a non-void return type is
 * AST_FUNCTION_DECL with the return-type node on `right` and the body on
 * `extra`; a void function is AST_PROCEDURE_DECL with the body on `right` and
 * no return-type node. Params are AST_VAR_DECL nodes moved into the decl's
 * children[].
 *
 * When parsed inside a `type` body (p->currentClassName set), this is a METHOD:
 * the name is mangled to ClassName.method, an implicit `myself` pointer param is
 * injected first, the node is flagged virtual with its v-table slot, and a
 * bare-name alias is registered so `obj.method(...)` resolves -- all exactly as
 * rea's parseFunctionDecl does for class methods. */
static AST *parseFnDecl(AetherParser *p) {
    int fnLine = p->current.line; /* line of the `fn` keyword, for diagnostics */
    aetherAdvance(p); /* consume 'fn' */

    if (!aetherTokenIsIdentifierLike(&p->current) || aetherIsAetherTextKeyword(&p->current)) {
        /* A reserved word/operator where the name should be (e.g. `fn div()`,
         * `fn new()`, `fn loop()`): name the collision instead of the bare parse error. */
        if (!reportReservedMemberName(&p->current,
                                      p->currentClassName ? "method" : "function")) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected function name after 'fn'.", NULL);
        }
        p->hadError = true;
        return NULL;
    }
    /* `fn __init__` is Python's constructor. Aether has none, and a method of
     * that name compiled as an ordinary method that nothing ever calls: `new P()`
     * left the object zero-valued and exit 0. Rejected before the signature is
     * read, so the missing-`->` error never leads the repair into that form.
     * `init` and `constructor` stay legal method names. */
    if (p->current.length == 8 && strncmp(p->current.start, "__init__", 8) == 0) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "'__init__' is not a constructor: Aether has no constructor methods, "
                "so nothing would ever call it.",
                "allocate with `new T { field: value }` (or `new T()` then set fields), "
                "or write a top-level factory `fn` with a non-reserved name.");
        p->hadError = true;
        return NULL;
    }
    Token *nameTok = currentAsIdentifier(p);
    if (!nameTok) return NULL;
    aetherAdvance(p); /* consume function name */

    /* Method: mangle name to ClassName.method and reserve a v-table slot. */
    bool isMethod = (p->currentClassName != NULL);
    int methodIndex = -1;
    if (isMethod && nameTok->value) {
        methodIndex = p->currentMethodIndex++;
        size_t ln = strlen(p->currentClassName) + 1 + strlen(nameTok->value) + 1;
        char *m = (char *)malloc(ln);
        if (m) {
            snprintf(m, ln, "%s.%s", p->currentClassName, nameTok->value);
            free(nameTok->value);
            nameTok->value = m;
            nameTok->length = strlen(m);
        }
    }

    if (p->current.type != REA_TOKEN_LEFT_PAREN) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '(' after function name.", NULL);
        p->hadError = true;
        freeToken(nameTok);
        return NULL;
    }
    aetherAdvance(p); /* consume '(' */

    /* Function scope: parameters and body-local declarations recorded in the
     * binding table from here on belong to THIS function only. Top-level
     * const/let, imported-module bindings and function return types (which
     * live in funcReturns) stay visible program-wide; a local that shadows a
     * global is restored on exit. Every return below this point must go
     * through bindingScopeLeave. */
    bindingScopeEnter(p->bindings);

    AST *params = newASTNode(AST_COMPOUND, NULL);
    /* Inject the implicit `myself` receiver as the first method parameter, byte
     * for byte as rea parseFunctionDecl (~line 2679):
     *   VAR_DECL[ VARIABLE("myself",POINTER) ], right=POINTER_TYPE->TYPE_REFERENCE(Class,RECORD). */
    if (isMethod) {
        Token *ptypeTok = newToken(TOKEN_IDENTIFIER, p->currentClassName, p->current.line, 0);
        AST *refNode = newASTNode(AST_TYPE_REFERENCE, ptypeTok);
        setTypeAST(refNode, TYPE_RECORD);
        AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
        setTypeAST(ptrNode, TYPE_POINTER);
        setRight(ptrNode, refNode);
        Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", p->current.line, 0);
        AST *selfVar = newASTNode(AST_VARIABLE, selfTok);
        setTypeAST(selfVar, TYPE_POINTER);
        AST *selfDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(selfDecl, selfVar);
        setRight(selfDecl, ptrNode);
        setTypeAST(selfDecl, TYPE_POINTER);
        addChild(params, selfDecl);
    }
    /* First parameter's name + Aether type name, captured for extension-method
     * detection below (`fn f(self: T, ...)` at top level lowers to a UFCS method
     * on T). Only meaningful when this is NOT already a `type`-body method. */
    char *firstParamName = NULL;
    char *firstParamAetherType = NULL;
    bool sawAnyParam = false;
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        /* A type-body method may explicitly name its receiver `self` (bare or
         * `self: Type`). The implicit `myself` injected above already serves as the
         * receiver, so consume and skip it (body `self.<field>` maps to `myself`, as
         * for the implicit form). Extension methods at top level (isMethod=false)
         * keep `self` as a real first param and are unaffected. */
        if (isMethod && !sawAnyParam && p->current.type == REA_TOKEN_IDENTIFIER &&
            tokTextIs(&p->current, "self")) {
            aetherAdvance(p); /* consume 'self' */
            if (p->current.type == REA_TOKEN_COLON) {
                aetherAdvance(p); /* consume ':' */
                VarType st = TYPE_UNKNOWN; char *sat = NULL;
                AST *stype = parseTypeWithArraySuffix(p, &st, &sat);
                free(sat);
                if (stype) freeAST(stype); /* receiver type is the enclosing class */
            }
            if (p->current.type == REA_TOKEN_COMMA) aetherAdvance(p);
            continue; /* myself is the receiver; do not add `self` as a param */
        }
        if (!aetherTokenIsIdentifierLike(&p->current)) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected parameter name.", NULL);
            p->hadError = true;
            break;
        }
        Token *paramNameTok = currentAsIdentifier(p);
        if (!paramNameTok) { p->hadError = true; break; }
        aetherAdvance(p); /* consume param name */

        if (p->current.type != REA_TOKEN_COLON) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected ':' after parameter name.", NULL);
            p->hadError = true;
            freeToken(paramNameTok);
            break;
        }
        aetherAdvance(p); /* consume ':' */

        if (p->current.type == REA_TOKEN_RIGHT_PAREN || p->current.type == REA_TOKEN_COMMA ||
            p->current.type == REA_TOKEN_EOF) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected parameter type.", NULL);
            p->hadError = true;
            freeToken(paramNameTok);
            break;
        }
        VarType pvtype = TYPE_UNKNOWN;
        char *pAetherType = NULL;
        AST *ptypeNode = parseTypeWithArraySuffix(p, &pvtype, &pAetherType);
        if (!ptypeNode) { freeToken(paramNameTok); free(pAetherType); p->hadError = true; break; }
        /* Bind the param's name -> its Aether type name so body inference resolves it
         * the way let-decls do: `let e = arr[i]` -> element type, `let m = n` -> Int.
         * Harmless for scalars; required for `arr: Int[]` element inference. */
        if (paramNameTok->value && pAetherType) {
            bindingTableSet(p->bindings, paramNameTok->value, pAetherType);
        }
        if (!sawAnyParam) {
            sawAnyParam = true;
            firstParamName = paramNameTok->value ? strdup(paramNameTok->value) : NULL;
            firstParamAetherType = pAetherType; /* take ownership */
            pAetherType = NULL;
        }
        free(pAetherType);

        AST *paramVar = newASTNode(AST_VARIABLE, paramNameTok);
        setTypeAST(paramVar, pvtype);
        AST *paramDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(paramDecl, paramVar);
        setRight(paramDecl, ptypeNode);
        setTypeAST(paramDecl, pvtype);
        addChild(params, paramDecl);

        if (p->current.type == REA_TOKEN_COMMA) {
            aetherAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    } else if (!p->hadError) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected ')' to close parameter list.", NULL);
        p->hadError = true;
    }
    /* A parameter-list error is the whole story: stop here. Carrying on used to
     * reach the `->` check below at whatever token the list stalled on and add
     * a false "functions must declare an explicit return type" even when the
     * signature has `-> Int`, pointing the repair at the wrong fix. */
    if (p->hadError) {
        free(firstParamName);
        free(firstParamAetherType);
        freeAST(params);
        freeToken(nameTok);
        aetherFreePending(&p->pending);
        bindingScopeLeave(p->bindings);
        return NULL;
    }

    /* Extension method: a top-level `fn f(self: T, ...)` whose first parameter is
     * self-like (`self`/`my`/`myself`) and typed as a user type T. The rewriter
     * lowers this to a free function (un-mangled) whose body's `self` references
     * become `myself`, and rewrites call sites `f(recv, ...)` -> `recv.f(...)`
     * (UFCS). We mirror that: register T.f so call sites can detect it, rename the
     * first param's variable to `myself` so the body resolves, and set the class
     * context for the body so bare `self` lowers to `myself`. The function name is
     * NOT mangled and no v-table slot is reserved. */
    bool isExtensionMethod = false;
    char *extClassName = NULL;
    if (!isMethod && firstParamName && firstParamAetherType &&
        (strcmp(firstParamName, "self") == 0 || strcmp(firstParamName, "my") == 0 ||
         strcmp(firstParamName, "myself") == 0) &&
        /* The receiver type must be a user type (not a builtin scalar/array). */
        strchr(firstParamAetherType, '[') == NULL) {
        VarType probe = TYPE_UNKNOWN; const char *reaName = NULL;
        bool isBuiltin = mapAetherType(firstParamAetherType, strlen(firstParamAetherType),
                                       &reaName, &probe);
        if (!isBuiltin) {
            isExtensionMethod = true;
            extClassName = strdup(firstParamAetherType);
            /* Rename the first param's variable to `myself` (rea's semantic does
             * this; doing it here lets the body's `myself` references bind). */
            if (params->child_count > 0 && params->children[0] &&
                params->children[0]->child_count > 0) {
                AST *pv = params->children[0]->children[0];
                if (pv && pv->token && pv->token->value) {
                    free(pv->token->value);
                    pv->token->value = strdup("myself");
                    if (pv->token->value) pv->token->length = strlen(pv->token->value);
                }
            }
        }
    }
    free(firstParamName);
    free(firstParamAetherType);
    p->lastFnWasExtension = isExtensionMethod;

    /* An explicit '-> RetType' is REQUIRED (SYN-001), matching the rewriter,
     * which rejects any `fn` lacking a declared return type. */
    AST *returnTypeNode = NULL;
    VarType vtype = TYPE_VOID;
    char *retTypeName = NULL; /* Aether return-type name, for the return table */
    bool hasTupleReturn = false;
    char **tupleItemTypes = NULL;
    size_t tupleItemCount = 0;
    const AetherTupleSig *tupleSig = NULL;
    if (p->current.type != REA_TOKEN_ARROW) {
        reportAetherAstError(aetherSemanticGetSourcePath(), fnLine, "function",
                             "functions must declare an explicit return type.",
                             "write `fn name(args) -> Void { ... }` or replace `Void` with the actual return type.");
        p->hadError = true;
        freeAST(params);
        freeToken(nameTok);
        aetherFreePending(&p->pending);
        bindingScopeLeave(p->bindings);
        return NULL;
    }
    if (p->current.type == REA_TOKEN_ARROW) {
        aetherAdvance(p); /* consume '->' */
        if (p->current.type == REA_TOKEN_LEFT_BRACE || p->current.type == REA_TOKEN_EOF) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected return type after '->'.", NULL);
            p->hadError = true;
        } else if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            /* Tuple return type `-> (T, U, ...)`. Capture the raw `(...)` text
             * from the source and split it. Tuple returns are only supported on
             * top-level functions (matching the rewriter), not methods. */
            const char *tupleStart = p->current.start;
            const char *tupleEnd = tupleStart;
            int depth = 0;
            while (*tupleEnd) {
                if (*tupleEnd == '(') depth++;
                else if (*tupleEnd == ')') { depth--; if (depth == 0) { tupleEnd++; break; } }
                else if (*tupleEnd == '\n') break;
                tupleEnd++;
            }
            if (parseTupleTypeList(tupleStart, tupleEnd, &tupleItemTypes, &tupleItemCount)) {
                if (isMethod) {
                    reportAetherAstError(aetherSemanticGetSourcePath(), fnLine, "tuple",
                                         "tuple return types are currently only supported on top-level functions.",
                                         "return a record/object from methods, or move tuple-return logic to a top-level helper function.");
                    p->hadError = true;
                } else {
                    hasTupleReturn = true;
                    /* The tuple signature + synthesized record type were
                     * registered in the top-level forward-decl pre-pass; look
                     * the signature up by name and resolve the return type to
                     * its synthesized record (reentrant record-by-value
                     * lowering -- see buildSyntheticTupleRecordType), the same
                     * way any other record-returning function resolves. */
                    tupleSig = tupleTableGet(p->tuples, nameTok->value,
                                             nameTok->value ? strlen(nameTok->value) : 0);
                    if (tupleSig) {
                        char synthName[40];
                        aetherTupleSyntheticTypeName(synthName, sizeof(synthName), tupleSig->typeId);
                        returnTypeNode = buildTypeNode(synthName, strlen(synthName), fnLine, &vtype);
                        /* Unlike the scalar/record branch, parseTypeWithArraySuffix
                         * (which normally fills retTypeName) never runs here -- so
                         * without this, p->funcReturns never learns this function's
                         * return type, and `let t = pair();` type inference
                         * (inferLetTypeName) would silently see no recorded return
                         * type and fail to infer `t`'s type at all. */
                        retTypeName = strdup(synthName);
                    } else {
                        vtype = TYPE_VOID;
                        returnTypeNode = NULL;
                    }
                }
                /* Re-sync the lexer past the `(...)` we read straight from source. */
                p->lexer.pos = (size_t)(tupleEnd - p->lexer.source);
                p->queueHead = 0; p->queueCount = 0;
                aetherAdvance(p);
            } else {
                /* Not a tuple type after all: fall through to scalar parsing. */
                returnTypeNode = parseTypeWithArraySuffix(p, &vtype, &retTypeName);
            }
        } else {
            returnTypeNode = parseTypeWithArraySuffix(p, &vtype, &retTypeName);
        }
    }

    /* Record the (possibly-mangled) function/method name -> Aether return type
     * so inferred `let x = f(...)` / `x = recv.method(...)` can resolve it, the
     * way the rewriter's function table does. Recorded before the body so a
     * recursive call inside the body could resolve too. */
    if (retTypeName && nameTok->value && p->funcReturns) {
        bindingTableSet(p->funcReturns, nameTok->value, retTypeName);
        /* Extension method: also register T.fnName so call-site UFCS rewriting can
         * detect `f(recv,...)` as `recv.f(...)` (mirrors the rewriter's function
         * table, which keys extension methods under the receiver type). */
        if (isExtensionMethod && extClassName) {
            size_t qn = strlen(extClassName) + 1 + strlen(nameTok->value) + 1;
            char *q = (char *)malloc(qn);
            if (q) {
                snprintf(q, qn, "%s.%s", extClassName, nameTok->value);
                bindingTableSet(p->funcReturns, q, retTypeName);
                free(q);
            }
        }
    }
    /* Retain the return-type name (transfer ownership) for the duration of the
     * body, so a `@post` predicate's bare `result` can be resolved and its
     * comparability type-checked (see checkContractComparisons). A Void fn has no
     * meaningful `result`. Freed in the context-restore below. */
    char *fnReturnTypeName = (vtype != TYPE_VOID) ? retTypeName : NULL;
    if (vtype == TYPE_VOID) free(retTypeName);
    retTypeName = NULL;

    /* --- Contract + tuple body context (MILESTONE 3) --- */
    /* Take ownership of the pending @pre/@post collected before this decl. The
     * post-expr text is rewritten for tuple result slots (`result.N` ->
     * `__aether_tuple_<id>_item<N>`) here so parseRet / the guard builder see a
     * plain expression. Method field-prefix + builtin aliasing happen
     * automatically in parseExprFromText. */
    char *preExpr = p->pending.preExpr;
    char *postExpr = p->pending.postExpr;
    int fnIsPure = p->pending.isPure;
    p->pending.preExpr = NULL;
    p->pending.postExpr = NULL;
    p->pending.isPure = 0;

    /* Register this declaration (and its @pure state) for the AST purity
     * checks. Methods are registered under both the mangled `Type.method`
     * name (the decl token) and the bare method name (what a call site's
     * AST_PROCEDURE_CALL token carries). */
    if (nameTok->value) {
        int fnReturnsReal = (vtype == TYPE_REAL || vtype == TYPE_DOUBLE ||
                             vtype == TYPE_FLOAT || vtype == TYPE_LONG_DOUBLE);
        aetherAstRegisterFunctionPurity(nameTok->value, fnIsPure);
        aetherAstRegisterFunctionReturnsReal(nameTok->value, fnReturnsReal);
        if (isMethod) {
            const char *dot = strrchr(nameTok->value, '.');
            if (dot && dot[1]) {
                aetherAstRegisterFunctionPurity(dot + 1, fnIsPure);
                aetherAstRegisterFunctionReturnsReal(dot + 1, fnReturnsReal);
            }
        } else {
            /* Not a `type { ... }`-body method: this bare name shadows any
             * same-named vm_builtin for FX-001/ANN-001 purposes (see the
             * `swap` collision writeup in docs/ideas_and_todo.md). */
            aetherAstRegisterTopLevelFunction(nameTok->value);
        }
    }
    /* A tuple-return @post must reference positional slots (`result.0`/`result.1`),
     * not a bare `result`. Match the rewriter's ANN-001 diagnostic for an invalid
     * bare `result` reference. (forwardScan suppresses output; the real pass
     * reports it.) */
    if (hasTupleReturn && postExpr && !p->forwardScan) {
        const char *s = postExpr;
        bool bareResult = false;
        while (*s) {
            if (strncmp(s, "result", 6) == 0 &&
                (s == postExpr || !(isalnum((unsigned char)s[-1]) || s[-1] == '_')) &&
                !(isalnum((unsigned char)s[6]) || s[6] == '_')) {
                const char *after = s + 6;
                while (*after == ' ' || *after == '\t') after++;
                if (*after != '.') { bareResult = true; break; }
            }
            s++;
        }
        if (bareResult) {
            reportAetherAstError(aetherSemanticGetSourcePath(), fnLine, "contract",
                                 "tuple-return @post checks must reference slots explicitly, for example `result.0` or `result.1`.",
                                 "use positional tuple slots in @post, for example `result.0` and `result.1`.");
            p->hadError = true;
            freeAST(params);
            if (returnTypeNode) freeAST(returnTypeNode);
            freeToken(nameTok);
            free(preExpr);
            free(postExpr);
            free(fnReturnTypeName);
            bindingScopeLeave(p->bindings);
            return NULL;
        }
    }
    /* Unlike the old global-slot lowering, `result.N` -> field-access rewriting
     * cannot happen once here: each `ret (a,b);` site now builds its own
     * per-call-site temp record variable (buildTempRecordReturn), so the
     * rewrite target name differs per site. parseTupleReturn does this
     * per-site via aetherRewriteTupleResultRefs, using the still-untouched
     * `postExpr` text captured below in p->currentPostExpr. */

    const AetherTupleSig *prevTupleSig = p->currentTupleSig;
    const char *prevPostExpr = p->currentPostExpr;
    const char *prevFnName = p->currentFunctionName;
    const char *prevReturnTypeName = p->currentReturnTypeName;
    bool prevIsMethod = p->currentFunctionIsMethod;
    p->currentTupleSig = hasTupleReturn ? tupleSig : NULL;
    /* The guard message uses the unmangled name (e.g. "area"), matching the
     * rewriter, which prints `failed in <name>` from the source token. */
    const char *guardName = nameTok->value ? nameTok->value : "";
    if (isMethod && guardName) {
        const char *dot = strrchr(guardName, '.');
        if (dot && dot[1]) guardName = dot + 1;
    }
    p->currentFunctionName = guardName;
    p->currentFunctionIsMethod = isMethod;
    p->currentReturnTypeName = fnReturnTypeName;
    p->currentPostExpr = postExpr; /* parseRet consumes it for value/tuple returns */

    /* Body. */
    VarType prevType = p->currentFunctionType;
    int prevDepth = p->functionDepth;
    p->currentFunctionType = vtype;
    p->functionDepth++;
    /* For an extension method, set the class context so the body's bare `self`
     * lowers to `myself` (parsePrimary) and `self.method()` mangles to T.method,
     * exactly as the rewriter's rewriteMethodScopedExpr does. Restored below. */
    const char *prevExtClass = p->currentClassName;
    if (isExtensionMethod && extClassName) {
        p->currentClassName = extClassName;
    }

    AST *block = NULL;
    bool hasBody = false;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        if (p->forwardScan) {
            /* Forward-declaration pre-pass: emit a body-less prototype (the rewriter
             * emits `RetType name(params);`). Skip the body by brace-matching rather
             * than parsing it, so body-level diagnostics fire only in the real pass
             * (avoids double-reporting an error for an invalid body). */
            int depth = 0;
            while (p->current.type != REA_TOKEN_EOF) {
                if (p->current.type == REA_TOKEN_LEFT_BRACE) depth++;
                else if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
                    depth--;
                    if (depth == 0) { aetherAdvance(p); break; }
                }
                aetherAdvance(p);
            }
            hasBody = false;
        } else {
            block = parseBlock(p);
            hasBody = true;
        }
    }

    if (isExtensionMethod) {
        p->currentClassName = prevExtClass;
    }
    free(extClassName);
    extClassName = NULL;
    p->currentFunctionType = prevType;
    p->functionDepth = prevDepth;

    /* Non-Void function whose body has top-level statements but never returns a
     * value: a fallthrough path (FLOW-001), matching the rewriter. Checked before
     * @pre/@post guard injection so an injected guard does not count as the
     * fallthrough statement. Tuple-return fns are no longer exempt: each `ret
     * (a,b);` lowers to a real value-bearing `return` (see parseTupleReturn),
     * so a genuine fallthrough in a tuple fn is now a real, catchable bug
     * instead of a silently-void-shaped escape. */
    if (hasBody && block && !p->hadError && vtype != TYPE_VOID &&
        aetherExperiment()->tail == AETHER_TAIL_RET && !postExpr && !p->currentTupleSig) {
        aetherTailRetBlock(p, block);
    }
    if (hasBody && block && !p->hadError && vtype != TYPE_VOID &&
        astBlockHasFallthroughStmt(block) && !astHasValueReturn(block)) {
        reportAetherAstError(aetherSemanticGetSourcePath(), fnLine, "function",
                             "non-Void functions have a fallthrough path with no return value.",
                             "add `ret value;` on the top-level path that can reach the closing `}`, or declare the function `-> Void` if it only performs side effects.");
        p->hadError = true;
    }

    /* Value-copy array parameters at the call boundary: Aether's contract
     * (the ARR-001 model -- "arrays are value-copied at the call boundary")
     * vs. the VM's dynamic arrays, which are reference types by design. A
     * literal-built argument arrives as a static ArrayObj and copies on
     * write, but a concat-/setlength-built argument arrives aliased, so
     * `v[i] = x` inside the body would silently mutate the caller's array.
     * Prepend one un-alias step per array-typed parameter (see
     * buildArrayUnaliasStmt) so both construction styles behave identically.
     * Runs after the FLOW-001 check above (a synthesized statement must not
     * turn an empty non-Void body into a "fallthrough path") and before the
     * @pre injection below (the guard still lands at children[0]; copies
     * never change parameter values, so guard-vs-copy order is immaterial). */
    /* AETHER_EXPERIMENT=arrays=ref (D8 census, src/aether/experiment.h): array
     * parameters by reference and no prologue copy. The forward prototype gets
     * the same marks, since the compiler reads parameter modes from it. */
    bool arraysByRef = aetherExperiment()->arrays == AETHER_ARRAYS_REF;
    if (arraysByRef) {
        for (int pi = 0; pi < params->child_count; pi++) {
            AST *paramDecl = params->children[pi];
            if (paramDecl && paramDecl->var_type == TYPE_ARRAY) paramDecl->by_ref = 1;
        }
    }
    if (hasBody && block && !p->hadError && !arraysByRef) {
        for (int pi = params->child_count - 1; pi >= 0; pi--) {
            AST *paramDecl = params->children[pi];
            if (!paramDecl || paramDecl->var_type != TYPE_ARRAY ||
                paramDecl->child_count == 0 || !paramDecl->children[0] ||
                paramDecl->children[0]->type != AST_VARIABLE ||
                !paramDecl->children[0]->token || !paramDecl->children[0]->token->value)
                continue;
            AST *copyStmt = buildArrayUnaliasStmt(paramDecl->children[0], fnLine);
            addChild(block, NULL); /* grow capacity by one slot */
            for (int i = block->child_count - 1; i > 0; i--) {
                block->children[i] = block->children[i - 1];
            }
            block->children[0] = copyStmt;
            copyStmt->parent = block;
        }
    }

    /* Inject the @pre guard at the very start of the body, exactly where the
     * rewriter emits it (immediately after the opening brace). */
    if (block && preExpr && !p->hadError) {
        AST *guard = buildContractGuard(p, preExpr, "pre", guardName, fnLine);
        if (guard) {
            addChild(block, NULL); /* grow capacity by one slot */
            for (int i = block->child_count - 1; i > 0; i--) {
                block->children[i] = block->children[i - 1];
            }
            block->children[0] = guard;
            guard->parent = block;
        }
    }
    /* A VOID function with a @post gets its guard before the implicit
     * fall-through close (no `result` to stage), matching the rewriter. Tuple and
     * value-returning fns handle @post at each `ret` instead; tuple fns are no
     * longer TYPE_VOID (they resolve to their synthesized record type), so this
     * condition already excludes them without an explicit !hasTupleReturn. */
    if (block && postExpr && !p->hadError && vtype == TYPE_VOID) {
        AST *guard = buildContractGuard(p, postExpr, "post", guardName, fnLine);
        if (guard) addChild(block, guard);
    }

    /* Restore the enclosing function's contract/tuple context. */
    p->currentTupleSig = prevTupleSig;
    p->currentPostExpr = prevPostExpr;
    p->currentFunctionName = prevFnName;
    p->currentReturnTypeName = prevReturnTypeName;
    p->currentFunctionIsMethod = prevIsMethod;
    free(preExpr);
    free(postExpr);
    free(fnReturnTypeName);
    bindingScopeLeave(p->bindings);

    if (p->hadError) {
        freeAST(params);
        if (returnTypeNode) freeAST(returnTypeNode);
        if (block) freeAST(block);
        freeToken(nameTok);
        return NULL;
    }

    /* If a non-void function declared no '->' return type but the body returns
     * a value, that is a user error; for Milestone 1 we follow the explicit
     * '->'. A Void function uses AST_PROCEDURE_DECL. */
    AST *func = (vtype == TYPE_VOID) ? newASTNode(AST_PROCEDURE_DECL, nameTok)
                                     : newASTNode(AST_FUNCTION_DECL, nameTok);
    /* Methods participate in the v-table (rea sets is_virtual + i_val=slot). */
    if (methodIndex >= 0) {
        func->is_virtual = true;
        func->i_val = methodIndex;
    }

    /* Move params into the function node's children[] (rea does this move). */
    if (params->child_count > 0) {
        func->children = params->children;
        func->child_count = params->child_count;
        func->child_capacity = params->child_capacity;
        for (int i = 0; i < func->child_count; i++) {
            if (func->children[i]) func->children[i]->parent = func;
        }
        params->children = NULL;
        params->child_count = 0;
        params->child_capacity = 0;
    }
    freeAST(params);

    if (vtype == TYPE_VOID) {
        setRight(func, block);          /* procedure: body on right, no ret type */
        if (returnTypeNode) freeAST(returnTypeNode);
    } else {
        setRight(func, returnTypeNode); /* function: return type on right */
        setExtra(func, block);          /*           body on extra */
    }
    setTypeAST(func, vtype);

    /* Inside a `mod { }` body, register under the module-qualified key
     * ("ModuleName.funcname") -- mirroring rea's own parser (ReaParser.inModule
     * / .currentModuleName in parser.c) and the qualified key rea's semantic
     * analysis registers module exports under (ensureModuleProcedureSymbol) and
     * the compiler reads back from (current_compilation_unit_name). Without
     * this, a bare, never-updated stub got registered here for module member
     * functions too, and the compiler's call-site lookup tries the bare name
     * FIRST -- finding that stale (arity always 0) stub instead of ever
     * reaching the qualified, correctly-arity'd symbol, so one module function
     * calling a sibling failed with a bogus "expects 0 arguments" error. */
    const char *regName = nameTok->value ? nameTok->value : "";
    char qualifiedName[MAX_SYMBOL_LENGTH * 2 + 2];
    if (p->currentModuleName && *p->currentModuleName) {
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", p->currentModuleName, regName);
        regName = qualifiedName;
    }
    /* A `use`d dependency file is parsed via its own, independent parse call
     * (loadModuleRecursive), after the program's own entry file already
     * registered its own top-level `main`. Without this guard, a dependency
     * file's stray top-level `main` (declared outside any `mod { }` block,
     * e.g. for the file's own standalone testing) reuses-and-overwrites the
     * entry file's already-registered bare "main" symbol, silently making
     * the dependency's main the program's entry point instead. Mirrors the
     * identical guard in rea's own parser.c function-registration tail. */
    bool suppressRegistration = !isMethod && !(p->currentModuleName && *p->currentModuleName) &&
                                 reaFrontendIsParsingLibraryFile() &&
                                 strcasecmp(regName, "main") == 0;
    if (!suppressRegistration) {
        registerFunctionSymbol(func, regName, vtype, hasBody, isMethod);
    }
    return func;
}

/* ------------------------------------------------------------------ */
/* const declarations (top-level or block)                             */
/* ------------------------------------------------------------------ */

/* const [Type] NAME = expr;  ->  AST_CONST_DECL (token=name, left=value,
 * right=type node or NULL), mirroring rea parseConstDecl. At top level
 * (functionDepth==0) the value is folded and registered with addCompilerConstant
 * so later references resolve -- the contract for `const X = ...;
 * let y = X;`. Aether writes the type *after* the name (`const NAME: T = e`),
 * unlike Rea's `const T NAME = e`, so we accept the Aether form and build the
 * same node. The binding is recorded for inferred-let type resolution. */
static AST *parseConstDecl(AetherParser *p) {
    aetherAdvance(p); /* consume 'const' */

    if (!aetherTokenIsIdentifierLike(&p->current)) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected name after 'const'.", NULL);
        p->hadError = true;
        return NULL;
    }
    Token *nameTok = currentAsIdentifier(p);
    if (!nameTok) return NULL;
    aetherAdvance(p); /* consume name */

    AST *typeNode = NULL;
    VarType vtype = TYPE_UNKNOWN;
    char *declaredTypeName = NULL;
    if (p->current.type == REA_TOKEN_COLON) {
        aetherAdvance(p); /* consume ':' */
        if (p->current.type == REA_TOKEN_EOF || p->current.type == REA_TOKEN_EQUAL ||
            p->current.type == REA_TOKEN_SEMICOLON) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected type after ':'.", NULL);
            p->hadError = true;
            freeToken(nameTok);
            return NULL;
        }
        declaredTypeName = (char *)malloc(p->current.length + 1);
        if (declaredTypeName) {
            memcpy(declaredTypeName, p->current.start, p->current.length);
            declaredTypeName[p->current.length] = '\0';
        }
        typeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &vtype);
        aetherAdvance(p); /* consume type name */
    }

    if (p->current.type != REA_TOKEN_EQUAL) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "const declaration requires '= value'.", NULL);
        p->hadError = true;
        freeToken(nameTok);
        if (typeNode) freeAST(typeNode);
        free(declaredTypeName);
        return NULL;
    }
    ReaToken eq = p->current;
    aetherAdvance(p); /* consume '=' */
    aetherNoteOperator(p, &eq);
    AST *value = parseExpr(p);
    if (!value) {
        aetherReportMissingExpr(p, NULL);
        freeToken(nameTok);
        if (typeNode) freeAST(typeNode);
        free(declaredTypeName);
        return NULL;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }

    AST *node = newASTNode(AST_CONST_DECL, nameTok);
    setLeft(node, value);
    if (typeNode) setRight(node, typeNode);
    setTypeAST(node, value->var_type);

    /* Record the binding for inferred-let resolution. Prefer the explicit type
     * name; otherwise derive it from the value's computed var_type. */
    if (!declaredTypeName) {
        const char *vn = aetherTypeNameForVarType(value->var_type);
        if (vn) declaredTypeName = strdup(vn);
    }
    if (declaredTypeName)
        bindingTableSet(p->bindings, nameTok->value, declaredTypeName);

    /* Fold + register the compile-time value (rea parseConstDecl tail). Suppressed
     * during the forward-declaration pre-pass: the real pass registers it, and
     * addCompilerConstant warns on the redefinition a double registration causes. */
    {
        Value v = evaluateCompileTimeValue(value);
        if (VALUE_TYPE(v) != TYPE_VOID && VALUE_TYPE(v) != TYPE_UNKNOWN) {
            if (p->functionDepth == 0 && !p->forwardScan) {
                addCompilerConstant(nameTok->value, &v, nameTok->line);
            }
            if (!typeNode) setTypeAST(node, VALUE_TYPE(v));
        }
        freeValue(&v);
    }
    free(declaredTypeName);
    return node;
}

/* Top-level/statement dispatcher kept under a distinct name for the forward
 * declaration; const parsing is identical at either depth (parseConstDecl reads
 * functionDepth to decide on compile-time folding). */
static AST *parseConstDeclTop(AetherParser *p) {
    return parseConstDecl(p);
}

/* ------------------------------------------------------------------ */
/* Constant record-field default values (`field: Type = <const>`)      */
/* ------------------------------------------------------------------ */

/* A declared field default must be a compile-time constant: the construction
 * path (pscal-core's emitDefaultFieldInitializers) folds it with
 * evaluateCompileTimeValue and bakes the result into every `new T()` and every
 * unset field of `new T { ... }`. Allow literals, the (again constant) array
 * literal, and constant unary/binary expressions over them; reject anything
 * that reads another field, `self`, or calls a function -- those have
 * evaluation-order/purity concerns, are FIELD-003, and belong at construction
 * time (`new T { field: value }`). */
static bool aetherFieldDefaultIsConstant(const AST *e) {
    if (!e) return false;
    switch (e->type) {
        case AST_NUMBER:
        case AST_STRING:
        case AST_BOOLEAN:
        case AST_NIL:
            return true;
        case AST_UNARY_OP:
            return aetherFieldDefaultIsConstant(e->left);
        case AST_BINARY_OP:
            return aetherFieldDefaultIsConstant(e->left) &&
                   aetherFieldDefaultIsConstant(e->right);
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < e->child_count; i++) {
                if (!aetherFieldDefaultIsConstant(e->children[i])) return false;
            }
            return true;
        default:
            return false;
    }
}

/* Type-check a constant field default against the field's declared type. The
 * rules mirror the scalar families Aether exposes (Int/Real/Text/Bool) plus
 * arrays and reference types; an Int default widens into a Real field. A field
 * type we do not model precisely stays permissive -- the constant guard above
 * still applies -- so inference gaps never raise a false type error. */
static bool aetherFieldDefaultTypeMatches(VarType fieldType, const AST *e) {
    if (!e) return false;
    VarType et = e->var_type;
    if (isIntegerFamilyType(fieldType)) {
        return isIntegerFamilyType(et);
    }
    if (isRealType(fieldType)) {
        return isRealType(et) || isIntegerFamilyType(et);
    }
    if (isPascalStringType(fieldType) || isPascalCharType(fieldType)) {
        return isPascalStringType(et) || isPascalCharType(et);
    }
    if (fieldType == TYPE_BOOLEAN) {
        return et == TYPE_BOOLEAN;
    }
    if (fieldType == TYPE_ARRAY) {
        return et == TYPE_ARRAY || et == TYPE_NIL;
    }
    /* A user class/record field is a pointer; only `nil` is a valid constant. */
    if (fieldType == TYPE_POINTER || fieldType == TYPE_RECORD || fieldType == TYPE_ENUM) {
        return et == TYPE_NIL;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* type (record/class) declarations                                    */
/* ------------------------------------------------------------------ */

/* type NAME { field: T; ... fn method(...) {...} }
 *
 * Lowers to the same AST a Rea `class NAME { ... }` produces (translate.c maps
 * `type ` -> `class `): an AST_RECORD_TYPE whose first member is a hidden
 * `__vtable` pointer field, followed by the data fields; methods are gathered
 * separately (with the implicit `myself` receiver). The whole thing is wrapped
 * in an AST_TYPE_DECL and returned in an is_global_scope AST_COMPOUND bundle
 * [ type-decl, method, method, ... ] for top-level flattening -- byte for byte
 * as rea parseStatement's REA_TOKEN_CLASS branch. */
static AST *parseTypeDecl(AetherParser *p) {
    aetherAdvance(p); /* consume 'type' */

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected a type name after 'type'.", NULL);
        p->hadError = true;
        return NULL;
    }
    Token *classNameTok = copyNameToken(p);
    if (!classNameTok) return NULL;
    aetherAdvance(p); /* consume type name */

    /* Build the record with the hidden vtable pointer field first. */
    AST *recordAst = newASTNode(AST_RECORD_TYPE, NULL);
    Token *vtTok = newToken(TOKEN_IDENTIFIER, "__vtable", classNameTok->line, 0);
    AST *vtVar = newASTNode(AST_VARIABLE, vtTok);
    setTypeAST(vtVar, TYPE_POINTER);
    AST *vtType = newASTNode(AST_POINTER_TYPE, NULL);
    setTypeAST(vtType, TYPE_POINTER);
    AST *vtDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(vtDecl, vtVar);
    setRight(vtDecl, vtType);
    setTypeAST(vtDecl, TYPE_POINTER);
    addChild(recordAst, vtDecl);

    AST *methods = newASTNode(AST_COMPOUND, NULL);

    const char *prevClass = p->currentClassName;
    int prevIndex = p->currentMethodIndex;
    const AetherFieldNameList *prevFields = p->classFields;
    p->currentClassName = classNameTok->value;
    p->currentMethodIndex = 0;
    /* Track this type's field names so method contract expressions can lower bare
     * field references to `myself.<field>` (rewriteMethodScopedExpr parity). */
    AetherFieldNameList fields;
    fieldNameListInit(&fields);
    p->classFields = &fields;

    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        aetherAdvance(p); /* consume '{' */
        while (p->current.type != REA_TOKEN_RIGHT_BRACE &&
               p->current.type != REA_TOKEN_EOF && !p->hadError) {
            /* Method contract annotations precede a `fn`. */
            collectPendingAnnotations(p);
            if (p->hadError) break;
            if (isAetherKeyword(&p->current, "fn")) {
                AST *m = parseFnDecl(p); /* class-aware: injects myself, mangles */
                if (m) {
                    addChild(methods, m);
                } else {
                    p->hadError = true;
                    break;
                }
            } else if (p->current.type == REA_TOKEN_IDENTIFIER &&
                       !aetherIsAetherTextKeyword(&p->current)) {
                /* A data field: NAME : Type ; */
                Token *fieldTok = currentAsIdentifier(p);
                if (!fieldTok) { p->hadError = true; break; }
                if (fieldTok->value) {
                    fieldNameListAdd(&fields, fieldTok->value, strlen(fieldTok->value));
                }
                aetherAdvance(p); /* consume field name */
                if (p->current.type != REA_TOKEN_COLON) {
                    reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                            "expected ':' after field name in type.", NULL);
                    p->hadError = true;
                    freeToken(fieldTok);
                    break;
                }
                aetherAdvance(p); /* consume ':' */
                VarType fvtype = TYPE_UNKNOWN;
                AST *ftypeNode = parseTypeWithArraySuffix(p, &fvtype, NULL);
                if (!ftypeNode) { freeToken(fieldTok); p->hadError = true; break; }
                AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
                setTypeAST(fieldVar, fvtype);
                AST *fieldDecl = newASTNode(AST_VAR_DECL, NULL);
                addChild(fieldDecl, fieldVar);
                setRight(fieldDecl, ftypeNode);
                setTypeAST(fieldDecl, fvtype);
                addChild(recordAst, fieldDecl);
                /* Optional constant default: `field: Type = <const-expr>`. The
                 * default is attached to the field's VAR_DECL `left` slot, which
                 * pscal-core's emitDefaultFieldInitializers folds and applies at
                 * construction (`new T()` and unset fields of `new T { ... }`).
                 * Only compile-time constants are allowed (FIELD-003), and the
                 * value must type-match the declared field. `=` is REA_TOKEN_EQUAL
                 * (assignment); `==` is REA_TOKEN_EQUAL_EQUAL and is not matched. */
                if (p->current.type == REA_TOKEN_EQUAL) {
                    int eqLine = p->current.line;
                    aetherAdvance(p); /* consume '=' */
                    /* A missing default keeps the field-default message below. */
                    bool noValue = (p->current.type == REA_TOKEN_SEMICOLON ||
                                    p->current.type == REA_TOKEN_COMMA ||
                                    p->current.type == REA_TOKEN_RIGHT_BRACE);
                    AST *defExpr = noValue ? NULL : parseExpr(p);
                    if (!defExpr || p->hadError) {
                        if (!p->hadError) {
                            reportAetherAstError(aetherSemanticGetSourcePath(), eqLine, "field-default",
                                    "expected a constant default value after '=' in type field.", NULL);
                        }
                        if (defExpr) freeAST(defExpr);
                        p->hadError = true;
                        break;
                    }
                    if (!aetherFieldDefaultIsConstant(defExpr)) {
                        reportAetherAstError(aetherSemanticGetSourcePath(), eqLine, "field-default",
                                "a field default must be a literal; this one is not.",
                                /* The old wording said \"set computed values at construction\" and
                                 * offered \"a constant expression\" as the remedy. Both mislead: the
                                 * check rejects a named const (`= MAX_SCORE`) and any arithmetic over
                                 * one (`= MAX_SCORE + 1`), neither of which is computed, and the
                                 * second is exactly the thing being suggested. Say literal, and name
                                 * the const case outright since it is the form a reader assumes
                                 * works. */
                                "use a literal (`= 0`, `= \"\"`, `= true`, `= []`). A named const is "
                                "NOT accepted -- `= MAX` fails just as `= MAX + 1` does. For anything "
                                "else, drop the default and set it at construction: "
                                "`new T { field: MAX + 1 }`.");
                        freeAST(defExpr);
                        p->hadError = true;
                        break;
                    }
                    /* Arrays: only the empty literal `= []` is a supported default
                     * (it matches the zero-initialized field). A populated array
                     * literal cannot be applied by the construction path, so route
                     * it to FIELD-003 instead of silently dropping the values. */
                    if (fvtype == TYPE_ARRAY && defExpr->type == AST_ARRAY_LITERAL &&
                        defExpr->child_count > 0) {
                        reportAetherAstError(aetherSemanticGetSourcePath(), eqLine, "field-default",
                                "only an empty array default (`= []`) is supported; populate array "
                                "fields at construction or in a method.",
                                "declare `xs: T[] = []` (or omit the default) and fill the array via "
                                "`new T { ... }` or method code.");
                        freeAST(defExpr);
                        p->hadError = true;
                        break;
                    }
                    if (!aetherFieldDefaultTypeMatches(fvtype, defExpr)) {
                        const char *ftName = aetherTypeNameForVarType(fvtype);
                        const char *vtName = aetherTypeNameForVarType(defExpr->var_type);
                        char detail[192];
                        snprintf(detail, sizeof(detail),
                                "field default value type mismatch: cannot use a %s default for a "
                                "field of type %s.",
                                vtName ? vtName : "non-matching", ftName ? ftName : "this");
                        reportAetherAstError(aetherSemanticGetSourcePath(), eqLine, "type", detail,
                                "make the default match the field type (e.g. `count: Int = 0`, "
                                "`name: Text = \"\"`), or set the value at construction with "
                                "`new T { field: value }`.");
                        freeAST(defExpr);
                        p->hadError = true;
                        break;
                    }
                    setLeft(fieldDecl, defExpr);
                }
                if (p->current.type == REA_TOKEN_SEMICOLON) {
                    aetherAdvance(p);
                } else if (p->current.type == REA_TOKEN_COMMA) {
                    /* `field: Type,` -- match the rewriter's type-field diagnostic
                     * (translate.c) rather than the generic "unexpected token". */
                    reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "type",
                                         "type fields must end with ';', not ','.",
                                         "write `fieldName: Type;` for each field inside a `type` block.");
                    p->hadError = true;
                    break;
                }
            } else if (p->current.type == REA_TOKEN_SEMICOLON) {
                aetherAdvance(p); /* tolerate stray semicolons */
            } else {
                /* A reserved word/type name where a field name should be (e.g.
                 * `word: Text;`): name the collision instead of the bare error. */
                if (!reportReservedMemberName(&p->current, "field")) {
                    reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                            "unexpected token in type body.", NULL);
                }
                p->hadError = true;
                break;
            }
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
            aetherAdvance(p);
        } else if (!p->hadError) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected '}' to close type body.", NULL);
            p->hadError = true;
        }
    }

    p->currentClassName = prevClass;
    p->currentMethodIndex = prevIndex;
    p->classFields = prevFields;
    fieldNameListFree(&fields);

    if (p->hadError) {
        freeAST(recordAst);
        freeAST(methods);
        freeToken(classNameTok);
        return NULL;
    }

    /* AST_TYPE_DECL(Name = record) + register the type globally. */
    AST *typeDecl = newASTNode(AST_TYPE_DECL, classNameTok);
    setLeft(typeDecl, recordAst);
    if (classNameTok->value) {
        insertType(classNameTok->value, recordAst);
    }

    /* Bundle: [ type-decl, methods... ], flagged for top-level flattening. */
    AST *bundle = newASTNode(AST_COMPOUND, NULL);
    bundle->is_global_scope = true;
    addChild(bundle, typeDecl);
    for (int i = 0; i < methods->child_count; i++) {
        addChild(bundle, methods->children[i]);
        methods->children[i] = NULL;
    }
    methods->child_count = 0;
    freeAST(methods);
    return bundle;
}

/* ------------------------------------------------------------------ */
/* Modules (use / mod / export)                                        */
/* ------------------------------------------------------------------ */

/* True if the current token is Aether's `mod` keyword. The shared Rea lexer maps
 * the 3-char lexeme "mod" to REA_TOKEN_PERCENT (Rea's modulo operator keyword),
 * so we must recognize it by token type + text rather than as an identifier. */
static bool aetherIsModKeyword(const AetherParser *p) {
    return p->current.type == REA_TOKEN_PERCENT && tokTextIs(&p->current, "mod");
}

/* `use NAME ;` / `use "NAME" ;` -> AST_USES_CLAUSE { AST_IMPORT(token=STRING path) }.
 * The rewriter lowers `use X;` to Rea `#import X;`, which rea's parseImport turns
 * into exactly this shape (the path token is always a STRING const, whether the
 * source spelled a bare identifier or a quoted string). Mirrors rea parseImport
 * for the single-import, no-alias case Aether emits. */
static AST *parseUse(AetherParser *p) {
    aetherAdvance(p); /* consume 'use' */
    char *path = NULL;
    int pathLine = p->current.line;
    if (p->current.type == REA_TOKEN_STRING) {
        /* Strip the surrounding quotes, like rea parseImport. */
        size_t len = (size_t)p->current.length;
        if (len >= 2) {
            path = (char *)malloc(len - 1);
            if (path) { memcpy(path, p->current.start + 1, len - 2); path[len - 2] = '\0'; }
        }
        aetherAdvance(p);
    } else if (p->current.type == REA_TOKEN_IDENTIFIER) {
        size_t len = (size_t)p->current.length;
        path = (char *)malloc(len + 1);
        if (path) { memcpy(path, p->current.start, len); path[len] = '\0'; }
        aetherAdvance(p);
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "import",
                "expected a module name after 'use'.", NULL);
        p->hadError = true;
        return NULL;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (!path) { p->hadError = true; return NULL; }

    Token *pathTok = newToken(TOKEN_STRING_CONST, path, pathLine, 0);
    free(path);
    AST *importNode = newASTNode(AST_IMPORT, pathTok);
    AST *uses = newASTNode(AST_USES_CLAUSE, NULL);
    addChild(uses, importNode);
    return uses;
}

/* Recursively flag exportable declarations as exported, mirroring rea
 * markExported (used for `export <decl>` inside a module body). */
static void aetherMarkExported(AST *node) {
    if (!node) return;
    if (node->type == AST_COMPOUND || node->type == AST_BLOCK) {
        if (node->left) aetherMarkExported(node->left);
        if (node->right) aetherMarkExported(node->right);
        if (node->extra) aetherMarkExported(node->extra);
        for (int i = 0; i < node->child_count; i++) aetherMarkExported(node->children[i]);
        return;
    }
    switch (node->type) {
        case AST_VAR_DECL: case AST_CONST_DECL: case AST_TYPE_DECL:
        case AST_FUNCTION_DECL: case AST_PROCEDURE_DECL:
            node->is_exported = true;
            break;
        default: break;
    }
}

/* Route a module-body member into the module's decls/stmts, mirroring rea
 * appendModuleNode: exportable decls + nested imports go to decls. */
static void aetherAppendModuleNode(AST *decls, AST *stmts, AST *node) {
    if (!node) return;
    if (node->type == AST_COMPOUND && node->is_global_scope) {
        for (int i = 0; i < node->child_count; i++) {
            AST *child = node->children[i];
            if (!child) continue;
            switch (child->type) {
                case AST_VAR_DECL: case AST_CONST_DECL: case AST_TYPE_DECL:
                case AST_FUNCTION_DECL: case AST_PROCEDURE_DECL:
                case AST_USES_CLAUSE: case AST_IMPORT:
                    addChild(decls, child); break;
                default: addChild(stmts, child); break;
            }
            node->children[i] = NULL;
        }
        freeAST(node);
        return;
    }
    switch (node->type) {
        case AST_VAR_DECL: case AST_CONST_DECL: case AST_TYPE_DECL:
        case AST_FUNCTION_DECL: case AST_PROCEDURE_DECL:
        case AST_USES_CLAUSE: case AST_IMPORT: case AST_MODULE:
            addChild(decls, node); break;
        default: addChild(stmts, node); break;
    }
}

/* Parse a single module-body member, handling the leading `export` qualifier.
 * `export <decl>` parses the declaration then flags it exported. Members are the
 * same declarations top-level allows (fn / type / const). */
static AST *parseModuleMember(AetherParser *p) {
    bool exported = false;
    if (p->current.type == REA_TOKEN_EXPORT || isAetherKeyword(&p->current, "export")) {
        exported = true;
        aetherAdvance(p); /* consume 'export' */
    }
    AST *decl = NULL;
    if (isAetherKeyword(&p->current, "fn")) {
        decl = parseFnDecl(p);
    } else if (p->current.type == REA_TOKEN_TYPE || isAetherKeyword(&p->current, "type")) {
        decl = parseTypeDecl(p);
    } else if (p->current.type == REA_TOKEN_CONST || isAetherKeyword(&p->current, "const")) {
        decl = parseConstDeclTop(p);
    } else if (aetherIsModKeyword(p)) {
        decl = NULL; /* nested modules are not part of Aether's surface syntax */
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected a declaration inside 'mod'.", NULL);
        p->hadError = true;
        return NULL;
    }
    if (decl && exported) aetherMarkExported(decl);
    return decl;
}

/* `mod NAME { members }` -> AST_MODULE(token=name, right=AST_BLOCK[decls,stmts]),
 * mirroring rea parseModule. The rewriter lowers `mod` to Rea `module`; the AST
 * is identical. Imported Aether module files are parsed through this same entry
 * (reaFrontendParseSource -> parseAether -> parseAetherAst). */
static AST *parseModuleDecl(AetherParser *p) {
    int modLine = p->current.line;
    aetherAdvance(p); /* consume 'mod' */
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected a module name after 'mod'.", NULL);
        p->hadError = true;
        return NULL;
    }
    Token *nameTok = currentAsIdentifier(p);
    if (!nameTok) return NULL;
    aetherAdvance(p); /* consume module name */
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '{' to begin module body.", NULL);
        p->hadError = true;
        freeToken(nameTok);
        return NULL;
    }
    aetherAdvance(p); /* consume '{' */

    AST *moduleNode = newASTNode(AST_MODULE, nameTok);
    AST *block = newASTNode(AST_BLOCK, NULL);
    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    decls->is_global_scope = true;
    stmts->is_global_scope = true;
    addChild(block, decls);
    addChild(block, stmts);
    setRight(moduleNode, block);

    const char *prevModuleName = p->currentModuleName;
    p->currentModuleName = nameTok->value;
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF &&
           !p->hadError) {
        collectPendingAnnotations(p);
        if (p->hadError) break;
        if (p->current.type == REA_TOKEN_SEMICOLON) { aetherAdvance(p); continue; }
        AST *member = parseModuleMember(p);
        if (!member) { if (!p->hadError) break; p->hadError = true; break; }
        aetherAppendModuleNode(decls, stmts, member);
        while (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    }
    p->currentModuleName = prevModuleName;
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        aetherAdvance(p);
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '}' to close module body.", NULL);
        p->hadError = true;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);

    (void)modLine;
    return moduleNode;
}

/* ------------------------------------------------------------------ */
/* Program entry                                                       */
/* ------------------------------------------------------------------ */

/* Append a parsed top-level declaration to `decls`/`stmts`, flattening the
 * `AST_COMPOUND` bundle that a `type` (record/class + methods) produces -- the
 * exact rea parseRea top-level handling.
 *
 * A `let` stays in `stmts`, unlike every other top-level form. It can sit
 * between two statements, and what it binds depends on everything that ran
 * before it, so hoisting it into `decls` would compile `let a = 1; a = 5;
 * let b = a;` with `b` reading `a` as it stood before the assignment. Its slot
 * is still defined ahead of the program (the compiler hoists that much -- see
 * hoistStatementGlobalSlots in pscal-core), so the binding is visible
 * everywhere regardless of where its initializer runs.
 *
 * A splice compound (i_val==1: the may-alias `let x: T[] = f();` un-alias
 * step, the concat and slice expansions, object-literal hoists) is flattened
 * the same way parseBlock flattens it. Kept whole, it compiles as a nested
 * block, which keeps its own locals, so the `let` it carries would be out of
 * scope everywhere else (SCOPE-001 at every use). */
static void appendTopLevelDecl(AST *decls, AST *stmts, AST *node) {
    if (!node) return;
    if (node->type == AST_COMPOUND && (node->is_global_scope || node->i_val == 1)) {
        for (int i = 0; i < node->child_count; i++) {
            AST *child = node->children[i];
            if (!child) continue;
            if (child->type == AST_FUNCTION_DECL ||
                child->type == AST_PROCEDURE_DECL || child->type == AST_TYPE_DECL ||
                child->type == AST_CONST_DECL) {
                addChild(decls, child);
            } else {
                addChild(stmts, child);
            }
            node->children[i] = NULL;
        }
        freeAST(node);
        return;
    }
    if (node->type == AST_FUNCTION_DECL ||
        node->type == AST_PROCEDURE_DECL || node->type == AST_TYPE_DECL ||
        node->type == AST_CONST_DECL || node->type == AST_MODULE ||
        node->type == AST_USES_CLAUSE || node->type == AST_IMPORT) {
        addChild(decls, node);
    } else {
        addChild(stmts, node);
    }
}

/* Build the synthesized record type for a tuple signature: a hidden __vtable
 * pointer field (copying parseTypeDecl's construction verbatim -- proven to
 * work for a zero-method record by the existing return_object_init_pass.aether
 * path) followed by one field per tuple item, named item0..itemN-1. Registers
 * it globally via insertType() and returns the AST_TYPE_DECL node to splice
 * into the top-level declarations, exactly like parseTypeDecl does for a
 * user-authored `type` statement. */
static AST *buildSyntheticTupleRecordType(int typeId, char **itemTypes, size_t itemCount, int line) {
    char typeName[40];
    aetherTupleSyntheticTypeName(typeName, sizeof(typeName), typeId);

    AST *recordAst = newASTNode(AST_RECORD_TYPE, NULL);

    /* Hidden vtable pointer field first, matching parseTypeDecl. */
    Token *vtTok = newToken(TOKEN_IDENTIFIER, "__vtable", line, 0);
    AST *vtVar = newASTNode(AST_VARIABLE, vtTok);
    setTypeAST(vtVar, TYPE_POINTER);
    AST *vtType = newASTNode(AST_POINTER_TYPE, NULL);
    setTypeAST(vtType, TYPE_POINTER);
    AST *vtDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(vtDecl, vtVar);
    setRight(vtDecl, vtType);
    setTypeAST(vtDecl, TYPE_POINTER);
    addChild(recordAst, vtDecl);

    for (size_t i = 0; i < itemCount; i++) {
        char fieldName[32];
        snprintf(fieldName, sizeof(fieldName), "item%zu", i);
        VarType vt = TYPE_UNKNOWN;
        /* FromName, not buildTypeNode: an item type may carry the `[]` suffix
         * (see the tuple signature table), which buildTypeNode cannot resolve.
         * The open AST_ARRAY_TYPE this yields is the same shape parseTypeDecl
         * gives a user-written `field: Int[]`, so the field initializes. */
        AST *typeNode = buildTypeNodeFromName(itemTypes[i], strlen(itemTypes[i]), line, &vt);
        if (!typeNode) { freeAST(recordAst); return NULL; }
        Token *fTok = newToken(TOKEN_IDENTIFIER, fieldName, line, 0);
        AST *fVar = newASTNode(AST_VARIABLE, fTok);
        setTypeAST(fVar, vt);
        AST *fDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(fDecl, fVar);
        setRight(fDecl, typeNode);
        setTypeAST(fDecl, vt);
        addChild(recordAst, fDecl);
    }

    Token *typeNameTok = newToken(TOKEN_IDENTIFIER, typeName, line, 0);
    AST *typeDecl = newASTNode(AST_TYPE_DECL, typeNameTok);
    setLeft(typeDecl, recordAst);
    insertType(typeName, recordAst);
    return typeDecl;
}

/* Pre-pass over the (TOON-preprocessed) source that registers tuple-return
 * function signatures and emits a synthesized record type per signature into
 * `decls` (reentrant record-by-value lowering -- see buildSyntheticTupleRecordType).
 * Scans only top-level (column-0) `fn NAME(...) -> (...)` lines; method tuple
 * returns are unsupported (handled/diagnosed in parseFnDecl). Returns false on
 * allocation failure. */
static bool aetherRegisterTupleGlobals(const char *source, AetherTupleTable *tuples,
                                       int *nextTupleTypeId, AST *decls) {
    const char *cursor = source;
    int lineNumber = 1;
    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        /* Only column-0 `fn ` lines (no leading whitespace). */
        if (lineStart < lineEnd && (lineStart[0] == 'f' && lineStart[1] == 'n') &&
            (lineStart + 2 < lineEnd) &&
            (lineStart[2] == ' ' || lineStart[2] == '\t')) {
            const char *c = lineStart + 2;
            while (c < lineEnd && (*c == ' ' || *c == '\t')) c++;
            const char *nameStart = c;
            while (c < lineEnd && (isalnum((unsigned char)*c) || *c == '_')) c++;
            const char *nameEnd = c;
            /* The parameter list (and the `-> RetType` that follows it) may be
             * wrapped across multiple lines for readability; a long signature
             * like
             *     fn t1(x: Real,
             *         y: Real) -> (Real, Real) {
             * has no `->` on the `fn` line itself. Scan from the opening '('
             * over the FULL source (not bounded to this line's lineEnd),
             * depth-counting parens to find where the parameter list actually
             * closes, then look for the arrow after that -- rather than only
             * ever considering text up to the first '\n'. Bail out on '{' or
             * ';' before an arrow is found so a malformed/bodyless line can't
             * run this scan away into unrelated code. */
            const char *paren = nameEnd;
            while (*paren && *paren != '(' && *paren != '{' && *paren != ';') paren++;
            const char *arrow = NULL;
            if (*paren == '(') {
                const char *scan = paren;
                int pdepth = 0;
                const char *paramsEnd = NULL;
                while (*scan) {
                    if (*scan == '(') pdepth++;
                    else if (*scan == ')') { pdepth--; if (pdepth == 0) { paramsEnd = scan + 1; break; } }
                    scan++;
                }
                if (paramsEnd) {
                    for (const char *q = paramsEnd; q[0] && q[1]; q++) {
                        if (q[0] == '-' && q[1] == '>') { arrow = q + 2; break; }
                        if (q[0] == '{' || q[0] == ';') break;
                    }
                }
            }
            if (nameEnd > nameStart && arrow) {
                const char *rt = arrow;
                while (*rt && isspace((unsigned char)*rt)) rt++;
                if (*rt == '(') {
                    /* Capture the `(...)` return-type span (also not bounded
                     * to lineEnd, in case the tuple return type itself wraps). */
                    const char *rtEnd = rt;
                    int depth = 0;
                    while (*rtEnd) {
                        if (*rtEnd == '(') depth++;
                        else if (*rtEnd == ')') { depth--; if (depth == 0) { rtEnd++; break; } }
                        rtEnd++;
                    }
                    char **items = NULL;
                    size_t itemCount = 0;
                    if (parseTupleTypeList(rt, rtEnd, &items, &itemCount)) {
                        char *fnName = (char *)malloc((size_t)(nameEnd - nameStart) + 1);
                        if (!fnName) { for (size_t i = 0; i < itemCount; i++) free(items[i]); free(items); return false; }
                        memcpy(fnName, nameStart, (size_t)(nameEnd - nameStart));
                        fnName[nameEnd - nameStart] = '\0';
                        (*nextTupleTypeId)++;
                        int typeId = *nextTupleTypeId;
                        if (!tupleTableSet(tuples, fnName, typeId, items, itemCount)) {
                            free(fnName); for (size_t i = 0; i < itemCount; i++) free(items[i]); free(items); return false;
                        }
                        AST *typeDecl = buildSyntheticTupleRecordType(typeId, items, itemCount, lineNumber);
                        if (typeDecl) addChild(decls, typeDecl);
                        free(fnName);
                        for (size_t i = 0; i < itemCount; i++) free(items[i]);
                        free(items);
                    }
                }
            }
        }
        cursor = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;
        if (*lineEnd == '\n') lineNumber++;
    }
    return true;
}

/* Forward-declaration pre-pass: parse every top-level declaration once with the
 * `forwardScan` flag set so a top-level function call that textually precedes its
 * `fn` definition resolves. The rewriter achieves this by emitting an explicit
 * `RetType name(params);` forward declaration for every top-level function; we
 * mirror that by appending each top-level function as a body-less prototype node
 * to `decls` (so rea's compiler sees the signature before the call), and discard
 * everything else (types are registered via insertType during their parse, which
 * is idempotent; top-level const folding is suppressed via forwardScan to avoid a
 * redefinition warning the real pass would trip). Shares the caller's
 * binding/return/tuple tables. */
static void aetherRunForwardScan(const char *source, AetherBindingTable *bindings,
                                 AetherBindingTable *funcReturns, AetherTupleTable *tuples,
                                 int *nextTupleTypeId, AST *decls) {
    AetherParser fp;
    aetherParserInit(&fp, source, bindings);
    fp.funcReturns = funcReturns;
    fp.tuples = tuples;
    fp.nextTupleTypeId = nextTupleTypeId;
    fp.forwardScan = true;
    aetherAdvance(&fp);
    while (fp.current.type != REA_TOKEN_EOF && !fp.hadError) {
        collectPendingAnnotations(&fp);
        if (fp.hadError || fp.current.type == REA_TOKEN_EOF) break;
        if (isAetherKeyword(&fp.current, "fn")) {
            AST *proto = parseFnDecl(&fp); /* body-less under forwardScan */
            if (!proto) break;
            /* The rewriter does not forward-declare extension methods (rea would
             * see prototype + definition as duplicate T.f methods); skip them. */
            if (fp.lastFnWasExtension) {
                freeAST(proto);
            } else {
                addChild(decls, proto);    /* emit the prototype (rea forward decl) */
            }
        } else if (fp.current.type == REA_TOKEN_TYPE || isAetherKeyword(&fp.current, "type")) {
            AST *t = parseTypeDecl(&fp);   /* insertType registers it; discard node */
            if (!t) break;
            freeAST(t);
        } else if (fp.current.type == REA_TOKEN_CONST || isAetherKeyword(&fp.current, "const")) {
            AST *c = parseConstDeclTop(&fp);
            if (!c) break;
            freeAST(c);
        } else if (isAetherKeyword(&fp.current, "use")) {
            AST *u = parseUse(&fp);
            if (!u) break;
            freeAST(u);
        } else if (aetherIsModKeyword(&fp)) {
            AST *m = parseModuleDecl(&fp);
            if (!m) break;
            freeAST(m);
        } else {
            break; /* malformed: let the real pass report it precisely */
        }
    }
    aetherFreePending(&fp.pending);
}

/* Context + sink for folding an imported module's exported binding/return types
 * into the AST parser's tables (so `let x = ImportedConst;` /
 * `let y = importedFn();` infer their type). Const/let bindings go into
 * `bindings`; function return types go into `funcReturns`. */
typedef struct {
    AetherBindingTable *bindings;
    AetherBindingTable *funcReturns;
} AetherImportSinkCtx;

static void aetherImportTypeSink(void *ctxv, const char *name, const char *aetherType,
                                 int isFunction) {
    AetherImportSinkCtx *ctx = (AetherImportSinkCtx *)ctxv;
    if (!ctx || !name || !aetherType) return;
    if (isFunction) {
        if (ctx->funcReturns) bindingTableSet(ctx->funcReturns, name, aetherType);
    } else {
        if (ctx->bindings) bindingTableSet(ctx->bindings, name, aetherType);
    }
}

/* The TOON Bool accessors (toon_get_bool[_or], toon_bool_value,
 * toon_null_value, toon_has_key, toon_has_at) lower in the builtin pre-pass to
 * Yyjson* backends that return an Int 1/0, so they printed 1/0 when passed to
 * println, returned from a `-> Bool` fn or put in a Bool[] literal. Every such
 * call in value position becomes `(call != 0)`, a real Bool; the node keeps its
 * own parenthesis, so `!toon_get_bool(x)` stays correct. The engine's makeInt
 * is left alone (Rea prints it with %d). A bare call statement is left as is. */
static bool aetherIsToonBoolBackendCall(const AST *n) {
    if (!n || n->type != AST_PROCEDURE_CALL || !n->token || !n->token->value) return false;
    const char *v = n->token->value;
    return strcasecmp(v, "YyjsonGetBool") == 0 || strcasecmp(v, "YyjsonHasKey") == 0 ||
           strcasecmp(v, "YyjsonHasIndex") == 0 || strcasecmp(v, "YyjsonIsNull") == 0;
}

static void aetherWrapToonBoolCalls(AST *n);

static void aetherWrapToonBoolSlot(AST *parent, AST **slot) {
    AST *n = *slot;
    if (!n) return;
    aetherWrapToonBoolCalls(n);
    if (!aetherIsToonBoolBackendCall(n) || parent->type == AST_COMPOUND ||
        parent->type == AST_BLOCK) {
        return;
    }
    int line = n->token ? n->token->line : 0;
    Token *neTok = newToken(TOKEN_NOT_EQUAL, "!=", line, 0);
    AST *ne = newASTNode(AST_BINARY_OP, neTok);
    Token *zeroTok = newToken(TOKEN_INTEGER_CONST, "0", line, 0);
    AST *zero = newASTNode(AST_NUMBER, zeroTok);
    setTypeAST(zero, TYPE_INT64);
    zero->i_val = 0;
    setLeft(ne, n);
    setRight(ne, zero);
    setTypeAST(ne, TYPE_BOOLEAN);
    *slot = ne;
    ne->parent = parent;
}

static void aetherWrapToonBoolCalls(AST *n) {
    if (!n) return;
    aetherWrapToonBoolSlot(n, &n->left);
    aetherWrapToonBoolSlot(n, &n->right);
    aetherWrapToonBoolSlot(n, &n->extra);
    for (int i = 0; i < n->child_count; i++) {
        aetherWrapToonBoolSlot(n, &n->children[i]);
    }
}

/* Does this top-level subtree call the routine named `main` (bare, at any
 * depth: inside fx, if, loop or an argument)? Used by the entry-point rule. */
static bool aetherSubtreeCallsMain(const AST *n) {
    if (!n) return false;
    if (n->type == AST_PROCEDURE_CALL && n->token && n->token->value &&
        strcasecmp(n->token->value, "main") == 0) {
        return true;
    }
    if (aetherSubtreeCallsMain(n->left) || aetherSubtreeCallsMain(n->right) ||
        aetherSubtreeCallsMain(n->extra)) {
        return true;
    }
    for (int i = 0; i < n->child_count; i++) {
        if (aetherSubtreeCallsMain(n->children[i])) return true;
    }
    return false;
}

#define AETHER_ENTRY_NOTHING_HINT \
    "add `fn main() -> Void { ... }`: a file run as a program needs an entry point."

AST *parseAetherAst(const char *rawSource) {
    if (!rawSource) return NULL;

    /* Entry-point rule (D16), empty file: an entry file with nothing but
     * whitespace used to exit 1 with no message at all. A `use`d module file
     * keeps its old handling. */
    if (!reaFrontendIsParsingLibraryFile()) {
        const char *c = rawSource;
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f' || *c == '\v') c++;
        if (*c == '\0') {
            reportAetherAstErrorWithCode(1, "ENTRY-001", "nothing to run: the file is empty.",
                                         AETHER_ENTRY_NOTHING_HINT);
            return NULL;
        }
    }

    /* TOON pre-pass: lower `toon:` blocks to escaped string literals before the
     * lexer runs. This (and the two pre-passes below) live in ast_prepasses.c,
     * a translation unit the AST path owns -- the rewriter's line-based machinery
     * (translate.c) is never called from here, so its fragility cannot leak into
     * AST parsing. On failure a diagnostic is already reported. */
    const char *sourcePath = aetherSemanticGetSourcePath();
    char *toonSource = aetherAstPrepassToonBlocks(rawSource, sourcePath);
    if (!toonSource) return NULL;
    /* Builtin-alias pre-pass: lower stdlib/TOON/capability call spellings to the
     * canonical pscal builtins (toon_*->Yyjson*, has_toon/string_eq/...). Runs
     * after TOON-block extraction so `let d: TOON = "..."` literal bindings are
     * visible to the toon_parse(d) lowering. */
    char *builtinSource = aetherAstPrepassBuiltins(toonSource);
    free(toonSource);
    if (!builtinSource) return NULL;
    /* string_eq(a, b) -> (a == b): a context-free inline-call alias. Run it as a
     * pre-pass so the AST path lowers it identically to the rewriter. */
    char *source = aetherAstPrepassInlineEq(builtinSource);
    free(builtinSource);
    if (!source) return NULL;

    AetherBindingTable bindings;
    bindingTableInit(&bindings);
    AetherBindingTable funcReturns;
    bindingTableInit(&funcReturns);
    AetherTupleTable tuples;
    tupleTableInit(&tuples);
    int nextTupleTypeId = 0;

    AetherParser p;
    aetherParserInit(&p, source, &bindings);
    p.funcReturns = &funcReturns;
    p.tuples = &tuples;
    p.nextTupleTypeId = &nextTupleTypeId;
    aetherAdvance(&p);

    /* Build the AST_PROGRAM root exactly like rea parseRea: a program whose
     * `right` is an AST_BLOCK holding two AST_COMPOUND children -- declarations
     * and statements. */
    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);
    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    /* Tuple forward decls: register tuple-return signatures + emit per-slot
     * globals before parsing bodies, so `fn ... -> (T,U)`, `ret (a,b)` and
     * `let (a,b) = call()` all resolve regardless of declaration order. */
    if (!aetherRegisterTupleGlobals(source, &tuples, &nextTupleTypeId, decls)) {
        bindingTableFree(&bindings);
        bindingTableFree(&funcReturns);
        tupleTableFree(&tuples);
        freeAST(program);
        free(source);
        return NULL;
    }

    /* Imported-module types: load each `use`d module and fold its exported
     * const/let binding types + fn return types into our tables, so inferred
     * `let x = ImportedConst;` / `let y = importedFn();` resolve (this mirrors
     * the rewriter's maybeLoadImportedBindings, reimplemented in ast_prepasses.c).
     * Runs before the forward scan + real parse so both see the imported types. */
    {
        AetherImportSinkCtx sinkCtx = { &bindings, &funcReturns };
        aetherAstCollectImportedTypes(rawSource, sourcePath, aetherImportTypeSink, &sinkCtx);
    }

    /* Forward-declaration pre-pass: pre-register every top-level function/type so
     * a call that precedes its definition resolves (the rewriter emits explicit
     * forward declarations for this). Runs over the same source with throwaway
     * output; shares the tuple table (already populated above). Muted: its
     * diagnostics are redundant with (and less precise than) the real pass below,
     * so emitting them here would double-report every signature-level error. */
    int savedStderr = aetherMuteStderr();
    aetherRunForwardScan(source, &bindings, &funcReturns, &tuples, &nextTupleTypeId, decls);
    aetherUnmuteStderr(savedStderr);

    /* From here on, every diagnostic is authoritative + visible. Reset the counter
     * so the silent-failure backstop (after the loop) can tell whether this real
     * pass said anything; the forward scan's muted increments are discarded. */
    g_aetherAstDiagCount = 0;

    bool has_executable_stmt = false;
    bool stmtIsLet = false;
    int firstStmtLine = 0;
    int stmtLine = 0;
    bool has_module_decl = false;
    while (p.current.type != REA_TOKEN_EOF && !p.hadError) {
        /* Contract annotations (`@pre/@post/@pure/@cost`) precede a `fn`. */
        collectPendingAnnotations(&p);
        if (p.hadError) break;
        /* An `@`-annotation must be followed by a `fn`; otherwise it is detached.
         * Match the rewriter's ANN-001 diagnostic, pointed at the annotation line. */
        if (p.pendingAnnotCount > 0 && !isAetherKeyword(&p.current, "fn")) {
            const char *path = aetherSemanticGetSourcePath();
            if (path && *path) aetherDiagf( "%s:%d: ", path, p.pendingAnnotLine);
            aetherDiagf(
                    "[ANN-001] Aether contract error: @%s must annotate the next function declaration.\n",
                    p.pendingAnnotName);
            aetherDiagf(
                    "help: see ANN-001 in the Aether guide\n");
            p.hadError = true;
            break;
        }
        if (p.current.type == REA_TOKEN_EOF) break;
        AST *decl = NULL;
        if (isAetherKeyword(&p.current, "fn")) {
            decl = parseFnDecl(&p);
        } else if (p.current.type == REA_TOKEN_TYPE || isAetherKeyword(&p.current, "type")) {
            decl = parseTypeDecl(&p);
        } else if (p.current.type == REA_TOKEN_CONST || isAetherKeyword(&p.current, "const")) {
            decl = parseConstDeclTop(&p);
        } else if (isAetherKeyword(&p.current, "use")) {
            decl = parseUse(&p);                 /* `use X;` -> AST_USES_CLAUSE */
        } else if (aetherIsModKeyword(&p)) {
            decl = parseModuleDecl(&p);          /* `mod X { ... }` -> AST_MODULE */
            has_module_decl = true;
        } else {
            /* Bare top-level statement: a script-style program with no explicit
             * `fn main`. parseStatement parses it; appendTopLevelDecl routes
             * var/const to globals and executable statements to the program body
             * (stmts), matching the rewriter's implicit-main wrapping. */
            stmtIsLet = isAetherKeyword(&p.current, "let");
            stmtLine = p.current.line;
            decl = parseStatement(&p);
        }
        if (!decl) {
            p.hadError = true;
            break;
        }
        int stmtsBefore = stmts->child_count;
        appendTopLevelDecl(decls, stmts, decl);
        /* A top-level `let` is a declaration, not user code, even when its
         * lowering splices statements after the decl (the un-alias setlength,
         * concat steps, tuple and object-literal field stores): only a
         * statement the program wrote counts as executable. */
        if (!stmtIsLet && stmts->child_count > stmtsBefore) {
            if (!has_executable_stmt) firstStmtLine = stmtLine;
            has_executable_stmt = true;
        }
        stmtIsLet = false;
    }

    aetherFreePending(&p.pending);

    /* Undefined method on a record -> SCOPE-001 at compile time. The parser lowers
     * `recv.method()` on a user record to a call to the mangled global `Type.method`
     * (parsePostfix); with no such method it would otherwise degrade to an
     * undefined-global read that fails only at runtime ("Undefined global variable
     * 'Point.distance'"). Field reads are already caught (compiler.c FIELD-002);
     * this closes the same gap for method calls. Runs only on an otherwise-clean
     * parse, before the silent backstop so a precise message wins. */
    if (!p.hadError && aetherCheckMemberCalls(program, decls) > 0) {
        p.hadError = true;
    }

    /* Silent-failure backstop: a parse failure that set p.hadError but reported
     * nothing (a NULL propagated up a chain that only set the flag) used to exit 1
     * with empty stderr + empty --diagnostics-json -- the worst case for humans and
     * the LLM repair loop (nothing to react to). Emit a coded [SYN-001] anchored at
     * wherever parsing stalled so there is always something to react to. */
    if (p.hadError && g_aetherAstDiagCount == 0) {
        const char *bpath = aetherSemanticGetSourcePath();
        int bline = p.current.line > 0 ? p.current.line : 1;
        if (bpath && *bpath) aetherDiagf("%s:%d: ", bpath, bline);
        if (p.current.type == REA_TOKEN_EOF) {
            aetherDiagf("[SYN-001] Aether syntax error: unexpected end of input; "
                        "a declaration or statement is incomplete.\n");
        } else {
            aetherDiagf("[SYN-001] Aether syntax error: unexpected token '%.*s'; "
                        "this construct could not be parsed.\n",
                        p.current.start ? (int)p.current.length : 1,
                        p.current.start ? p.current.start : "?");
        }
        aetherDiagf("hint: check this line against the Aether guide "
                    "(fn/type/let/const/loop/if/fx/par forms).\n");
        aetherReportGuideHelp("SYN-001");
    }

    if (p.hadError) {
        bindingTableFree(&bindings);
        bindingTableFree(&funcReturns);
        tupleTableFree(&tuples);
        freeAST(program);
        free(source);
        return NULL;
    }
    bindingTableFree(&bindings);
    bindingTableFree(&funcReturns);
    tupleTableFree(&tuples);
    free(source);

    aetherWrapToonBoolCalls(program);

    /* A `use`d dependency file is not the program, so it has no entry point to
     * invoke (see the registration guard in parseFnDecl: only the entry file's
     * own main is ever the entry point). Its top-level statements still run as
     * module initialisation, but a bare `main();` written for the file's
     * standalone self-test is dropped, and none is injected below. Compiled
     * into initialisation, that call binds to the importer's main by bare name
     * and runs it before the program starts. Identical to rea parseRea's tail. */
    if (reaFrontendIsParsingLibraryFile()) {
        int kept = 0;
        for (int i = 0; i < stmts->child_count; i++) {
            AST *s = stmts->children[i];
            AST *call = (s && s->type == AST_EXPR_STMT) ? s->left : s;
            if (call && call->type == AST_PROCEDURE_CALL && call->child_count == 0 &&
                call->token && call->token->value && strcasecmp(call->token->value, "main") == 0) {
                freeAST(s);
                continue;
            }
            stmts->children[kept++] = s;
        }
        stmts->child_count = kept;
        return program;
    }

    /* If a routine named 'main' exists and there are no top-level statements,
     * inject an implicit `main()` call so the VM runs user code on start --
     * identical to rea parseRea's tail. */
    bool has_main = false;
    bool mainIsInt = false;
    VarType mainType = TYPE_VOID;
    AST *mainDecl = NULL;
    for (int i = 0; i < decls->child_count; i++) {
        AST *d = decls->children[i];
        if (!d) continue;
        if ((d->type == AST_FUNCTION_DECL || d->type == AST_PROCEDURE_DECL) &&
            d->token && d->token->value && strcasecmp(d->token->value, "main") == 0) {
            has_main = true;
            mainDecl = d;
            if (d->type == AST_FUNCTION_DECL &&
                (d->var_type == TYPE_INT64 || d->var_type == TYPE_INT32 ||
                 d->var_type == TYPE_INTEGER)) {
                mainIsInt = true;
                mainType = d->var_type;
            }
        }
    }
    /* A top-level `let` sits in `stmts` to keep its position, but it is a
     * declaration, not user code -- a file that is nothing but bindings and a
     * `fn main` still needs the implicit call. has_executable_stmt is set in
     * the parse loop above, which knows which statements a `let` produced.
     *
     * Entry-point rule (D16, ENTRY-001; this diverges from rea parseRea, which
     * stays silent): a program that would run nothing, or would skip main, is
     * rejected. Script mode -- top-level statements and no `fn main` -- stays
     * legal. */
    {
        const char *entryMsg = NULL;
        const char *entryHint = NULL;
        int entryLine = 1;
        if (mainDecl) {
            int mainLine = mainDecl->token ? mainDecl->token->line : 1;
            if (mainDecl->child_count > 0) {
                entryMsg = "fn main takes no parameters.";
                entryHint = "write `fn main() -> Void`; read command-line arguments with "
                            "paramcount() and paramstr(i).";
                entryLine = mainLine;
            } else if (mainDecl->type == AST_FUNCTION_DECL && !mainIsInt) {
                entryMsg = "fn main must return Void or Int.";
                entryHint = "write `fn main() -> Void`, or `fn main() -> Int` to set the exit status.";
                entryLine = mainLine;
            }
        }
        if (!entryMsg && has_main && has_executable_stmt) {
            bool callsMain = false;
            for (int i = 0; i < stmts->child_count && !callsMain; i++) {
                callsMain = aetherSubtreeCallsMain(stmts->children[i]);
            }
            if (!callsMain) {
                entryMsg = "fn main will not run: this file also has top-level statements "
                           "(this line is the first).";
                entryHint = "move the top-level statements into main, or end the file with `main();`.";
                entryLine = firstStmtLine;
            }
        }
        /* A file that declares a `mod` is a module file: compiled directly it
         * has nothing to run, and that is not a mistake (it is meant to be
         * `use`d), so it keeps compiling. */
        if (!entryMsg && !has_main && !has_executable_stmt && !has_module_decl) {
            entryMsg = "nothing to run: no `fn main` and no top-level statements.";
            entryHint = AETHER_ENTRY_NOTHING_HINT;
        }
        if (entryMsg) {
            reportAetherAstErrorWithCode(entryLine, "ENTRY-001", entryMsg, entryHint);
            freeAST(program);
            return NULL;
        }
    }
    if (!has_executable_stmt && has_main) {
        Token *mainTok = newToken(TOKEN_IDENTIFIER, "main", 0, 0);
        AST *call = newASTNode(AST_PROCEDURE_CALL, mainTok);
        /* `fn main() -> Int` sets the process exit status: inject
         * `halt(main())`, so `ret 2;` from main exits 2. The bare call
         * discarded the value and always exited 0. */
        if (mainIsInt) {
            setTypeAST(call, mainType);
            Token *haltTok = newToken(TOKEN_IDENTIFIER, "halt", 0, 0);
            AST *halt = newASTNode(AST_PROCEDURE_CALL, haltTok);
            addChild(halt, call);
            setTypeAST(halt, TYPE_VOID);
            /* Compiler-injected, like the contract guards' halt: outside the
             * fx fence (it would otherwise draw FX-001 on a line-0 call). */
            aetherAstRegisterSynthesizedSubtree(halt);
            call = halt;
        }
        AST *stmt = newASTNode(AST_EXPR_STMT, call->token);
        setLeft(stmt, call);
        addChild(stmts, stmt);
    }

    return program;
}
