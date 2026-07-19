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
 * to is identical, so we reproduce rea's node construction verbatim and only
 * change how the tokens are recognized:
 *
 *   - Aether keywords (fn, ret, fx, loop, let, const) are NOT Rea keywords, so
 *     the lexer hands them back as REA_TOKEN_IDENTIFIER; we match them by text.
 *   - Aether type names (Int/Real/Text/Bool/Void) arrive as identifiers too;
 *     we map them to the Rea keyword-name + VarType the rewriter would have
 *     produced (Int->"int"/INT64, Real->"float"/DOUBLE, Text->"str"/
 *     UNICODE_STRING, Bool->"bool"/BOOLEAN, Void->VOID) so the AST -- and thus
 *     program output -- matches the rewriter path byte-for-byte.
 *
 * MILESTONE 1 grammar (a slice of roadmap P2, enough to prove the pipeline):
 *   program   := { fnDecl }
 *   fnDecl    := 'fn' NAME '(' [params] ')' [ '->' Type ] block
 *   params    := param { ',' param }
 *   param     := NAME ':' Type
 *   block     := '{' { statement } '}'
 *   statement := letDecl | constDecl | ret | fxBlock | ifStmt | loopStmt
 *              | assignment | exprStmt
 *   letDecl   := ('let'|'const') NAME ':' Type [ '=' expr ] ';'
 *   ret       := 'ret' [ expr ] ';'
 *   fxBlock   := 'fx' block          (effect wrapper erased -> inner block)
 *   expr      := assignment ladder (||, &&, ==, <, +, *, unary, primary)
 */

#include "aether/parser.h"

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

/* Provided by core/utils.c */
Token *newToken(TokenType type, const char *value, int line, int column);
/* Compile-time constant folding, declared in compiler/compiler.h. Aether links
 * rea's parser which already pulls these in; declaring them here avoids adding a
 * compiler header dependency to the front end. evaluateCompileTimeValue is
 * aliased to rea_evaluateCompileTimeValue under FRONTEND_REA (see
 * common/frontend_symbol_aliases.h, pulled in transitively). */
void addCompilerConstant(const char *name_original_case, const Value *value, int line);
Value evaluateCompileTimeValue(AST *node);

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
static int aetherDiagf(const char *fmt, ...) {
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
} AetherFnPurityEntry;

typedef struct {
    const AST *node;
    char *surface;
} AetherCallSurfaceEntry;

static AetherNodePtrSet g_aetherFxBlocks;
static struct { AetherFnPurityEntry *items; size_t count; size_t cap; } g_aetherFnPurity;
static struct { AetherCallSurfaceEntry *items; size_t count; size_t cap; } g_aetherCallSurfaces;
static struct { const AST **items; size_t count; size_t cap; } g_aetherSynthesized;

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
        g_aetherFnPurity.count++;
    }
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

    for (size_t i = 0; i < g_aetherCallSurfaces.count; i++) {
        free(g_aetherCallSurfaces.items[i].surface);
    }
    free(g_aetherCallSurfaces.items);
    g_aetherCallSurfaces.items = NULL;
    g_aetherCallSurfaces.count = 0;
    g_aetherCallSurfaces.cap = 0;

    free((void *)g_aetherSynthesized.items);
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
static void reportAetherAstError(const char *path, int line, const char *kind,
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
/* Binding table -- name -> Aether type name (e.g. "Int","Real","Text",        */
/* "Bool", or a user type). Mirrors translate.c's AetherBindingTable: it is how */
/* the rewriter infers the type of `let x = <bare-name>` and method receivers.  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *typeName; /* Aether type name */
} AetherBinding;

typedef struct {
    AetherBinding *items;
    size_t count;
    size_t cap;
    /* Function-scope support (mark/restore). While a function body is being
     * parsed (scopeDepth > 0), entries appended past scopeMark are that
     * function's locals/parameters and are discarded on scope exit. A local
     * that SHADOWS a pre-scope entry does not append -- bindingTableSet
     * overwrites in place -- so the overwritten entry's original type is
     * saved in `shadowed` and restored on exit (in reverse order, so the
     * value from before the scope wins even after repeated overwrites). */
    int scopeDepth;
    size_t scopeMark;
    AetherBinding *shadowed;
    size_t shadowedCount;
    size_t shadowedCap;
} AetherBindingTable;

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

static void bindingTableSet(AetherBindingTable *t, const char *name, const char *typeName) {
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

static const char *bindingTableGet(const AetherBindingTable *t, const char *name, size_t len) {
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
/* Contract + tuple support tables (MILESTONE 3)                       */
/* ------------------------------------------------------------------ */

/* Pending `@pre`/`@post` contract expressions accumulated immediately before a
 * `fn`/method decl. Multiple `@pre` (or `@post`) lines combine with `&&` exactly
 * as the rewriter's appendContractExpr does. `@pure`/`@cost` are recorded only as
 * presence flags -- they carry no codegen (the runtime check is none; the
 * semantic layer validates them on the original source text). */
typedef struct {
    char *preExpr;   /* combined @pre expression text, or NULL  */
    char *postExpr;  /* combined @post expression text, or NULL */
    int isPure;      /* a @pure annotation precedes the next fn decl */
} AetherPendingContracts;

/* A tuple-return function signature: name -> synthetic record-free lowering via
 * per-slot globals `__aether_tuple_<id>_item<k>`. itemTypes are the Aether type
 * names of each slot (e.g. "Int","Text"). Mirrors translate.c AetherTupleSig. */
typedef struct {
    char *functionName;
    int   typeId;          /* the N in __aether_tuple_N */
    char **itemTypes;      /* Aether type names per slot */
    size_t itemCount;
} AetherTupleSig;

typedef struct {
    AetherTupleSig *items;
    size_t count;
    size_t cap;
} AetherTupleTable;

/* Field list of the type currently being parsed, so bare field references inside
 * a method's contract expression lower to `myself.<field>` the way the rewriter's
 * rewriteMethodScopedExpr does. */
typedef struct {
    char **names;
    size_t count;
    size_t cap;
} AetherFieldNameList;

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

static const AetherTupleSig *tupleTableGet(const AetherTupleTable *t, const char *name, size_t len) {
    if (!t || !name) return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (strlen(t->items[i].functionName) == len &&
            strncmp(t->items[i].functionName, name, len) == 0) {
            return &t->items[i];
        }
    }
    return NULL;
}

/* Deterministic synthetic type name for a tuple signature's record lowering,
 * e.g. "__AetherTuple3" for typeId 3. Written into a caller-supplied buffer
 * (no storage needed on AetherTupleSig -- every use site can re-derive it from
 * typeId alone). */
static void aetherTupleSyntheticTypeName(char *buf, size_t bufSize, int typeId) {
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
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

/* Synthetic token type for the Aether range operator `..`. The shared Rea lexer
 * folds `0..5` into NUMBER("0.") NUMBER(".5") and `a..b` into IDENT DOT DOT, so
 * it has no `..` token (roadmap P1). We layer a thin re-tokenizer over
 * reaNextToken() that recognizes the `..` sequence and hands back this synthetic
 * token, recovering correct numeric *and* identifier range bounds. */
#define AE_TOKEN_DOTDOT ((ReaTokenType)0x7FFF0001)

typedef struct {
    ReaLexer lexer;
    ReaToken current;        /* current (possibly synthesized) token             */
    ReaToken queue[3];       /* small FIFO of buffered tokens (pushback/lookahead)*/
    int queueHead;
    int queueCount;
    VarType currentFunctionType;
    int functionDepth;
    bool hadError;
    /* Class-context (mirrors rea's ReaParser fields used by method parsing). */
    const char *currentClassName; /* non-NULL while parsing a `type` body        */
    int currentMethodIndex;       /* v-table slot for the next method            */
    const char *currentModuleName; /* non-NULL while parsing a `mod` body; mirrors
                                     * rea's ReaParser.currentModuleName so a
                                     * member fn's procedure_table registration
                                     * uses the same "Module.func" qualified key
                                     * rea's own module-function registration and
                                     * the compiler's qualified call-site fallback
                                     * expect (see registerFunctionSymbol).       */
    AetherBindingTable *bindings;       /* in-scope name->type for inference     */
    AetherBindingTable *funcReturns;    /* fn/method (possibly-mangled) name ->   */
                                        /* Aether return-type name, for inferring */
                                        /* `let x = f(...)` / `x = recv.m(...)`.  */
    /* Contract + tuple state (MILESTONE 3). */
    AetherTupleTable *tuples;           /* tuple-return fn signatures (global)     */
    int *nextTupleTypeId;               /* monotonically-increasing tuple type id  */
    const AetherFieldNameList *classFields; /* fields of the type being parsed     */
    /* The tuple signature of the function whose body is being parsed (so `ret
     * (a,b)` lowers to per-slot global writes), NULL outside a tuple fn body.    */
    const AetherTupleSig *currentTupleSig;
    /* The combined @post expression text for the current function (already
     * method-scoped / tuple-result rewritten), consumed at each `ret`. NULL when
     * there is no @post. */
    const char *currentPostExpr;
    const char *currentFunctionName;    /* unmangled name, for guard messages      */
    /* Aether return-type name of the function whose body is being parsed (e.g.
     * "Int", "Int[]"), so a `@post` predicate's bare `result` can be type-checked
     * for comparability. NULL for Void fns and outside any fn body. */
    const char *currentReturnTypeName;
    bool currentFunctionIsMethod;
    /* When true, bare identifiers that name a current-class field lower to
     * `myself.<field>` (contract expressions inside a method). */
    bool inMethodContract;
    /* When true, this is the forward-declaration pre-pass: top-level function and
     * type signatures are registered (so forward references resolve) but the
     * produced nodes are discarded and compile-time-constant registration is
     * suppressed (it warns on the redefinition the real pass would cause). */
    bool forwardScan;
    /* Set by parseFnDecl when the just-parsed function was an extension method
     * (`fn f(self: T, ...)`). The rewriter does NOT emit a forward declaration for
     * extension methods (rea would see the prototype + definition as duplicate
     * T.f methods), so the forward scan reads this and skips the prototype. */
    bool lastFnWasExtension;
    /* Contracts accumulated from `@pre`/`@post` lines immediately before the next
     * `fn`/method decl. Consumed (and cleared) by parseFnDecl. */
    AetherPendingContracts pending;
    /* Set by collectPendingAnnotations: how many `@`-annotations it just consumed,
     * the directive of the first one, and its source line -- so the caller can emit
     * the ANN-001 "detached annotation" diagnostic when no `fn` follows. */
    int pendingAnnotCount;
    char pendingAnnotName[8];
    int pendingAnnotLine;
    /* Object literals hoisted out of expression position (array elements,
     * call args, etc.) awaiting splice into the enclosing statement -- each
     * entry is an AST_COMPOUND (i_val==1) from buildObjectInitDecl: a
     * synthesized temp var-decl plus its field assigns. Flushed by the
     * parseStatement wrapper once the statement finishes parsing. */
    AST **pendingObjLits;
    int pendingObjLitCount;
    int pendingObjLitCapacity;
    int nextObjLitId;          /* monotonic counter for unique temp names */
} AetherParser;

/* Raw next token straight from the rea lexer (no `..` synthesis), honoring the
 * small FIFO buffer used to queue synthesized/look-ahead tokens. */
static ReaToken aetherRawNext(AetherParser *p) {
    if (p->queueCount > 0) {
        ReaToken t = p->queue[p->queueHead];
        p->queueHead = (p->queueHead + 1) % 3;
        p->queueCount--;
        return t;
    }
    return reaNextToken(&p->lexer);
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
static void aetherAdvance(AetherParser *p) {
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
}

/* Queue a hoisted object-literal declaration (an i_val==1 AST_COMPOUND from
 * buildObjectInitDecl) for splice into the statement currently being parsed.
 * See the parseStatement wrapper below for where these get flushed. */
static void pushPendingObjLit(AetherParser *p, AST *hoisted) {
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

static bool isAetherKeyword(const ReaToken *t, const char *kw) {
    return t->type == REA_TOKEN_IDENTIFIER && tokTextIs(t, kw);
}

/* The Rea lexer reserves lowercase type-name words (`int`, `text`, `str`,
 * `bool`, `char`, `void`, ...) as dedicated keyword tokens. Aether spells its
 * own types capitalized (`Int`, `Text`, `Bool`), so in Aether source those
 * lowercase spellings are ordinary identifiers -- the text rewriter accepts
 * them as such because it never lexes them. The AST path must do the same:
 * wherever an Aether *identifier* is expected (a binding/param/field name, or a
 * bare variable / call reference), one of these type-keyword tokens is just as
 * valid as REA_TOKEN_IDENTIFIER. This predicate gates exactly those positions;
 * it deliberately excludes value/structure keywords (true/false/nil, if/for/
 * while, new/match, ...) so they keep their reserved meaning. */
static bool aetherTokenIsIdentifierLike(const ReaToken *t) {
    if (!t) return false;
    switch (t->type) {
        case REA_TOKEN_IDENTIFIER:
        /* Lowercase type-name keywords usable as Aether identifiers. */
        case REA_TOKEN_INT:
        case REA_TOKEN_INT64:
        case REA_TOKEN_INT32:
        case REA_TOKEN_INT16:
        case REA_TOKEN_INT8:
        case REA_TOKEN_UINT64:
        case REA_TOKEN_UINT32:
        case REA_TOKEN_UINT16:
        case REA_TOKEN_UINT8:
        case REA_TOKEN_FLOAT:
        case REA_TOKEN_FLOAT32:
        case REA_TOKEN_LONG_DOUBLE:
        case REA_TOKEN_CHAR:
        case REA_TOKEN_BYTE:
        case REA_TOKEN_WORD:
        case REA_TOKEN_STR:
        case REA_TOKEN_TEXT:
        case REA_TOKEN_MSTREAM:
        case REA_TOKEN_VOID:
        case REA_TOKEN_BOOL:
            return true;
        /* `match` is a Rea keyword, but Aether has no match-expression, so models
         * freely use it as an ordinary identifier (e.g. `let match: Bool`). Accept
         * it as a name; copyNameToken/currentAsIdentifier use the token's actual
         * text, so the real spelling is preserved. */
        case REA_TOKEN_MATCH:
            return true;
        default:
            return false;
    }
}

/* Classify a token that turned up where an Aether *member name* (a `type` field
 * or a `fn` name) was expected, but which Rea's lexer had reserved as a keyword
 * or operator word -- so the diagnostic can name the collision instead of a bare
 * "unexpected token" / "expected function name". Returns a short human category,
 * or NULL when the token is not a word-shaped reserved token (punctuation, a
 * literal, EOF, or an ordinary identifier) and the caller should keep its generic
 * message. Note on reachability: aetherTokenIsIdentifierLike already accepts the
 * lowercase type-name keywords (int/word/text/...) at the `fn`/param positions, so
 * at the `fn` site only operator words / value-structure keywords reach here; the
 * "reserved type name" branch fires at the field site, whose acceptance test is
 * REA_TOKEN_IDENTIFIER exactly. */
static const char *aetherReservedWordCategory(const ReaToken *t) {
    if (!t || t->length == 0 || !t->start) return NULL;
    unsigned char c0 = (unsigned char)t->start[0];
    if (!(isalpha(c0) || c0 == '_')) return NULL; /* only word-shaped lexemes collide */
    switch (t->type) {
        case REA_TOKEN_IDENTIFIER:
            return NULL; /* an ordinary identifier is a perfectly good name */
        /* lowercase type-name keywords: int/word/text/bool/char/byte/void/... */
        case REA_TOKEN_INT: case REA_TOKEN_INT64: case REA_TOKEN_INT32:
        case REA_TOKEN_INT16: case REA_TOKEN_INT8:
        case REA_TOKEN_UINT64: case REA_TOKEN_UINT32: case REA_TOKEN_UINT16:
        case REA_TOKEN_UINT8:
        case REA_TOKEN_FLOAT: case REA_TOKEN_FLOAT32: case REA_TOKEN_LONG_DOUBLE:
        case REA_TOKEN_CHAR: case REA_TOKEN_BYTE: case REA_TOKEN_WORD:
        case REA_TOKEN_STR: case REA_TOKEN_TEXT: case REA_TOKEN_MSTREAM:
        case REA_TOKEN_VOID: case REA_TOKEN_BOOL:
            return "reserved type name";
        /* word forms of arithmetic operators: mul(*), div, mod(%), xor */
        case REA_TOKEN_STAR: case REA_TOKEN_INT_DIV:
        case REA_TOKEN_PERCENT: case REA_TOKEN_XOR:
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
                 "rename it (e.g. `%sValue`); reserved type names (Int/Word/Text/...), "
                 "keywords (new/for/if/...) and operator words (mul/div/mod/xor) are not "
                 "valid field or method names.", name);
    }
    reportAetherAstError(aetherSemanticGetSourcePath(), t->line, "declaration", detail, hint);
    return true;
}

/* Map an Aether stdlib builtin name to its canonical Rea/pscal builtin, exactly
 * as translate.c appendAetherBuiltinAlias() does textually. Returns the canonical
 * name, or `name` unchanged if there is no alias. Keeping this in sync with the
 * rewriter is what lets call output match byte-for-byte (e.g. println -> writeln
 * -> AST_WRITELN). */
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
 * followed by rea mapType(), which is the byte-for-byte contract: the type node
 * token value and var_type must equal what the rewriter+parseRea produce. */
static bool mapAetherType(const char *name, size_t len,
                          const char **outReaName, VarType *outType) {
    struct { const char *aether; const char *rea; VarType vt; } table[] = {
        { "Int",     "int",   TYPE_INT64 },
        { "Real",    "float", TYPE_DOUBLE },
        { "Float",   "float", TYPE_DOUBLE },
        { "Text",    "str",   TYPE_UNICODE_STRING },
        { "Bool",    "bool",  TYPE_BOOLEAN },
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

/* Build the type node for a value-bearing type (non-Void), mirroring how rea's
 * parseVarDecl builds the type node:
 *   - builtin keyword type -> AST_TYPE_IDENTIFIER with the mapped VarType.
 *   - user type that resolves (via lookupType) to a record/class -> a POINTER:
 *     AST_POINTER_TYPE -> AST_TYPE_REFERENCE(TYPE_RECORD), var_type POINTER
 *     (object variables are pointers; this is what makes `c.method()` type-check
 *     and matches the rewriter's `Counter c = ...;` lowering byte for byte).
 *   - otherwise an AST_TYPE_REFERENCE with TYPE_UNKNOWN. */
static AST *buildTypeNode(const char *name, size_t len, int line, VarType *outType) {
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
        if (!resolved->token) freeAST(resolved);
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
static AST *buildTypeNodeFromName(const char *name, size_t len, int line, VarType *outType) {
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
/* Type promotion helpers (verbatim from rea parser.c)                 */
/* ------------------------------------------------------------------ */

static VarType promoteRealBinaryType(VarType a, VarType b) {
    if (a == TYPE_LONG_DOUBLE || b == TYPE_LONG_DOUBLE) return TYPE_LONG_DOUBLE;
    if (a == TYPE_DOUBLE || b == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (a == TYPE_FLOAT || b == TYPE_FLOAT) return TYPE_FLOAT;
    return TYPE_DOUBLE;
}

static VarType promoteIntegralBinaryType(VarType a, VarType b) {
    if (a == TYPE_UNKNOWN) return b == TYPE_UNKNOWN ? TYPE_INT32 : b;
    if (b == TYPE_UNKNOWN) return a;
    static const VarType order[] = {
        TYPE_INT64, TYPE_UINT64, TYPE_INT32, TYPE_UINT32,
        TYPE_INT16, TYPE_UINT16, TYPE_INT8, TYPE_UINT8,
        TYPE_WORD, TYPE_BYTE, TYPE_BOOLEAN, TYPE_CHAR
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (a == order[i] || b == order[i]) return order[i];
    }
    return TYPE_INT32;
}

static VarType inferStringLiteralType(const char *text, size_t len) {
    if (!text) return TYPE_STRING;
    if (len == 0) return TYPE_STRING;
    uint32_t codepoint = 0;
    size_t advance = 0;
    if (decodeUtf8Codepoint(text, len, &codepoint, &advance) && advance == len) {
        /* A single codepoint in a DOUBLE-quoted literal is a one-character string,
         * not a Char (the Char literal is the single-quote 'x' form). ASCII -> the
         * Pascal String; a non-ASCII codepoint stays WIDECHAR. */
        return (codepoint <= 127u) ? TYPE_STRING : TYPE_WIDECHAR;
    }
    if (isValidUtf8Bytes(text, len) && utf8CodepointCount(text, len) < len) {
        return TYPE_UNICODE_STRING;
    }
    return TYPE_STRING;
}

/* ------------------------------------------------------------------ */
/* Conditional (if-expression / ternary) type resolution               */
/* (verbatim from rea parser.c promoteConditionalNumericType +          */
/*  resolveConditionalType, minus the type_def plumbing which the AST    */
/*  parser does not yet track for value-position conditionals)          */
/* ------------------------------------------------------------------ */

static VarType promoteConditionalNumericType(VarType a, VarType b) {
    static const VarType order[] = {
        TYPE_LONG_DOUBLE, TYPE_DOUBLE, TYPE_FLOAT,
        TYPE_INT64, TYPE_UINT64, TYPE_INT32, TYPE_UINT32,
        TYPE_INT16, TYPE_UINT16, TYPE_INT8, TYPE_UINT8,
        TYPE_WORD, TYPE_BYTE
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (a == order[i] || b == order[i]) return order[i];
    }
    return TYPE_UNKNOWN;
}

static VarType resolveConditionalType(AST *thenExpr, AST *elseExpr) {
    VarType thenType = thenExpr ? thenExpr->var_type : TYPE_UNKNOWN;
    VarType elseType = elseExpr ? elseExpr->var_type : TYPE_UNKNOWN;

    if (thenType == elseType) return thenType;
    if (thenType == TYPE_UNKNOWN) return elseType;
    if (elseType == TYPE_UNKNOWN) return thenType;
    if ((thenType == TYPE_POINTER && elseType == TYPE_NIL) ||
        (thenType == TYPE_NIL && elseType == TYPE_POINTER)) {
        return TYPE_POINTER;
    }
    if (thenType == TYPE_POINTER && elseType == TYPE_POINTER) return TYPE_POINTER;
    if (isPascalStringType(thenType) || isPascalStringType(elseType) ||
        isPascalCharType(thenType) || isPascalCharType(elseType)) {
        return inferBinaryOpType(thenType, elseType);
    }
    if (thenType == TYPE_CHAR && elseType == TYPE_CHAR) return TYPE_CHAR;
    if (thenType == TYPE_BOOLEAN && elseType == TYPE_BOOLEAN) return TYPE_BOOLEAN;
    VarType numeric = promoteConditionalNumericType(thenType, elseType);
    if (numeric != TYPE_UNKNOWN) return numeric;
    return thenType;
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

static AST *parseExpr(AetherParser *p);
static AST *parseStatement(AetherParser *p);
static AST *parseBlock(AetherParser *p);
static AST *buildObjectInitDecl(Token *nameTok, AST *typeNode, VarType vtype,
                                const char *typeName, AST *lit, int line);
static AST *parseFnDecl(AetherParser *p);
static AST *parseTypeDecl(AetherParser *p);
static AST *parseConstDeclTop(AetherParser *p);
static char *inferLetTypeName(AetherParser *p, AST *init);
static void buildArrayAppendSteps(const AST *target, AST *item, int line,
                                  AST **outSetlenStmt, AST **outIdxAssign);
static AST *parseExprFromText(AetherParser *p, const char *text, int line,
                             bool inMethodContract);
static AST *buildContractGuard(AetherParser *p, const char *exprText,
                              const char *kind, const char *fnName, int line);
static AST *parseLetTupleDestructure(AetherParser *p, int kwLine);

/* ------------------------------------------------------------------ */
/* Primary / call expressions (mirrors rea parseFactor primary cases)  */
/* ------------------------------------------------------------------ */

static AST *parseStringLiteral(AetherParser *p) {
    int startLine = p->current.line;
    size_t totalLen = 0;
    size_t capacity = 0;
    char *buffer = NULL;
    bool haveSegment = false;
    bool charCandidate = false;

    while (p->current.type == REA_TOKEN_STRING) {
        size_t tokenLen = p->current.length;
        if (tokenLen < 2) { free(buffer); return NULL; }
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
        if (!arg) break;
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
static Token *copyNameToken(AetherParser *p) {
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

static AST *parseRecordInitBlock(AetherParser *p) {
    return parseRecordInitDelimited(p, REA_TOKEN_RIGHT_BRACE);
}

/* Apply postfix `.field` / `.method(args)` / `[index]` chains to `base`,
 * mirroring rea parseFactor's member-access loop (the bare-identifier branch:
 * no name mangling for an ordinary receiver -- the bare method name resolves via
 * the alias rea registers for each class method). `myself`/`self` receivers DO
 * get mangled to ClassName.method, matching rea. */
static AST *parsePostfix(AetherParser *p, AST *base) {
    AST *node = base;
    while (p->current.type == REA_TOKEN_DOT || p->current.type == REA_TOKEN_LEFT_BRACKET) {
        if (p->current.type == REA_TOKEN_LEFT_BRACKET) {
            /* array index: base[expr] -> AST_ARRAY_ACCESS (rea parseArrayAccess). */
            int openLine = p->current.line;
            aetherAdvance(p); /* consume '[' */
            AST *index = parseExpr(p);
            if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
                aetherAdvance(p);
            } else if (!p->hadError) {
                char msg[96];
                snprintf(msg, sizeof(msg), "expected ']' to close index expression (opened at line %d).", openLine);
                reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser", msg, NULL);
                p->hadError = true;
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
                        if (!rtyNode->token) freeAST(rtyNode);
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
    if (!cond) { p->hadError = true; return NULL; }
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
    if (!thenExpr) { p->hadError = true; freeAST(cond); return NULL; }
    if (p->current.type != REA_TOKEN_ELSE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "if-expression requires an 'else' branch.", NULL);
        p->hadError = true;
        freeAST(cond); freeAST(thenExpr);
        return NULL;
    }
    aetherAdvance(p); /* consume 'else' */
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '{' after 'else' in if-expression.", NULL);
        p->hadError = true;
        freeAST(cond); freeAST(thenExpr);
        return NULL;
    }
    aetherAdvance(p); /* consume '{' */
    AST *elseExpr = parseExpr(p);
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
    if (!elseExpr) { p->hadError = true; freeAST(cond); freeAST(thenExpr); return NULL; }

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
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_MINUS, "-", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, right->var_type);
        return node;
    }
    /* Unary not */
    if (p->current.type == REA_TOKEN_BANG) {
        ReaToken op = p->current;
        aetherAdvance(p);
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
            if (!element) break;
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
            const char *canonical = aliasBuiltinName(raw);
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

static AST *parseAdd(AetherParser *p) {
    AST *node = parseMul(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        aetherAdvance(p);
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

static AST *parseComparison(AetherParser *p) {
    AST *node = parseAdd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL ||
           p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseAdd(p);
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

static AST *parseLogicalAnd(AetherParser *p) {
    AST *node = parseEquality(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND_AND) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseEquality(p);
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
    while (p->current.type == REA_TOKEN_OR_OR) {
        ReaToken op = p->current;
        aetherAdvance(p);
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
    AST *thenBranch = parseExpr(p);
    if (!thenBranch) { p->hadError = true; freeAST(cond); return NULL; }
    if (p->current.type != REA_TOKEN_COLON) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "Expected ':' in conditional expression.", NULL);
        p->hadError = true;
        freeAST(cond); freeAST(thenBranch);
        return NULL;
    }
    aetherAdvance(p); /* consume ':' */
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
static AST *parseExpr(AetherParser *p) {
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
        AST *value = parseExpr(p);
        if (!value) return NULL;
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
static AST *buildContractGuard(AetherParser *p, const char *exprText,
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

/* Map a VarType back to the Aether builtin type *name*, so an inferred binding
 * can be recorded the way the rewriter records it. Mirrors the inverse of
 * mapAetherType for the scalar builtins; returns NULL for non-builtins. */
static const char *aetherTypeNameForVarType(VarType vt) {
    switch (vt) {
        case TYPE_INT64: case TYPE_INT32: case TYPE_INT16: case TYPE_INT8:
        case TYPE_UINT64: case TYPE_UINT32: case TYPE_UINT16: case TYPE_UINT8:
        case TYPE_WORD: case TYPE_BYTE:
            return "Int";
        case TYPE_DOUBLE: case TYPE_FLOAT: case TYPE_LONG_DOUBLE:
            return "Real";
        case TYPE_STRING: case TYPE_UNICODE_STRING:
            return "Text";
        case TYPE_BOOLEAN:
            return "Bool";
        case TYPE_CHAR: case TYPE_WIDECHAR:
            return "Text";
        case TYPE_MEMORYSTREAM:
            return "MStream";
        default:
            return NULL;
    }
}

/* Return the Aether return-type name of a known stdlib helper, by the helper's
 * CANONICAL (already-aliased) name as the call node carries it. Ported from
 * translate.c inferHelperReturnTypeName, but keyed on the canonical builtin name
 * (e.g. `hasextbuiltin` rather than `has_toon`, `YyjsonRead` rather than
 * `toon_parse`) since parsePrimary aliases the name before the call node exists. */
static const char *inferBuiltinReturnTypeName(const char *name) {
    if (!name) return NULL;
    struct { const char *fn; const char *ret; } table[] = {
        /* TOON doc/node handles (the EXACT canonical Yyjson* names the builtin
         * pre-pass + parsePrimary aliasing produce -- see appendJsonAliasReplacement
         * / toonScalarGetterForName in translate.c). */
        { "YyjsonRead", "ToonDoc" }, { "YyjsonReadFile", "ToonDoc" },
        { "YyjsonGetRoot", "ToonNode" }, { "YyjsonGetKey", "ToonNode" },
        { "YyjsonGetIndex", "ToonNode" },
        /* Memory-stream handles (vm builtin names; lookup is case-insensitive). */
        { "mstreamcreate", "MStream" }, { "mstreamfromstring", "MStream" },
        { "socketreceive", "MStream" },
        { "mstreambuffer", "Text" },
        { "mstreamloadfromfile", "Bool" }, { "mstreamsavetofile", "Bool" },
        /* HTTP surface: session handles and response status are plain Ints. */
        { "httpsession", "Int" }, { "httprequest", "Int" },
        /* Text-returning. */
        { "YyjsonGetString", "Text" }, { "ai_chat", "Text" },
        { "openaichatcompletions", "Text" },
        { "aetherbuiltinsjson", "Text" }, { "aetherbuiltininfo", "Text" },
        { "inttostr", "Text" },
        /* Int-returning. */
        { "length", "Int" }, { "YyjsonGetInt", "Int" }, { "YyjsonGetLength", "Int" },
        /* Real-returning. */
        { "YyjsonGetNumber", "Real" },
        /* Math: pow/power -> Real. (int^int may be exact-Int at runtime, but Real is
         * the safe inferred type; matches pscal-core's builtin-return type list.) */
        { "pow", "Real" }, { "power", "Real" },
        /* Bool-returning. */
        { "hasextbuiltin", "Bool" },
        { "YyjsonGetBool", "Bool" }, { "YyjsonIsNull", "Bool" },
        { "YyjsonHasKey", "Bool" }, { "YyjsonHasIndex", "Bool" },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcasecmp(name, table[i].fn) == 0) return table[i].ret;
    }
    return NULL;
}

/* Infer the Aether type *name* of an initializer expression for an
 * inferred `let`/`const`, mirroring translate.c inferAetherBindingTypeName: a
 * bare name resolves through the in-scope binding table; a `new T`/record
 * literal yields the class name; a function/method call resolves through the
 * recorded return-type table (user fns + builtins); otherwise we fall back to
 * the expression's computed var_type. Returns a malloc'd name or NULL. */
static char *inferLetTypeName(AetherParser *p, AST *init) {
    if (!init) return NULL;
    /* new T(...) / record literal -> the class name. */
    if (init->type == AST_NEW && init->token && init->token->value) {
        return strdup(init->token->value);
    }
    /* bare identifier -> its recorded binding type. */
    if (init->type == AST_VARIABLE && init->token && init->token->value) {
        const char *bt = bindingTableGet(p->bindings, init->token->value,
                                         strlen(init->token->value));
        if (bt) return strdup(bt);
    }
    /* function / method call -> recorded return type or builtin return type. The
     * call token carries the (possibly mangled) callee name. */
    if (init->type == AST_PROCEDURE_CALL && init->token && init->token->value) {
        const char *rt = bindingTableGet(p->funcReturns, init->token->value,
                                         strlen(init->token->value));
        if (rt) return strdup(rt);
        rt = inferBuiltinReturnTypeName(init->token->value);
        if (rt) return strdup(rt);
    }
    /* Ternary `(c ? a : b)` -> infer from a branch (the shape the builtin pre-pass
     * emits for `toon_get_*_or(...)`: `(YyjsonHasKey(...) ? YyjsonGet*(...) : def)`).
     * The rewriter infers these from the original `toon_get_*_or` helper; we recover
     * the type from the then-branch getter call (or the else default). */
    if (init->type == AST_TERNARY) {
        char *t = inferLetTypeName(p, init->right);
        if (t) return t;
        t = inferLetTypeName(p, init->extra);
        if (t) return t;
    }
    /* Array index `arr[i]` -> the array's element type: infer the base array's type
     * name (e.g. "Int[]") and strip one trailing "[]". Works for Int[]/Text[]/Real[]
     * and one dimension of a nested array; the base is AST_ARRAY_ACCESS->left. */
    if (init->type == AST_ARRAY_ACCESS && init->left) {
        char *base = inferLetTypeName(p, init->left);
        if (base) {
            size_t n = strlen(base);
            if (n >= 2 && base[n - 2] == '[' && base[n - 1] == ']') {
                base[n - 2] = '\0';
                return base;
            }
            free(base);
        }
    }
    /* Mixed string/non-string arithmetic (e.g. `tag + 1`, Text + Int) has no
     * inferable result type -> NULL, so the caller emits "cannot infer" like the
     * rewriter. Checked before the var_type fallback below, which would otherwise
     * name the operation's (misleading) promoted type. */
    if (init->type == AST_BINARY_OP && init->token && init->token->value) {
        const char *bop = init->token->value;
        if (strcmp(bop, "+") == 0 || strcmp(bop, "-") == 0 || strcmp(bop, "*") == 0 ||
            strcmp(bop, "div") == 0 || strcmp(bop, "mod") == 0) {
            char *lt = inferLetTypeName(p, init->left);
            char *rt = inferLetTypeName(p, init->right);
            int lStr = lt && (strcmp(lt, "Text") == 0 || strcmp(lt, "str") == 0);
            int rStr = rt && (strcmp(rt, "Text") == 0 || strcmp(rt, "str") == 0);
            int mixed = lt && rt && (lStr != rStr);
            free(lt);
            free(rt);
            if (mixed) return NULL;
        }
    }
    /* fall back to the expression's own computed var_type. */
    const char *name = aetherTypeNameForVarType(init->var_type);
    if (name) return strdup(name);
    /* Arithmetic binary op whose operand types we can't name at parse time (e.g.
     * `let x = self.field + n`, where neither operand is scope-resolved here): default
     * to the dominant Int, or Real for `/`. Relational/logical ops are already typed
     * BOOLEAN above, so they're handled by the var_type fallback. */
    if (init->type == AST_BINARY_OP && init->token && init->token->value) {
        const char *op = init->token->value;
        if (strcmp(op, "/") == 0) return strdup("Real");
        if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
            strcmp(op, "div") == 0 || strcmp(op, "mod") == 0)
            return strdup("Int");
    }
    return NULL;
}

/* Expand a typed object-literal initializer `let x: T = T { f: v, ... };` into
 * the rea shape the rewriter produces: an AST_VAR_DECL with init = AST_NEW(T)
 * (no record-init block) followed by one AST_ASSIGN(x.f = v) per field. The
 * caller passes the already-parsed AST_NEW `lit` (built from the bare `T { }`
 * form). Returns an AST_COMPOUND[ var-decl, x.f=v ... ], or NULL on mismatch. */
static AST *buildObjectInitDecl(Token *nameTok, AST *typeNode, VarType vtype,
                                const char *typeName, AST *lit, int line) {
    /* lit is AST_NEW with extra = AST_COMPOUND of field AST_ASSIGNs. */
    AST *inits = lit->extra;
    /* Strip the record-init block off the NEW so it becomes a plain `new T()`. */
    lit->extra = NULL;

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, lit);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    /* Mark as a declaration-group wrapper (rea convention) so the block parser
     * splices these statements as siblings -- otherwise the nested COMPOUND
     * would scope the new variable away from later sibling statements. */
    outer->i_val = 1;
    addChild(outer, decl);

    if (inits) {
        for (int i = 0; i < inits->child_count; i++) {
            AST *fa = inits->children[i];
            if (!fa || fa->type != AST_ASSIGN) continue;
            AST *fieldVar = fa->left;   /* AST_VARIABLE(field) */
            AST *value = fa->right;
            if (!fieldVar || !fieldVar->token) continue;
            /* Build  x.field = value;  */
            Token *recvTok = newToken(TOKEN_IDENTIFIER, nameTok->value, line, 0);
            AST *recv = newASTNode(AST_VARIABLE, recvTok);
            setTypeAST(recv, vtype);
            Token *fldTok = newToken(TOKEN_IDENTIFIER, fieldVar->token->value, line, 0);
            AST *fldVar2 = newASTNode(AST_VARIABLE, fldTok);
            AST *fldAccess = newASTNode(AST_FIELD_ACCESS, fldTok);
            setLeft(fldAccess, recv);
            setRight(fldAccess, fldVar2);
            Token *asgnTok = newToken(TOKEN_ASSIGN, "=", line, 0);
            AST *assign = newASTNode(AST_ASSIGN, asgnTok);
            setLeft(assign, fldAccess);
            /* Move the value out of the literal's init compound. */
            setRight(assign, value);
            fa->right = NULL;
            setTypeAST(assign, value ? value->var_type : TYPE_UNKNOWN);
            addChild(outer, assign);
        }
        freeAST(inits);
    }
    (void)typeName;
    return outer;
}

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
        aetherAdvance(p); /* consume '=' */

        /* Inline object-method: `let x = T { f: v, ... }.method(args);`. The
         * rewriter hoists the inline object literal into a temp
         * `__aether_obj_<line>`, assigns its fields, then binds x to
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
            if (resolvedTy && !resolvedTy->token) freeAST(resolvedTy);
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
                /* Build the temp object: __aether_obj_<line> = new T(); field = v; */
                Token *clsTok = copyNameToken(p);
                aetherAdvance(p); /* consume type name */
                VarType objVt = TYPE_UNKNOWN;
                AST *objTypeNode = buildTypeNode(probeName, strlen(probeName), kwLine, &objVt);
                AST *newNode = newASTNode(AST_NEW, clsTok);
                setTypeAST(newNode, TYPE_POINTER);
                AST *objInits = parseRecordInitBlock(p);

                char tempName[64];
                snprintf(tempName, sizeof(tempName), "__aether_obj_%d", kwLine);
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
            /* Not an object literal after all: treat the consumed name as a bare
             * variable reference and continue postfix parsing. */
            AST *var = newASTNode(AST_VARIABLE, clsTok);
            setTypeAST(var, TYPE_UNKNOWN);
            init = parsePostfix(p, var);
        } else {
            init = parseExpr(p);
        }
        if (!init) {
            freeToken(nameTok);
            if (typeNode) freeAST(typeNode);
            free(declaredTypeName);
            return NULL;
        }
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }

    /* Array-append initializer: `let x: T[] = src + [item];` / `let x = src +
     * [item];`. Rewrite `init` in place to just `src` (a plain array-copy
     * initializer, which the normal declaration path below already handles
     * correctly -- arrays are value types) and remember `item`/`appendLine` so
     * the setlength+indexed-assign steps can be spliced in after the
     * declaration once `decl` is built. Without this, `src + [item]` compiles
     * as a literal `ARRAY + ARRAY_LITERAL` binary op the VM has no operator
     * for ("Runtime Error: Operands must be numbers for arithmetic operation
     * '+'... Got ARRAY and ARRAY") -- the only shape that ever worked was the
     * self-reassignment statement `xs = xs + [v];`, handled separately below
     * in parseStmt. */
    AST *appendItem = NULL;
    int appendLine = 0;
    if (init && init->type == AST_BINARY_OP && init->token &&
        init->token->type == TOKEN_PLUS &&
        init->right && init->right->type == AST_ARRAY_LITERAL &&
        init->right->child_count == 1) {
        appendLine = init->token->line;
        AST *src = init->left;
        appendItem = init->right->children[0];
        init->right->children[0] = NULL;
        init->right->child_count = 0;
        if (appendItem) appendItem->parent = NULL;
        init->left = NULL;
        freeAST(init); /* frees the now-empty `+`/array-literal shell only */
        init = src;
        if (init) init->parent = NULL;
    }

    /* Binding a tuple-return call to a single name is unsupported: `let v = pair();`
     * where `pair` returns `(...)`. Match the rewriter's specific diagnostic rather
     * than the generic "cannot infer type" so error-message parity holds. Applies
     * regardless of explicit type, like the rewriter. */
    if (init && init->type == AST_PROCEDURE_CALL && init->token && init->token->value &&
        p->tuples &&
        tupleTableGet(p->tuples, init->token->value, strlen(init->token->value))) {
        reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "tuple",
                             "tuple-return calls must be destructured directly.",
                             "use `let (a, b) = pair();` rather than binding the tuple call to one name.");
        p->hadError = true;
        freeToken(nameTok);
        if (typeNode) freeAST(typeNode);
        free(declaredTypeName);
        freeAST(init);
        return NULL;
    }

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
    free(declaredTypeName);

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);
    if (appendItem) {
        /* `let x: T[] = src + [item];` -- `decl` above now declares `x` as a
         * copy of `src` (init was rewritten to `src` further up); splice in
         * the setlength+indexed-assign steps right after it. */
        AST *setlenStmt = NULL, *idxAssign = NULL;
        buildArrayAppendSteps(var, appendItem, appendLine, &setlenStmt, &idxAssign);
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        outer->i_val = 1; /* splice into the surrounding block, like buildArrayAppend's */
        addChild(outer, decl);
        addChild(outer, setlenStmt);
        addChild(outer, idxAssign);
        return outer;
    }
    return decl;
}

/* Structural equality for the lvalue chains that can appear on both sides of an
 * array-append (`xs = xs + [v]` / `box.values = box.values + [v]`): a bare
 * variable, a field-access chain, or an array access. The rewriter's append
 * detection compares the LHS and the `+` left operand as TEXT; we compare the
 * already-parsed nodes, which is the same relation. */
/* Is `node` an assignable lvalue chain (bare variable, field-access chain, or
 * array-access chain) -- the same shapes aetherLValueEqual compares? Used to
 * validate the *assignment target* of an array-append expansion even when
 * there's no second lvalue to structurally compare it against (the `let x: T[]
 * = src + [v];` and `x = src + [v];` with `x != src` cases). */
static bool aetherIsLValueChain(const AST *node) {
    if (!node) return false;
    switch (node->type) {
        case AST_VARIABLE:
            return node->token && node->token->value;
        case AST_FIELD_ACCESS:
            return node->token && node->token->value && aetherIsLValueChain(node->left);
        case AST_ARRAY_ACCESS:
            return aetherIsLValueChain(node->left);
        default:
            return false;
    }
}

static bool aetherLValueEqual(const AST *a, const AST *b) {
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    switch (a->type) {
        case AST_VARIABLE:
            return a->token && b->token && a->token->value && b->token->value &&
                   strcmp(a->token->value, b->token->value) == 0;
        case AST_FIELD_ACCESS:
            /* token is the field name; left is the receiver chain. */
            if (!(a->token && b->token && a->token->value && b->token->value &&
                  strcmp(a->token->value, b->token->value) == 0))
                return false;
            return aetherLValueEqual(a->left, b->left);
        case AST_ARRAY_ACCESS:
            return aetherLValueEqual(a->left, b->left);
        default:
            return false;
    }
}

/* Build `length(<target-copy>)` as an AST_PROCEDURE_CALL (INTEGER), the call the
 * rewriter emits inside its setlength/index-assign append expansion. */
static AST *buildLengthCall(const AST *target, int line) {
    Token *lenTok = newToken(TOKEN_IDENTIFIER, "length", line, 0);
    AST *call = newASTNode(AST_PROCEDURE_CALL, lenTok);
    addChild(call, copyAST((AST *)target));
    setTypeAST(call, TYPE_INTEGER);
    return call;
}

/* Build an integer constant node. */
static AST *buildIntLiteral(long v, int line) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", v);
    Token *tok = newToken(TOKEN_INTEGER_CONST, buf, line, 0);
    AST *node = newASTNode(AST_NUMBER, tok);
    setTypeAST(node, TYPE_INT64);
    node->i_val = v;
    return node;
}

/* Build the two append statements shared by every array-append expansion:
 *     setlength(target, length(target) + 1);
 *     target[length(target) - 1] = item;
 * `target` is only read (copied); ownership of `item` transfers to the
 * returned `*outIdxAssign`. */
static void buildArrayAppendSteps(const AST *target, AST *item, int line,
                                  AST **outSetlenStmt, AST **outIdxAssign) {
    Token *slTok = newToken(TOKEN_IDENTIFIER, "setlength", line, 0);
    AST *setlen = newASTNode(AST_PROCEDURE_CALL, slTok);
    addChild(setlen, copyAST((AST *)target));
    Token *plusTok = newToken(TOKEN_PLUS, "+", line, 0);
    AST *lenPlus1 = newASTNode(AST_BINARY_OP, plusTok);
    setLeft(lenPlus1, buildLengthCall(target, line));
    setRight(lenPlus1, buildIntLiteral(1, line));
    setTypeAST(lenPlus1, TYPE_INTEGER);
    addChild(setlen, lenPlus1);
    setTypeAST(setlen, TYPE_VOID);
    AST *setlenStmt = newASTNode(AST_EXPR_STMT, setlen->token);
    setLeft(setlenStmt, setlen);

    Token *minusTok = newToken(TOKEN_MINUS, "-", line, 0);
    AST *lenMinus1 = newASTNode(AST_BINARY_OP, minusTok);
    setLeft(lenMinus1, buildLengthCall(target, line));
    setRight(lenMinus1, buildIntLiteral(1, line));
    setTypeAST(lenMinus1, TYPE_INTEGER);
    AST *access = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(access, copyAST((AST *)target));
    addChild(access, lenMinus1);
    setTypeAST(access, TYPE_UNKNOWN);
    Token *aTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *idxAssign = newASTNode(AST_ASSIGN, aTok);
    setLeft(idxAssign, access);
    setRight(idxAssign, item);
    setTypeAST(idxAssign, item ? item->var_type : TYPE_UNKNOWN);

    *outSetlenStmt = setlenStmt;
    *outIdxAssign = idxAssign;
}

/* Expand `target = src + [item]` into the two/three-statement append the
 * rewriter's self-reassignment case emits (translate.c
 * translateArrayAppendLine), generalized to any `src` (not just `src ==
 * target`):
 *     target = src;                       -- only when src != target
 *     setlength(target, length(target) + 1);
 *     target[length(target) - 1] = item;
 * When `src` is NULL or structurally equal to `target` (the original,
 * narrower self-reassignment shape this function used to require), the copy
 * step is skipped -- `target` already holds the array to extend, exactly as
 * before. This is what lets `let b: Int[] = a + [3];` and `b = a + [3];`
 * (`b` a different, already-declared array than `a`) lower the same way
 * `xs = xs + [v]` always has, instead of falling through to a raw
 * `ARRAY + ARRAY_LITERAL` binary op the VM has no operator for (previously:
 * "Runtime Error: Operands must be numbers for arithmetic operation '+'...
 * Got ARRAY and ARRAY").
 * Returns an AST_COMPOUND splice (i_val==1) so parseBlock flattens it; the caller
 * has already verified the shape. `assign` is consumed (its target + the item are
 * reused/freed). */
static AST *buildArrayAppend(AST *assign, AST *target, AST *item, int line, AST *src) {
    AST *copyStmt = NULL;
    if (src && !aetherLValueEqual(target, src)) {
        Token *aTok = newToken(TOKEN_ASSIGN, "=", line, 0);
        AST *copyAssign = newASTNode(AST_ASSIGN, aTok);
        setLeft(copyAssign, copyAST(target));
        setRight(copyAssign, copyAST(src));
        setTypeAST(copyAssign, target->var_type);
        copyStmt = newASTNode(AST_EXPR_STMT, copyAssign->token);
        setLeft(copyStmt, copyAssign);
    }
    AST *setlenStmt = NULL, *idxAssign = NULL;
    buildArrayAppendSteps(target, item, line, &setlenStmt, &idxAssign);

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */
    if (copyStmt) addChild(outer, copyStmt);
    addChild(outer, setlenStmt);
    addChild(outer, idxAssign);

    /* The original assign node's target was copied; release it along with the
     * now-detached `+` expression (item was moved out, so detach it first). */
    freeAST(assign);
    return outer;
}

/* Build an assignment `<name> = <value>;` AST (AST_ASSIGN of an AST_VARIABLE). */
static AST *buildSimpleAssign(const char *name, AST *value, int line) {
    Token *nTok = newToken(TOKEN_IDENTIFIER, name, line, 0);
    AST *var = newASTNode(AST_VARIABLE, nTok);
    setTypeAST(var, value ? value->var_type : TYPE_UNKNOWN);
    Token *aTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *assign = newASTNode(AST_ASSIGN, aTok);
    setLeft(assign, var);
    setRight(assign, value);
    setTypeAST(assign, value ? value->var_type : TYPE_UNKNOWN);
    return assign;
}

/* Shared "temp var + new T() + per-field assign + [guard] + return temp" skeleton
 * used by both `ret T { f: v, ... }` (buildReturnObjectInit) and tuple-return
 * lowering (parseTupleReturn). fieldNames/fieldValues are parallel arrays of
 * length fieldCount; fieldValues ownership transfers to the returned AST.
 * `tempName` is the caller-chosen temp variable identifier (must be unique per
 * ret site -- callers use "__aether_retobj_<line>"). `guard`, if non-NULL,
 * ownership transfers and is spliced in immediately before the final return
 * (a @post check must run before control leaves the function). Returns an
 * AST_COMPOUND splice (i_val==1). */
static AST *buildTempRecordReturn(const char *typeName, VarType vtype, AST *typeNode,
                                  const char *tempName,
                                  const char **fieldNames, AST **fieldValues, size_t fieldCount,
                                  AST *guard, int line) {
    Token *newTok = newToken(TOKEN_IDENTIFIER, typeName, line, 0);
    AST *newNode = newASTNode(AST_NEW, newTok);
    setTypeAST(newNode, TYPE_POINTER);

    Token *tmpVarTok = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
    AST *tmpVar = newASTNode(AST_VARIABLE, tmpVarTok);
    setTypeAST(tmpVar, vtype);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, tmpVar);
    setLeft(decl, newNode);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */
    addChild(outer, decl);

    for (size_t i = 0; i < fieldCount; i++) {
        AST *valExpr = fieldValues[i];
        Token *recvTok = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
        AST *recv = newASTNode(AST_VARIABLE, recvTok);
        setTypeAST(recv, vtype);
        Token *fldTok = newToken(TOKEN_IDENTIFIER, fieldNames[i], line, 0);
        AST *fldVar = newASTNode(AST_VARIABLE, fldTok);
        AST *fldAccess = newASTNode(AST_FIELD_ACCESS, fldTok);
        setLeft(fldAccess, recv);
        setRight(fldAccess, fldVar);
        Token *asgnTok = newToken(TOKEN_ASSIGN, "=", line, 0);
        AST *assign = newASTNode(AST_ASSIGN, asgnTok);
        setLeft(assign, fldAccess);
        setRight(assign, valExpr);
        setTypeAST(assign, valExpr ? valExpr->var_type : TYPE_UNKNOWN);
        addChild(outer, assign);
    }

    if (guard) addChild(outer, guard);

    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *ret = newASTNode(AST_RETURN, retTok);
    Token *resTok = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
    AST *resVar = newASTNode(AST_VARIABLE, resTok);
    setTypeAST(resVar, vtype);
    setLeft(ret, resVar);
    setTypeAST(ret, vtype);
    addChild(outer, ret);

    return outer;
}

/* Rewrite `result.N` references in a tuple @post expression to `<tempName>.itemN`
 * field access against the return statement's own per-call-site temp record
 * variable (each `ret (a,b);` site gets its own temp, so this must run per
 * site, not once up front). Returns a newly malloc'd string (caller frees), or
 * NULL on allocation failure. */
static char *aetherRewriteTupleResultRefs(const char *postExpr, const char *tempName) {
    size_t cap = strlen(postExpr) + 64;
    char *rewritten = (char *)malloc(cap);
    if (!rewritten) return NULL;
    size_t w = 0;
    const char *s = postExpr;
    while (*s) {
        if (strncmp(s, "result.", 7) == 0 &&
            (s == postExpr || !(isalnum((unsigned char)s[-1]) || s[-1] == '_' || s[-1] == '.'))) {
            const char *digits = s + 7;
            if (isdigit((unsigned char)*digits)) {
                unsigned long k = strtoul(digits, NULL, 10);
                const char *dEnd = digits;
                while (isdigit((unsigned char)*dEnd)) dEnd++;
                char repl[96];
                int rl = snprintf(repl, sizeof(repl), "%s.item%lu", tempName, k);
                while (w + (size_t)rl + 1 >= cap) {
                    cap *= 2; rewritten = (char *)realloc(rewritten, cap);
                }
                memcpy(rewritten + w, repl, (size_t)rl);
                w += (size_t)rl;
                s = dEnd;
                continue;
            }
        }
        if (w + 2 >= cap) { cap *= 2; rewritten = (char *)realloc(rewritten, cap); }
        rewritten[w++] = *s++;
    }
    rewritten[w] = '\0';
    return rewritten;
}

/* ret (a, b, ...) ;  for a tuple-return function. Lowers to the reentrant
 * record-by-value shape: `__AetherTuple<id> __aether_retobj_<line> = new
 * __AetherTuple<id>(); __aether_retobj_<line>.item<k> = expr<k>; [@post guard;]
 * return __aether_retobj_<line>;` -- the same VM return-by-value path an
 * ordinary record-returning function already uses (see buildReturnObjectInit),
 * so recursion/par-sharing of the same tuple-returning function is structurally
 * reentrant (each call gets its own temp + stack-copied return value) rather
 * than merely rejected. The result is an AST_COMPOUND splice (i_val==1) so
 * parseBlock flattens it into the body. */
static AST *parseTupleReturn(AetherParser *p, int line) {
    const AetherTupleSig *sig = p->currentTupleSig;
    aetherAdvance(p); /* consume '(' */
    AST *items[16];
    size_t idx = 0;
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *item = parseExpr(p);
        if (!item) { p->hadError = true; for (size_t i = 0; i < idx; i++) freeAST(items[i]); return NULL; }
        if (idx >= sig->itemCount || idx >= 16) {
            reportAetherAstError(aetherSemanticGetSourcePath(), line, "tuple",
                    "tuple return has more values than the declared return type.", NULL);
            p->hadError = true;
            freeAST(item);
            for (size_t i = 0; i < idx; i++) freeAST(items[i]);
            return NULL;
        }
        items[idx++] = item;
        if (p->current.type == REA_TOKEN_COMMA) { aetherAdvance(p); continue; }
        break;
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    } else if (!p->hadError) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected ')' to close tuple return.", NULL);
        p->hadError = true;
        for (size_t i = 0; i < idx; i++) freeAST(items[i]);
        return NULL;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (idx != sig->itemCount) {
        reportAetherAstError(aetherSemanticGetSourcePath(), line, "tuple",
                "tuple return arity does not match the declared return type.", NULL);
        p->hadError = true;
        for (size_t i = 0; i < idx; i++) freeAST(items[i]);
        return NULL;
    }

    char typeName[40];
    aetherTupleSyntheticTypeName(typeName, sizeof(typeName), sig->typeId);
    VarType vtype = TYPE_UNKNOWN;
    AST *typeNode = buildTypeNode(typeName, strlen(typeName), line, &vtype);

    char tempName[64];
    snprintf(tempName, sizeof(tempName), "__aether_retobj_%d", line);

    const char *fieldNames[16];
    char fieldNameBufs[16][32];
    for (size_t i = 0; i < idx; i++) {
        snprintf(fieldNameBufs[i], sizeof(fieldNameBufs[i]), "item%zu", i);
        fieldNames[i] = fieldNameBufs[i];
    }

    AST *guard = NULL;
    if (p->currentPostExpr) {
        /* The guard text is re-parsed from scratch (buildContractGuard ->
         * parseExprFromText, a detached sub-parser) with no knowledge of
         * tempName's type, so `tempName.itemN` would not resolve as field
         * access without this: register the temp's synthesized record type in
         * the shared binding table first, exactly as parseLetTupleDestructure
         * does for user-facing destructured names. */
        bindingTableSet(p->bindings, tempName, typeName);
        char *rewritten = aetherRewriteTupleResultRefs(p->currentPostExpr, tempName);
        if (!rewritten) { for (size_t i = 0; i < idx; i++) freeAST(items[i]); return NULL; }
        guard = buildContractGuard(p, rewritten, "post", p->currentFunctionName, line);
        free(rewritten);
        if (!guard) { for (size_t i = 0; i < idx; i++) freeAST(items[i]); return NULL; }
    }

    return buildTempRecordReturn(typeName, vtype, typeNode, tempName,
                                 fieldNames, items, idx, guard, line);
}

/* let (a, b, ...) = call();  tuple destructuring.
 *
 * Lowers to the same shape the rewriter (translateTupleDestructureLetLine)
 * emits: call the tuple-return function as a statement, then read each slot
 * global into a typed local:
 *     call();
 *     <type0> a = __aether_tuple_N_item0;
 *     <type1> b = __aether_tuple_N_item1;
 * Requires a direct call to a known tuple-return function (matching the
 * rewriter). Returns an AST_COMPOUND splice (i_val==1). Called with `current`
 * positioned at the '(' that opens the destructuring pattern. */
static AST *parseLetTupleDestructure(AetherParser *p, int kwLine) {
    aetherAdvance(p); /* consume '(' */
    /* Collect the binding names. */
    char *names[16];
    size_t nameCount = 0;
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        if (!aetherTokenIsIdentifierLike(&p->current) || nameCount >= 16) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "tuple",
                    "expected a binding name in tuple destructuring.", NULL);
            p->hadError = true;
            for (size_t i = 0; i < nameCount; i++) free(names[i]);
            return NULL;
        }
        char *nm = (char *)malloc(p->current.length + 1);
        if (!nm) { p->hadError = true; for (size_t i = 0; i < nameCount; i++) free(names[i]); return NULL; }
        memcpy(nm, p->current.start, p->current.length);
        nm[p->current.length] = '\0';
        names[nameCount++] = nm;
        aetherAdvance(p); /* consume name */
        if (p->current.type == REA_TOKEN_COMMA) { aetherAdvance(p); continue; }
        break;
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    } else if (!p->hadError) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected ')' to close tuple destructuring pattern.", NULL);
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    /* Optional type annotation: `let (a, b): (T0, T1) = ...`. The rewriter accepts
     * and ignores it (the slot globals below carry the real per-item types); skip
     * from ':' to the '=' so the assignment check still fires. */
    if (p->current.type == REA_TOKEN_COLON) {
        aetherAdvance(p); /* consume ':' */
        while (p->current.type != REA_TOKEN_EQUAL && p->current.type != REA_TOKEN_EOF &&
               p->current.type != REA_TOKEN_SEMICOLON) {
            aetherAdvance(p);
        }
    }
    if (p->current.type != REA_TOKEN_EQUAL) {
        reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "tuple",
                "expected '=' in tuple destructuring.", NULL);
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    aetherAdvance(p); /* consume '=' */

    /* The right side must be a direct call `name(args)` to a tuple-return fn. */
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "tuple",
                             "tuple destructuring currently requires a direct call to a known tuple-return function.",
                             "use `let tmp = fnCall();` and then read fields, or destructure a direct tuple-return call.");
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    char calleeName[128];
    size_t cl = p->current.length < sizeof(calleeName) - 1 ? p->current.length : sizeof(calleeName) - 1;
    memcpy(calleeName, p->current.start, cl);
    calleeName[cl] = '\0';
    const AetherTupleSig *sig = tupleTableGet(p->tuples, calleeName, strlen(calleeName));
    if (!sig) {
        reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "tuple",
                             "tuple destructuring target is not a known tuple-return function.",
                             "the callee must be a top-level tuple-return function defined in this module; otherwise return a record/object and read its fields.");
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    if (sig->itemCount != nameCount) {
        reportAetherAstError(aetherSemanticGetSourcePath(), kwLine, "tuple",
                             "tuple destructuring arity does not match the function return tuple.",
                             "make the number of bindings match the number of returned tuple elements.");
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    /* Parse the call as a full expression (handles args), yielding a
     * PROCEDURE_CALL we use as a statement. */
    AST *call = parseExpr(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (!call) {
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }

    char typeName[40];
    aetherTupleSyntheticTypeName(typeName, sizeof(typeName), sig->typeId);
    VarType tmpVtype = TYPE_UNKNOWN;
    AST *tmpTypeNode = buildTypeNode(typeName, strlen(typeName), kwLine, &tmpVtype);

    char tempName[64];
    snprintf(tempName, sizeof(tempName), "__aether_tupdest_%d", kwLine);

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */

    /* <SynthType> __aether_tupdest_<kwLine> = <call>; -- captures the callee's
     * return-by-value record once. The VM deep-copies a record on return (see
     * returnFromCall/copyRecord in pscal-core), so this temp is an independent
     * snapshot: recursion or a concurrent `par` branch calling the same
     * tuple-returning function cannot alias it. */
    Token *tmpVarTok = newToken(TOKEN_IDENTIFIER, tempName, kwLine, 0);
    AST *tmpVar = newASTNode(AST_VARIABLE, tmpVarTok);
    setTypeAST(tmpVar, tmpVtype);
    AST *tmpDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(tmpDecl, tmpVar);
    setLeft(tmpDecl, call);
    setRight(tmpDecl, tmpTypeNode);
    setTypeAST(tmpDecl, tmpVtype);
    addChild(outer, tmpDecl);

    /* One typed local per binding, reading the matching field off the temp
     * record (ordinary field access, same AST shape as buildTempRecordReturn),
     * and record the binding's Aether type for downstream inference. */
    for (size_t i = 0; i < nameCount; i++) {
        char fieldName[32];
        snprintf(fieldName, sizeof(fieldName), "item%zu", i);
        VarType vt = TYPE_UNKNOWN;
        AST *typeNode = buildTypeNode(sig->itemTypes[i], strlen(sig->itemTypes[i]), kwLine, &vt);

        Token *recvTok = newToken(TOKEN_IDENTIFIER, tempName, kwLine, 0);
        AST *recv = newASTNode(AST_VARIABLE, recvTok);
        setTypeAST(recv, tmpVtype);
        Token *fldTok = newToken(TOKEN_IDENTIFIER, fieldName, kwLine, 0);
        AST *fldVar = newASTNode(AST_VARIABLE, fldTok);
        AST *fldAccess = newASTNode(AST_FIELD_ACCESS, fldTok);
        setLeft(fldAccess, recv);
        setRight(fldAccess, fldVar);
        setTypeAST(fldAccess, vt);

        Token *nameTok = newToken(TOKEN_IDENTIFIER, names[i], kwLine, 0);
        AST *var = newASTNode(AST_VARIABLE, nameTok);
        setTypeAST(var, vt);
        AST *decl = newASTNode(AST_VAR_DECL, NULL);
        addChild(decl, var);
        setLeft(decl, fldAccess);
        setRight(decl, typeNode);
        setTypeAST(decl, vt);
        addChild(outer, decl);
        bindingTableSet(p->bindings, names[i], sig->itemTypes[i]);
        free(names[i]);
    }
    return outer;
}

/* `ret T { f: v, ... } ;` -> the temp-object pattern the rewriter emits
 * (translate.c translateReturnObjectInitLine):
 *     T __aether_retobj_<line> = new T();
 *     __aether_retobj_<line>.f = v;   (one per field)
 *     return __aether_retobj_<line>;
 * Returns an AST_COMPOUND splice (i_val==1) so parseBlock flattens it. The
 * current token is the type-name identifier (verified by the caller to be
 * followed by '{'). `line` is the source line of the `ret`, used for the temp
 * name (so it matches the rewriter byte-for-byte). */
static AST *buildReturnObjectInit(AetherParser *p, int line) {
    Token *clsTok = copyNameToken(p);
    if (!clsTok) return NULL;
    /* Resolve the class type so the temp var-decl carries the same pointer type
     * node the rewriter's `T x = new T();` lowering produces. */
    VarType vtype = TYPE_UNKNOWN;
    AST *typeNode = buildTypeNode(clsTok->value, strlen(clsTok->value), line, &vtype);
    aetherAdvance(p); /* consume type name */

    /* Parse the `{ f: v, ... }` field list (same as the let/new object literal). */
    AST *inits = parseRecordInitBlock(p);

    const char *fieldNames[64];
    AST *fieldValues[64];
    size_t fieldCount = 0;
    if (inits) {
        for (int i = 0; i < inits->child_count && fieldCount < 64; i++) {
            AST *fa = inits->children[i];
            if (!fa || fa->type != AST_ASSIGN) continue;
            AST *fieldVar = fa->left; /* AST_VARIABLE(field) */
            if (!fieldVar || !fieldVar->token) continue;
            fieldNames[fieldCount] = fieldVar->token->value;
            fieldValues[fieldCount] = fa->right;
            fa->right = NULL; /* moved */
            fieldCount++;
        }
    }

    char tempName[64];
    snprintf(tempName, sizeof(tempName), "__aether_retobj_%d", line);
    AST *outer = buildTempRecordReturn(clsTok->value, vtype, typeNode, tempName,
                                       fieldNames, fieldValues, fieldCount, NULL, line);

    if (inits) freeAST(inits);
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    freeToken(clsTok);
    return outer;
}

/* ret [expr] ;  ->  AST_RETURN (mirrors rea parseReturn).
 *
 * Three contract/tuple-aware shapes (MILESTONE 3), matching translate.c:
 *   - tuple-return fn: `ret (a,b);`  -> per-slot writes + [post] + `return;`
 *   - @post on a value fn: `ret e;`  -> `result = e; <post guard>; return result;`
 *   - otherwise: a plain AST_RETURN. */
static AST *parseRet(AetherParser *p) {
    int line = p->current.line;

    /* Tuple-return function: `ret (a, b);`. */
    if (p->currentTupleSig && p->currentTupleSig->itemCount > 0) {
        aetherAdvance(p); /* consume 'ret' */
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            return parseTupleReturn(p, line);
        }
        reportAetherAstError(aetherSemanticGetSourcePath(), line, "tuple",
                "a tuple-return function must return a tuple literal `(...)`.", NULL);
        p->hadError = true;
        return NULL;
    }

    aetherAdvance(p); /* consume 'ret' */

    /* Return object-init: `ret T { f: v, ... };`. The rewriter lowers this to a
     * temp-object pattern (translateReturnObjectInitLine). Detect `IDENT {` -- the
     * only place `{` follows an identifier in expression position is an object
     * literal -- and build the temp/return splice. Not applied when a @post guard
     * is active (the rewriter stages those differently); fall through then. */
    if (p->current.type == REA_TOKEN_IDENTIFIER && !p->currentPostExpr) {
        ReaToken save = p->current;
        int savedHead = p->queueHead, savedCount = p->queueCount;
        ReaToken q0 = p->queue[0], q1 = p->queue[1], q2 = p->queue[2];
        ReaLexer savedLexer = p->lexer;
        aetherAdvance(p); /* tentatively consume the identifier */
        bool isObjInit = (p->current.type == REA_TOKEN_LEFT_BRACE);
        /* restore to the identifier */
        p->lexer = savedLexer;
        p->queueHead = savedHead; p->queueCount = savedCount;
        p->queue[0] = q0; p->queue[1] = q1; p->queue[2] = q2;
        p->current = save;
        if (isObjInit) {
            return buildReturnObjectInit(p, line);
        }
    }

    AST *value = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON && p->current.type != REA_TOKEN_RIGHT_BRACE &&
        p->current.type != REA_TOKEN_EOF) {
        value = parseExpr(p);
    } else if (p->currentFunctionType != TYPE_VOID) {
        /* Empty `ret;` in a non-Void function. Route through the coded path so the
         * diagnostic carries a code (FLOW-002) + guide pointer, matching its
         * FLOW-001 fallthrough sibling instead of a raw uncoded stderr line. */
        reportAetherAstError(aetherSemanticGetSourcePath(), line, "function",
                             "return requires a value.",
                             "supply a value: `ret <expr>;` (a non-Void function must "
                             "return a value), or declare the function `-> Void`.");
        p->hadError = true;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }
    if (p->hadError) return NULL;

    /* @post on a value-returning function: stage `result`, check, then return it,
     * exactly as the rewriter's translateReturnWithPost. */
    if (p->currentPostExpr && value) {
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        outer->i_val = 1; /* splice into the surrounding block */
        addChild(outer, buildSimpleAssign("result", value, line));
        AST *guard = buildContractGuard(p, p->currentPostExpr, "post",
                                        p->currentFunctionName, line);
        if (!guard) { freeAST(outer); return NULL; }
        addChild(outer, guard);
        Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
        AST *ret = newASTNode(AST_RETURN, retTok);
        Token *resTok = newToken(TOKEN_IDENTIFIER, "result", line, 0);
        AST *resVar = newASTNode(AST_VARIABLE, resTok);
        setTypeAST(resVar, value->var_type);
        setLeft(ret, resVar);
        setTypeAST(ret, value->var_type);
        addChild(outer, ret);
        return outer;
    }

    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *node = newASTNode(AST_RETURN, retTok);
    setLeft(node, value);
    setTypeAST(node, value ? value->var_type : TYPE_VOID);
    return node;
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


/* Inject the loop post-step before every `continue` in a range-loop body, exactly
 * as rea's parseFor does (rewriteContinueWithPost). `loop i in a..b` lowers to a
 * while whose post-increment is the last body statement, so a bare `continue`
 * would jump straight to the condition, skip the increment, and spin forever.
 * Rewrite each `continue` to `{ post; continue; }`. Recurses through the whole
 * subtree, matching rea so output stays byte-for-byte identical to the rewriter. */
static AST *aetherRewriteContinueWithPost(AST *node, AST *postStmt) {
    if (!node) return NULL;
    if (node->type == AST_CONTINUE) {
        AST *comp = newASTNode(AST_COMPOUND, NULL);
        addChild(comp, copyAST(postStmt));
        addChild(comp, newASTNode(AST_CONTINUE, NULL));
        return comp;
    }
    node->left  = aetherRewriteContinueWithPost(node->left, postStmt);
    node->right = aetherRewriteContinueWithPost(node->right, postStmt);
    node->extra = aetherRewriteContinueWithPost(node->extra, postStmt);
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i])
            node->children[i] = aetherRewriteContinueWithPost(node->children[i], postStmt);
    }
    return node;
}

/* Parse a range bound expression up to (but not consuming) the AE_TOKEN_DOTDOT
 * or the body-opening '{'. Bounds may be arbitrary expressions (numbers,
 * identifiers, calls, arithmetic), so reuse the full expression parser, stopping
 * the precedence ladder at the range/brace boundary. The expression parser
 * naturally stops at '{' and AE_TOKEN_DOTDOT (neither is an operator it
 * recognizes), so a plain parseExpr() call suffices. */

/* NAME in LOW..HIGH { body }  (the loop/for keyword is consumed by the caller)
 *
 * The rewriter lowers both `loop i in a..b` and `for i in a..b` to a C-style
 * half-open for loop
 *     for (int i = LOW; i < HIGH; i = i + 1) { body }
 * which rea's parseFor turns into:
 *     COMPOUND[ init-var-decl,
 *               WHILE(cond: i < HIGH,
 *                     body: COMPOUND[ body-block, post-expr-stmt ]) ]
 * We reproduce that exact structure so output matches byte-for-byte.
 *
 * The range operator is now a real AE_TOKEN_DOTDOT token (the aetherAdvance()
 * tokenizer reconstructs it despite the shared Rea lexer folding the dots), so
 * both bounds are parsed straight from the token stream as expressions -- no
 * raw-source-span workaround. Handles numeric and identifier bounds uniformly. */
static AST *parseLoopRange(AetherParser *p) {
    if (!aetherTokenIsIdentifierLike(&p->current)) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected loop variable name.", NULL);
        p->hadError = true;
        return NULL;
    }
    /* Loop variable name; we reuse it several times so capture the lexeme. */
    size_t nlen = (size_t)p->current.length;
    char *nameBuf = (char *)malloc(nlen + 1);
    if (!nameBuf) return NULL;
    memcpy(nameBuf, p->current.start, nlen);
    nameBuf[nlen] = '\0';
    int idLine = p->current.line;
    aetherAdvance(p); /* consume loop var */

    if (!isAetherKeyword(&p->current, "in")) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected 'in' in loop range.", NULL);
        p->hadError = true;
        free(nameBuf);
        return NULL;
    }
    aetherAdvance(p); /* consume 'in' */

    /* Bounds are parsed at additive precedence (parseAdd), not the full
     * parseExpr ladder: parseExpr would happily keep consuming into
     * comparison/equality/logical operators, so `loop i in 0..5 && cond`
     * silently parsed as `loop i in 0..(5 && cond)` -- a Bool upper bound
     * that coerced to 0/1, turning the intended range into a single
     * iteration with no error. parseAdd still covers every legitimate bound
     * (literals, identifiers, calls, indexing, +/-/ * // /div/mod, unary,
     * parens, if-expressions), just not the operators that indicate the
     * author meant something other than a number. */
    AST *low = parseAdd(p);
    if (!low || p->current.type != AE_TOKEN_DOTDOT) {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "expected '<low>..<high>' in loop range.", NULL);
        p->hadError = true;
        if (low) freeAST(low);
        free(nameBuf);
        return NULL;
    }
    if (low->var_type == TYPE_BOOLEAN) {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "loop range bound must be numeric (Int/Real), not Bool -- "
                "did you mean a separate condition, e.g. `loop cond { ... }` "
                "or `if placed { break; }` inside the loop body?", NULL);
        p->hadError = true;
        freeAST(low);
        free(nameBuf);
        return NULL;
    }
    aetherAdvance(p); /* consume '..' */
    AST *high = parseAdd(p);
    if (!high) {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "could not parse loop range bounds.", NULL);
        p->hadError = true;
        freeAST(low);
        free(nameBuf);
        return NULL;
    }
    if (high->var_type == TYPE_BOOLEAN) {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "loop range bound must be numeric (Int/Real), not Bool -- "
                "did you mean a separate condition, e.g. `loop cond { ... }` "
                "or `if placed { break; }` inside the loop body?", NULL);
        p->hadError = true;
        freeAST(low);
        freeAST(high);
        free(nameBuf);
        return NULL;
    }

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "expected '{' to open loop body.", NULL);
        p->hadError = true;
        freeAST(low);
        freeAST(high);
        free(nameBuf);
        return NULL;
    }

    /* Build: int i = LOW;  (AST_VAR_DECL, matching rea parseFor init). */
    Token *initNameTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *initVar = newASTNode(AST_VARIABLE, initNameTok);
    setTypeAST(initVar, TYPE_INT64);
    Token *intTypeTok = newToken(TOKEN_IDENTIFIER, "int", idLine, 0);
    AST *intTypeNode = newASTNode(AST_TYPE_IDENTIFIER, intTypeTok);
    setTypeAST(intTypeNode, TYPE_INT64);
    AST *initDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(initDecl, initVar);
    setLeft(initDecl, low);
    setRight(initDecl, intTypeNode);
    setTypeAST(initDecl, TYPE_INT64);

    /* Condition: i < HIGH  (AST_BINARY_OP, BOOLEAN). */
    Token *condVarTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *condVar = newASTNode(AST_VARIABLE, condVarTok);
    setTypeAST(condVar, TYPE_UNKNOWN);
    Token *ltTok = newToken(TOKEN_LESS, "<", idLine, 0);
    AST *cond = newASTNode(AST_BINARY_OP, ltTok);
    setLeft(cond, condVar);
    setRight(cond, high);
    setTypeAST(cond, TYPE_BOOLEAN);

    /* Post: i = i + 1  (AST_ASSIGN of an AST_BINARY_OP). */
    Token *postLhsTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *postLhs = newASTNode(AST_VARIABLE, postLhsTok);
    setTypeAST(postLhs, TYPE_UNKNOWN);
    Token *addLhsTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *addLhs = newASTNode(AST_VARIABLE, addLhsTok);
    setTypeAST(addLhs, TYPE_UNKNOWN);
    Token *oneTok = newToken(TOKEN_INTEGER_CONST, "1", idLine, 0);
    AST *oneNode = newASTNode(AST_NUMBER, oneTok);
    setTypeAST(oneNode, TYPE_INT64);
    Token *plusTok = newToken(TOKEN_PLUS, "+", idLine, 0);
    AST *addExpr = newASTNode(AST_BINARY_OP, plusTok);
    setLeft(addExpr, addLhs);
    setRight(addExpr, oneNode);
    setTypeAST(addExpr, promoteIntegralBinaryType(TYPE_UNKNOWN, TYPE_INT64));
    Token *assignTok = newToken(TOKEN_ASSIGN, "=", idLine, 0);
    AST *postAssign = newASTNode(AST_ASSIGN, assignTok);
    setLeft(postAssign, postLhs);
    setRight(postAssign, addExpr);
    setTypeAST(postAssign, TYPE_UNKNOWN);
    AST *postStmt = newASTNode(AST_EXPR_STMT, postAssign->token);
    setLeft(postStmt, postAssign);

    /* Rewrite `continue` in the body to run postStmt first (rea parseFor does this
     * via rewriteContinueWithPost) -- otherwise `continue` jumps to the condition,
     * skips the increment, and the loop spins forever. */
    body = aetherRewriteContinueWithPost(body, postStmt);

    /* while body = COMPOUND[ body, postStmt ]  (rea parseFor with post). */
    AST *whileBody = newASTNode(AST_COMPOUND, NULL);
    addChild(whileBody, body);
    addChild(whileBody, postStmt);
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);

    /* outer = COMPOUND[ initDecl, whileNode ]. */
    AST *outer = newASTNode(AST_COMPOUND, NULL);
    addChild(outer, initDecl);
    addChild(outer, whileNode);

    free(nameBuf);
    return outer;
}

/* Build a while loop from an already-parsed condition + body, mirroring rea
 * parseWhile: AST_WHILE(left=condition, right=body). */
static AST *buildWhile(AST *cond, AST *body) {
    AST *node = newASTNode(AST_WHILE, NULL);
    setLeft(node, cond);
    setRight(node, body);
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

/* `par { call1(); call2(); ... }` -> the spawn/join block the rewriter emits
 * (translate.c translateParallelCallLine): a nested `{ }` scope holding one
 * `int __aether_par_<N> = spawn callN();` per body call followed by a
 * `join __aether_par_<N>;` for each, in order. The handle counter restarts at 1
 * per par block (clearParBlockState resets nextHandle). Returns an AST_COMPOUND
 * (a block statement). */
static AST *parseParBlock(AetherParser *p) {
    aetherAdvance(p); /* consume 'par' */
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '{' to open par block.", NULL);
        p->hadError = true;
        return NULL;
    }
    aetherAdvance(p); /* consume '{' */

    AST *block = newASTNode(AST_COMPOUND, NULL);
    AST *joins = newASTNode(AST_COMPOUND, NULL); /* staged join statements */
    int handle = 0;
    /* Data-race guard: track which record-typed argument variable was passed to
     * which par branch. The same pointer-backed record handed to two branches is
     * written concurrently by the spawned threads -> a heap double-free
     * (SIGABRT/SIGTRAP), silent today (see PAR-001 below). Pointers into the arg
     * tokens (owned by the call nodes, which outlive this loop); no allocation. */
    struct { const char *name; int branch; } sharedRecs[64];
    int sharedRecCount = 0;
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF &&
           !p->hadError) {
        int callLine = p->current.line;
        AST *stmt = parseStatement(p);
        if (!stmt) break;
        /* Extract the call expression from the parsed statement (a call statement is
         * AST_EXPR_STMT wrapping the call). */
        AST *call = NULL;
        if (stmt->type == AST_EXPR_STMT && stmt->left) {
            call = stmt->left;
            stmt->left = NULL;
            freeAST(stmt);
        } else {
            call = stmt; /* tolerate a bare call node */
        }
        /* Only direct call statements may appear in a par block. A non-call body
         * statement (e.g. `x = 5;` -> AST_ASSIGN, or an `fx { }` block) would
         * otherwise be wrapped in a spawn node and surface later as codegen's terse
         * "spawn expects procedure call" -- emitted twice, once with a bogus L0.
         * Reject it here with the legacy rewriter's exact diagnostic + repair hint
         * (translate.c: "only direct call statements are allowed inside par blocks.")
         * so the AST path's error UX matches byte for byte: a single diagnostic on
         * the correct line. */
        if (!call || call->type != AST_PROCEDURE_CALL) {
            reportAetherAstError(aetherSemanticGetSourcePath(), callLine, "par",
                                 "only direct call statements are allowed inside par blocks.",
                                 "move side effects into direct calls inside `par { ... }`.");
            p->hadError = true;
            if (call) freeAST(call);
            freeAST(block);
            freeAST(joins);
            return NULL;
        }
        handle++;
        char handleName[64];
        snprintf(handleName, sizeof(handleName), "__aether_par_%d", handle);

        /* PAR-001: reject a record shared across par branches before it becomes a
         * concurrent double-free at runtime. Scan this call's argument variables
         * (children[0] is the receiver for a method call); a bare identifier whose
         * declared type is a user aggregate (record/array -- pointer-backed, so it
         * aliases across threads) is flagged if a *different* branch already passed
         * the same name. Scalars (Int/Real/Text/Bool/...) are value-copied per
         * thread, so they are never flagged. */
        for (int ci = 0; ci < call->child_count; ci++) {
            AST *arg = call->children[ci];
            if (!arg || arg->type != AST_VARIABLE || !arg->token || !arg->token->value) continue;
            const char *an = arg->token->value;
            const char *aty = bindingTableGet(p->bindings, an, strlen(an));
            if (!aty) continue;
            VarType avt = TYPE_UNKNOWN; const char *arn = NULL;
            if (mapAetherType(aty, strlen(aty), &arn, &avt)) continue; /* scalar -> safe */
            int prior = -1;
            for (int si = 0; si < sharedRecCount; si++) {
                if (sharedRecs[si].name && strcmp(sharedRecs[si].name, an) == 0) {
                    prior = sharedRecs[si].branch;
                    break;
                }
            }
            if (prior >= 0 && prior != handle) {
                const char *ppath = aetherSemanticGetSourcePath();
                if (ppath && *ppath) aetherDiagf("%s:%d: ", ppath, callLine);
                aetherDiagf("[PAR-001] Aether par error: record '%s' is shared by more than one par "
                            "branch; the spawned threads write it concurrently, which races (heap "
                            "corruption).\n", an);
                aetherDiagf("hint: give each par branch its own record, then combine the results "
                            "after the `par { ... }` block.\n");
                aetherReportGuideHelp("PAR-001");
                p->hadError = true;
                freeAST(call);
                freeAST(block);
                freeAST(joins);
                return NULL;
            }
            if (prior < 0 && sharedRecCount < (int)(sizeof(sharedRecs) / sizeof(sharedRecs[0]))) {
                sharedRecs[sharedRecCount].name = an;
                sharedRecs[sharedRecCount].branch = handle;
                sharedRecCount++;
            }
        }

        /* PAR-003 (tuple-return function shared across par branches) used to be
         * rejected here: tuple returns lowered to shared per-function globals,
         * so two branches calling the same tuple-returning function raced on
         * them. Now that tuple returns lower to a record returned by value
         * (each call gets its own VM-deep-copied result -- see
         * returnFromCall/copyRecord in pscal-core, and parseTupleReturn /
         * parseLetTupleDestructure), that defect class is structurally
         * impossible: nothing is shared, so there is nothing left to detect. */

        /* int __aether_par_N = spawn <call>; */
        Token *hTok = newToken(TOKEN_IDENTIFIER, handleName, callLine, 0);
        AST *hVar = newASTNode(AST_VARIABLE, hTok);
        setTypeAST(hVar, TYPE_INT64);
        Token *intTok = newToken(TOKEN_IDENTIFIER, "int", callLine, 0);
        AST *intType = newASTNode(AST_TYPE_IDENTIFIER, intTok);
        setTypeAST(intType, TYPE_INT64);
        AST *spawnNode = newThreadSpawn(call);
        setTypeAST(spawnNode, TYPE_INT64);
        AST *decl = newASTNode(AST_VAR_DECL, NULL);
        addChild(decl, hVar);
        setLeft(decl, spawnNode);
        setRight(decl, intType);
        setTypeAST(decl, TYPE_INT64);
        addChild(block, decl);

        /* Stage  join __aether_par_N;  for after all spawns. */
        Token *jTok = newToken(TOKEN_IDENTIFIER, handleName, callLine, 0);
        AST *jVar = newASTNode(AST_VARIABLE, jTok);
        setTypeAST(jVar, TYPE_INT64);
        AST *joinNode = newThreadJoin(jVar);
        addChild(joins, joinNode);

        while (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        aetherAdvance(p);
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                "expected '}' to close par block.", NULL);
        p->hadError = true;
    }
    /* Append the staged joins after the spawns, in order. */
    for (int i = 0; i < joins->child_count; i++) {
        addChild(block, joins->children[i]);
        joins->children[i] = NULL;
    }
    joins->child_count = 0;
    freeAST(joins);
    return block;
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
    /* Expression statement or assignment. */
    AST *expr = parseExpr(p);
    if (!expr) return NULL;
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }
    if (expr->type == AST_ASSIGN) {
        /* Array append: `target = src + [item]` (single-element literal). Lower
         * it to the rewriter's setlength + indexed-assign expansion, whether
         * `src` is `target` itself (the original self-reassignment idiom,
         * `xs = xs + [v];`) or a different array (`target = src + [v];` /
         * `let target: T[] = src + [v];` below) -- the latter previously fell
         * through to a raw `ARRAY + ARRAY_LITERAL` binary op the VM has no
         * operator for ("Operands must be numbers for arithmetic operation
         * '+'... Got ARRAY and ARRAY"). `target` must still be an assignable
         * lvalue chain; `src` can be any expression of the right array type. */
        AST *target = expr->left;
        AST *rhs = expr->right;
        if (target && rhs && rhs->type == AST_BINARY_OP && rhs->token &&
            rhs->token->type == TOKEN_PLUS &&
            rhs->right && rhs->right->type == AST_ARRAY_LITERAL &&
            rhs->right->child_count == 1 &&
            aetherIsLValueChain(target)) {
            int line = expr->token ? expr->token->line : p->current.line;
            /* Move the single element out of the literal so it survives the free. */
            AST *item = rhs->right->children[0];
            rhs->right->children[0] = NULL;
            rhs->right->child_count = 0;
            if (item) item->parent = NULL;
            return buildArrayAppend(expr, target, item, line, rhs->left);
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
static AST *parseStatement(AetherParser *p) {
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
    if (stmt) addChild(wrapper, stmt);
    return wrapper;
}

static AST *parseBlock(AetherParser *p) {
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
/* True if the subtree contains a value-bearing `ret <expr>` (AST_RETURN with a
 * non-NULL value in ->left). Does not descend into nested function/procedure
 * declarations, whose returns belong to them. Mirrors the rewriter's
 * "sawValueReturn" used for the FLOW-001 fallthrough check. */
static bool astHasValueReturn(const AST *node) {
    if (!node) return false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) return false;
    if (node->type == AST_RETURN && node->left) return true;
    if (astHasValueReturn(node->left)) return true;
    if (astHasValueReturn(node->right)) return true;
    if (astHasValueReturn(node->extra)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (astHasValueReturn(node->children[i])) return true;
    }
    return false;
}

/* True if the block has a top-level statement that "falls through": an
 * expression/assignment/call, not a declaration, control-flow construct, or
 * return. Mirrors the rewriter's sawFallthroughTopLevelStmt (translate.c ~8966,
 * which excludes if/else/loop/for/while/let/const/fn), so an all-declarations
 * body (e.g. a malformed unclosed `fn`) is not mistaken for a fallthrough. */
static bool astBlockHasFallthroughStmt(const AST *block) {
    if (!block) return false;
    for (int i = 0; i < block->child_count; i++) {
        const AST *c = block->children[i];
        if (!c) continue;
        switch (c->type) {
            case AST_VAR_DECL:
            case AST_CONST_DECL:
            case AST_FUNCTION_DECL:
            case AST_PROCEDURE_DECL:
            case AST_IF:
            case AST_WHILE:
            case AST_RETURN:
                break; /* declaration / control-flow / return: not a fallthrough */
            default:
                return true;
        }
    }
    return false;
}

static AST *parseFnDecl(AetherParser *p) {
    int fnLine = p->current.line; /* line of the `fn` keyword, for diagnostics */
    aetherAdvance(p); /* consume 'fn' */

    if (!aetherTokenIsIdentifierLike(&p->current)) {
        /* A reserved word/operator where the name should be (e.g. `fn mul()`,
         * `fn new()`): name the collision instead of the bare parse error. */
        if (!reportReservedMemberName(&p->current,
                                      p->currentClassName ? "method" : "function")) {
            reportAetherAstError(aetherSemanticGetSourcePath(), p->current.line, "parser",
                    "expected function name after 'fn'.", NULL);
        }
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
        aetherAstRegisterFunctionPurity(nameTok->value, fnIsPure);
        if (isMethod) {
            const char *dot = strrchr(nameTok->value, '.');
            if (dot && dot[1]) {
                aetherAstRegisterFunctionPurity(dot + 1, fnIsPure);
            }
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
        astBlockHasFallthroughStmt(block) && !astHasValueReturn(block)) {
        reportAetherAstError(aetherSemanticGetSourcePath(), fnLine, "function",
                             "non-Void functions have a fallthrough path with no return value.",
                             "add `ret value;` on the top-level path that can reach the closing `}`, or declare the function `-> Void` if it only performs side effects.");
        p->hadError = true;
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
 * so later references resolve -- the byte-for-byte contract for `const X = ...;
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
    aetherAdvance(p); /* consume '=' */
    AST *value = parseExpr(p);
    if (!value) {
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
            } else if (p->current.type == REA_TOKEN_IDENTIFIER) {
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
                    AST *defExpr = parseExpr(p);
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
                                "only constant field defaults are supported; set computed values at "
                                "construction via `new T { field: value }`.",
                                "use a literal or constant expression (e.g. `= 0`, `= \"\"`, `= true`), "
                                "or drop the default and set the value in `new T { ... }`.");
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
 * exact rea parseRea top-level handling. */
static void appendTopLevelDecl(AST *decls, AST *stmts, AST *node) {
    if (!node) return;
    if (node->type == AST_COMPOUND && node->is_global_scope) {
        for (int i = 0; i < node->child_count; i++) {
            AST *child = node->children[i];
            if (!child) continue;
            if (child->type == AST_VAR_DECL || child->type == AST_FUNCTION_DECL ||
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
    if (node->type == AST_VAR_DECL || node->type == AST_FUNCTION_DECL ||
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
        AST *typeNode = buildTypeNode(itemTypes[i], strlen(itemTypes[i]), line, &vt);
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
            const char *paren = c;
            while (paren < lineEnd && *paren != '(') paren++;
            const char *arrow = NULL;
            for (const char *q = paren; q + 1 < lineEnd; q++) {
                if (q[0] == '-' && q[1] == '>') { arrow = q + 2; break; }
            }
            if (nameEnd > nameStart && arrow) {
                const char *rt = arrow;
                while (rt < lineEnd && isspace((unsigned char)*rt)) rt++;
                if (rt < lineEnd && *rt == '(') {
                    /* Capture the `(...)` return-type span. */
                    const char *rtEnd = rt;
                    int depth = 0;
                    while (rtEnd < lineEnd) {
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

/* True if `node`'s subtree declares a function/procedure whose (mangled) name
 * matches `mangled`, case-insensitively (mirroring the compiler's lookup, which
 * lowercases). Used to decide whether a `Type.method` call resolves to a real
 * method. */
static bool aetherProcDefined(AST *node, const char *mangled) {
    if (!node || !mangled) return false;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) &&
        node->token && node->token->value &&
        strcasecmp(node->token->value, mangled) == 0) {
        return true;
    }
    AST *c0 = node->child_count > 0 ? node->children[0] : NULL;
    if (node->left && node->left != c0 && aetherProcDefined(node->left, mangled)) return true;
    if (node->right && aetherProcDefined(node->right, mangled)) return true;
    if (node->extra && aetherProcDefined(node->extra, mangled)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (aetherProcDefined(node->children[i], mangled)) return true;
    }
    return false;
}

/* True if the AST_RECORD_TYPE `recordAst` declares a field named `field`
 * (case-insensitive). Only immediate fields -- callers that reach here have already
 * excluded types with a parent, so inherited fields cannot matter. */
static bool aetherRecordHasField(AST *recordAst, const char *field) {
    if (!recordAst || !field) return false;
    for (int i = 0; i < recordAst->child_count; i++) {
        AST *decl = recordAst->children[i];
        if (!decl || decl->type != AST_VAR_DECL) continue;
        for (int j = 0; j < decl->child_count; j++) {
            AST *v = decl->children[j];
            if (v && v->token && v->token->value &&
                strcasecmp(v->token->value, field) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Walk the whole program; for every `Type.member(...)` call where the parser
 * mangled a record-receiver method, verify the method exists. Emits a compile-time
 * [SCOPE-001] for each undefined method and returns the count. Conservative by
 * construction so it never rejects a valid benchmark-corpus program:
 *   - the pre-dot segment must name a real user record type (lookupType), never a
 *     module/unit qualifier or a builtin;
 *   - the record must have NO parent type -- inheritance can define the method on
 *     an ancestor under a different mangling, so skip to stay safe;
 *   - a matching field is left alone (not our concern here);
 *   - only single-dot names (the `Type.method` mangling) are considered. */
static int aetherCheckMemberCalls(AST *node, AST *decls) {
    if (!node) return 0;
    int errs = 0;
    if (node->type == AST_PROCEDURE_CALL && node->token && node->token->value) {
        const char *name = node->token->value;
        const char *dot = strchr(name, '.');
        if (dot && dot != name && dot[1] != '\0' && strchr(dot + 1, '.') == NULL) {
            size_t tlen = (size_t)(dot - name);
            char typeName[128];
            if (tlen > 0 && tlen < sizeof(typeName)) {
                memcpy(typeName, name, tlen);
                typeName[tlen] = '\0';
                const char *member = dot + 1;
                AST *rec = lookupType(typeName);
                /* Resolved if EITHER a true method `Type.member` exists, OR a plain
                 * top-level `fn member(self: Type, ...)` extension method exists --
                 * those stay un-mangled in the AST (registered as `Type.member` only
                 * in funcReturns), and a `recv.member()` call site is still valid via
                 * UFCS. Checking the un-mangled `member` decl covers that without a
                 * false positive; missing an unrelated same-named fn only *under*-
                 * reports, which is the safe direction. */
                if (rec && rec->type == AST_RECORD_TYPE && rec->extra == NULL &&
                    !aetherProcDefined(decls, name) &&
                    !aetherProcDefined(decls, member) &&
                    !aetherRecordHasField(rec, member)) {
                    const char *path = aetherSemanticGetSourcePath();
                    int line = node->token->line > 0 ? node->token->line : 1;
                    if (path && *path) aetherDiagf("%s:%d: ", path, line);
                    aetherDiagf("[SCOPE-001] Aether method error: method '%s' is not defined on "
                                "type '%s'.\n", member, typeName);
                    aetherDiagf("hint: define `fn %s(...)` inside `type %s { ... }`, or fix the "
                                "method name (methods do not capture outer locals -- METH-001).\n",
                                member, typeName);
                    aetherReportGuideHelp("SCOPE-001");
                    errs++;
                }
            }
        }
    }
    AST *c0 = node->child_count > 0 ? node->children[0] : NULL;
    if (node->left && node->left != c0) errs += aetherCheckMemberCalls(node->left, decls);
    if (node->right) errs += aetherCheckMemberCalls(node->right, decls);
    if (node->extra) errs += aetherCheckMemberCalls(node->extra, decls);
    for (int i = 0; i < node->child_count; i++) {
        errs += aetherCheckMemberCalls(node->children[i], decls);
    }
    return errs;
}

AST *parseAetherAst(const char *rawSource) {
    if (!rawSource) return NULL;

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
        } else {
            /* Bare top-level statement: a script-style program with no explicit
             * `fn main`. parseStatement parses it; appendTopLevelDecl routes
             * var/const to globals and executable statements to the program body
             * (stmts), matching the rewriter's implicit-main wrapping. */
            decl = parseStatement(&p);
        }
        if (!decl) {
            p.hadError = true;
            break;
        }
        appendTopLevelDecl(decls, stmts, decl);
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

    /* If a routine named 'main' exists and there are no top-level statements,
     * inject an implicit `main()` call so the VM runs user code on start --
     * identical to rea parseRea's tail. */
    bool has_main = false;
    for (int i = 0; i < decls->child_count; i++) {
        AST *d = decls->children[i];
        if (!d) continue;
        if ((d->type == AST_FUNCTION_DECL || d->type == AST_PROCEDURE_DECL) &&
            d->token && d->token->value && strcasecmp(d->token->value, "main") == 0) {
            has_main = true;
        }
    }
    if (stmts->child_count == 0 && has_main) {
        Token *mainTok = newToken(TOKEN_IDENTIFIER, "main", 0, 0);
        AST *call = newASTNode(AST_PROCEDURE_CALL, mainTok);
        AST *stmt = newASTNode(AST_EXPR_STMT, call->token);
        setLeft(stmt, call);
        addChild(stmts, stmt);
    }

    return program;
}
