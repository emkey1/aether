/*
 * ast_parser.c -- Aether recursive-descent parser (MILESTONE 1).
 *
 * Experimental alternative to the line-based text rewriter in translate.c.
 * Enabled only when the environment variable AETHER_PARSER is set to "ast";
 * otherwise parser.c keeps using aetherRewriteSource()+parseRea() unchanged.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ast/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/globals.h"
#include "core/type_registry.h"
#include "symbol/symbol.h"
#include "rea/lexer.h"
#include "aether/semantic.h"
#include "aether/diagnostics.h"
#include "aether/translate.h"

/* Provided by core/utils.c */
Token *newToken(TokenType type, const char *value, int line, int column);
/* Compile-time constant folding, declared in compiler/compiler.h. Aether links
 * rea's parser which already pulls these in; declaring them here avoids adding a
 * compiler header dependency to the front end. evaluateCompileTimeValue is
 * aliased to rea_evaluateCompileTimeValue under FRONTEND_REA (see
 * common/frontend_symbol_aliases.h, pulled in transitively). */
void addCompilerConstant(const char *name_original_case, const Value *value, int line);
Value evaluateCompileTimeValue(AST *node);

/* Emit a diagnostic in the exact format the rewriter's (static) report helper in
 * translate.c uses, so error parity holds byte for byte. Reuses the shared
 * diagnostics helpers (aetherInferDiagnosticCode / aetherReportGuideHelp). */
static void reportAetherAstError(const char *path, int line, const char *kind,
                                 const char *detail, const char *hint) {
    const char *code = aetherInferDiagnosticCode(kind, detail);
    if (code) {
        fprintf(stderr, "%s:%d: [%s] Aether %s rewrite error: %s\n",
                path ? path : "<aether>", line > 0 ? line : 1, code,
                kind ? kind : "rewrite", detail ? detail : "unknown rewrite error.");
    } else {
        fprintf(stderr, "%s:%d: Aether %s rewrite error: %s\n",
                path ? path : "<aether>", line > 0 ? line : 1,
                kind ? kind : "rewrite", detail ? detail : "unknown rewrite error.");
    }
    if (hint && *hint) {
        fprintf(stderr, "hint: %s\n", hint);
    }
    aetherReportGuideHelp(code);
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
} AetherBindingTable;

static void bindingTableInit(AetherBindingTable *t) { t->items = NULL; t->count = 0; t->cap = 0; }

static void bindingTableFree(AetherBindingTable *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->items[i].name);
        free(t->items[i].typeName);
    }
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

static void bindingTableSet(AetherBindingTable *t, const char *name, const char *typeName) {
    if (!name || !typeName) return;
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].name, name) == 0) {
            char *dup = strdup(typeName);
            if (dup) { free(t->items[i].typeName); t->items[i].typeName = dup; }
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
    const AetherBindingTable *bindings; /* in-scope name->type for inference     */
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
    bool currentFunctionIsMethod;
    /* When true, bare identifiers that name a current-class field lower to
     * `myself.<field>` (contract expressions inside a method). */
    bool inMethodContract;
    /* Contracts accumulated from `@pre`/`@post` lines immediately before the next
     * `fn`/method decl. Consumed (and cleared) by parseFnDecl. */
    AetherPendingContracts pending;
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
                             const AetherBindingTable *bindings) {
    reaInitLexer(&p->lexer, source);
    p->queueHead = 0;
    p->queueCount = 0;
    p->currentFunctionType = TYPE_VOID;
    p->functionDepth = 0;
    p->hadError = false;
    p->currentClassName = NULL;
    p->currentMethodIndex = 0;
    p->bindings = bindings;
    p->funcReturns = NULL;
    p->tuples = NULL;
    p->nextTupleTypeId = NULL;
    p->classFields = NULL;
    p->currentTupleSig = NULL;
    p->currentPostExpr = NULL;
    p->currentFunctionName = NULL;
    p->currentFunctionIsMethod = false;
    p->inMethodContract = false;
    p->pending.preExpr = NULL;
    p->pending.postExpr = NULL;
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
    if (len == 1) return TYPE_CHAR;
    if (len == 0) return TYPE_STRING;
    uint32_t codepoint = 0;
    size_t advance = 0;
    if (decodeUtf8Codepoint(text, len, &codepoint, &advance) && advance == len) {
        return TYPE_WIDECHAR;
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
static void registerFunctionSymbol(AST *func, const char *name, VarType vtype, bool hasBody) {
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
     * that `obj.method(...)` resolves -- exactly as rea parseFunctionDecl does. */
    if (sym && sym_is_new && sym->name) {
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
static AST *parseFnDecl(AetherParser *p);
static AST *parseTypeDecl(AetherParser *p);
static AST *parseConstDeclTop(AetherParser *p);
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

/* Parse an argument list assuming '(' is the current token. Returns an
 * AST_COMPOUND whose children are the argument expressions (caller moves them
 * onto the call node, matching rea). */
static AST *parseArgList(AetherParser *p) {
    aetherAdvance(p); /* consume '(' */
    AST *args = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *arg = parseExpr(p);
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
    }
    return args;
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
        if (p->current.type != REA_TOKEN_IDENTIFIER) {
            fprintf(stderr, "L%d: Expected field name in record initializer.\n", p->current.line);
            p->hadError = true;
            break;
        }
        Token *fieldTok = copyNameToken(p);
        if (!fieldTok) break;
        aetherAdvance(p); /* consume field name */
        if (p->current.type != REA_TOKEN_COLON) {
            fprintf(stderr, "L%d: Expected ':' after field name in record initializer.\n", p->current.line);
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
        fprintf(stderr, "L%d: Expected closing delimiter for record initializer.\n", p->current.line);
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
            aetherAdvance(p); /* consume '[' */
            AST *index = parseExpr(p);
            if (p->current.type == REA_TOKEN_RIGHT_BRACKET) {
                aetherAdvance(p);
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
        if (p->current.type != REA_TOKEN_IDENTIFIER) break;
        Token *nameTok = copyNameToken(p);
        if (!nameTok) break;
        aetherAdvance(p); /* consume member name */
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
            if (cls) {
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
        fprintf(stderr, "L%d: expected a class name after 'new'.\n", p->current.line);
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
        fprintf(stderr, "L%d: expected '{' after if-expression condition.\n", p->current.line);
        p->hadError = true;
        freeAST(cond);
        return NULL;
    }
    aetherAdvance(p); /* consume '{' */
    AST *thenExpr = parseExpr(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) aetherAdvance(p);
    if (!thenExpr) { p->hadError = true; freeAST(cond); return NULL; }
    if (p->current.type != REA_TOKEN_ELSE) {
        fprintf(stderr, "L%d: if-expression requires an 'else' branch.\n", p->current.line);
        p->hadError = true;
        freeAST(cond); freeAST(thenExpr);
        return NULL;
    }
    aetherAdvance(p); /* consume 'else' */
    if (p->current.type != REA_TOKEN_LEFT_BRACE) {
        fprintf(stderr, "L%d: expected '{' after 'else' in if-expression.\n", p->current.line);
        p->hadError = true;
        freeAST(cond); freeAST(thenExpr);
        return NULL;
    }
    aetherAdvance(p); /* consume '{' */
    AST *elseExpr = parseExpr(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) aetherAdvance(p);
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
        aetherAdvance(p);
        AST *expr = parseExpr(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            aetherAdvance(p);
        }
        return expr;
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
        memcpy(lex, start, len);
        lex[len] = '\0';
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
    /* Identifier: bare variable or call f(args). */
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        Token *tok = currentAsIdentifier(p);
        if (!tok) return NULL;
        int idLine = p->current.line;
        aetherAdvance(p); /* consume identifier */

        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            /* Only names that are immediately called get the stdlib alias
             * treatment, matching the rewriter (it rewrites `name(` spans). */
            const char *raw = tok->value ? tok->value : "";
            const char *canonical = aliasBuiltinName(raw);
            if (canonical != raw && strcmp(canonical, raw) != 0) {
                freeToken(tok);
                tok = newToken(TOKEN_IDENTIFIER, canonical, idLine, 0);
                if (!tok) return NULL;
            }
            const char *tokValue = tok->value ? tok->value : "";
            AST *args = parseArgList(p);
            AST *call;
            if (strcasecmp(tokValue, "writeln") == 0) {
                call = newASTNode(AST_WRITELN, NULL);
                freeToken(tok);
            } else if (strcasecmp(tokValue, "write") == 0) {
                call = newASTNode(AST_WRITE, NULL);
                freeToken(tok);
            } else {
                call = newASTNode(AST_PROCEDURE_CALL, tok);
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

static AST *parseEquality(AetherParser *p) {
    AST *node = parseComparison(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_EQUAL_EQUAL || p->current.type == REA_TOKEN_BANG_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseComparison(p);
        if (!right) return NULL;
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

/* Assignment is right-associative and only valid with an lvalue on the left
 * (mirrors rea parseAssignment). Produces AST_ASSIGN. */
static AST *parseExpr(AetherParser *p) {
    AST *left = parseLogicalOr(p);
    if (!left) return NULL;
    if ((left->type == AST_VARIABLE || left->type == AST_FIELD_ACCESS ||
         left->type == AST_ARRAY_ACCESS) &&
        p->current.type == REA_TOKEN_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *value = parseExpr(p);
        if (!value) return NULL;
        Token *assignTok = newToken(TOKEN_ASSIGN, "=", op.line, 0);
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
    /* Stamp the directive line on the whole tree so a contract error reports the
     * @pre/@post line, not column 0. */
    if (expr->token) expr->token->line = line;
    return expr;
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
        /* TOON doc/node handles (canonical Yyjson* names). */
        { "yyjsonread", "ToonDoc" }, { "yyjsonreadfile", "ToonDoc" },
        { "yyjsongetroot", "ToonNode" }, { "yyjsongetkey", "ToonNode" },
        { "yyjsongetidx", "ToonNode" }, { "yyjsonarrget", "ToonNode" },
        /* Text-returning. */
        { "yyjsongettext", "Text" }, { "ai_chat", "Text" },
        { "openaichatcompletions", "Text" },
        { "aetherbuiltinsjson", "Text" }, { "aetherbuiltininfo", "Text" },
        { "inttostr", "Text" },
        /* Int-returning. */
        { "length", "Int" }, { "yyjsongetint", "Int" }, { "yyjsongetlen", "Int" },
        /* Real-returning. */
        { "yyjsongetreal", "Real" },
        /* Bool-returning. */
        { "hasextbuiltin", "Bool" },
        { "yyjsongetbool", "Bool" }, { "yyjsonisnull", "Bool" },
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
    /* fall back to the expression's own computed var_type. */
    const char *name = aetherTypeNameForVarType(init->var_type);
    if (name) return strdup(name);
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
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected name after 'let'.\n", p->current.line);
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
            fprintf(stderr, "L%d: expected type after ':'.\n", p->current.line);
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
        if (!typeNode) { freeToken(nameTok); free(declaredTypeName); return NULL; }
        aetherAdvance(p); /* consume type name */
    }

    AST *init = NULL;
    if (p->current.type == REA_TOKEN_EQUAL) {
        aetherAdvance(p); /* consume '=' */
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
                    bindingTableSet((AetherBindingTable *)p->bindings,
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

    /* Inferred type: derive from the initializer, like the rewriter. */
    if (!explicitType) {
        if (!init) {
            fprintf(stderr, "L%d: '%s' requires a type or an initializer.\n",
                    kwLine, nameTok->value ? nameTok->value : "let");
            p->hadError = true;
            freeToken(nameTok);
            return NULL;
        }
        char *inferred = inferLetTypeName(p, init);
        if (!inferred) {
            const char *path = aetherSemanticGetSourcePath();
            if (path && *path) fprintf(stderr, "%s:%d: ", path, kwLine);
            fprintf(stderr,
                    "[declaration] cannot infer the type of '%s' from its initializer.\n",
                    nameTok->value ? nameTok->value : "");
            p->hadError = true;
            freeToken(nameTok);
            freeAST(init);
            return NULL;
        }
        VarType iv = TYPE_UNKNOWN;
        const char *reaName = NULL;
        if (mapAetherType(inferred, strlen(inferred), &reaName, &iv)) {
            Token *ttok = newToken(TOKEN_IDENTIFIER, reaName, kwLine, 0);
            typeNode = newASTNode(AST_TYPE_IDENTIFIER, ttok);
            setTypeAST(typeNode, iv);
            vtype = iv;
        } else {
            /* user-defined type name */
            Token *ttok = newToken(TOKEN_IDENTIFIER, inferred, kwLine, 0);
            typeNode = newASTNode(AST_TYPE_REFERENCE, ttok);
            setTypeAST(typeNode, TYPE_UNKNOWN);
            vtype = TYPE_UNKNOWN;
        }
        declaredTypeName = inferred; /* take ownership for binding below */
    }

    if (declaredTypeName)
        bindingTableSet((AetherBindingTable *)p->bindings, nameTok->value, declaredTypeName);
    free(declaredTypeName);

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);
    return decl;
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

/* ret (a, b, ...) ;  for a tuple-return function. Lowers to the same shape the
 * rewriter emits: per-slot global writes `__aether_tuple_N_item<k> = expr<k>;`
 * followed by the @post guard (if any) and a bare `return;`. The result is an
 * AST_COMPOUND splice (i_val==1) so parseBlock flattens it into the body. */
static AST *parseTupleReturn(AetherParser *p, int line) {
    const AetherTupleSig *sig = p->currentTupleSig;
    aetherAdvance(p); /* consume '(' */
    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* declaration-group splice wrapper */
    size_t idx = 0;
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *item = parseExpr(p);
        if (!item) { p->hadError = true; freeAST(outer); return NULL; }
        if (idx >= sig->itemCount) {
            fprintf(stderr, "L%d: tuple return has more values than the declared return type.\n", line);
            p->hadError = true;
            freeAST(item);
            freeAST(outer);
            return NULL;
        }
        char fieldName[80];
        snprintf(fieldName, sizeof(fieldName), "__aether_tuple_%d_item%zu", sig->typeId, idx);
        addChild(outer, buildSimpleAssign(fieldName, item, line));
        idx++;
        if (p->current.type == REA_TOKEN_COMMA) { aetherAdvance(p); continue; }
        break;
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) aetherAdvance(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) aetherAdvance(p);
    if (idx != sig->itemCount) {
        fprintf(stderr, "L%d: tuple return arity does not match the declared return type.\n", line);
        p->hadError = true;
        freeAST(outer);
        return NULL;
    }
    if (p->currentPostExpr) {
        AST *guard = buildContractGuard(p, p->currentPostExpr, "post",
                                        p->currentFunctionName, line);
        if (!guard) { freeAST(outer); return NULL; }
        addChild(outer, guard);
    }
    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *ret = newASTNode(AST_RETURN, retTok);
    setLeft(ret, NULL);
    setTypeAST(ret, TYPE_VOID);
    addChild(outer, ret);
    return outer;
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
        if (p->current.type != REA_TOKEN_IDENTIFIER || nameCount >= 16) {
            fprintf(stderr, "L%d: expected a binding name in tuple destructuring.\n", p->current.line);
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
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) aetherAdvance(p);
    if (p->current.type != REA_TOKEN_EQUAL) {
        fprintf(stderr, "L%d: expected '=' in tuple destructuring.\n", kwLine);
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    aetherAdvance(p); /* consume '=' */

    /* The right side must be a direct call `name(args)` to a tuple-return fn. */
    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        const char *path = aetherSemanticGetSourcePath();
        if (path && *path) fprintf(stderr, "%s:%d: ", path, kwLine);
        fprintf(stderr, "[feature] tuple destructuring currently requires a direct call to a known tuple-return function.\n");
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
        const char *path = aetherSemanticGetSourcePath();
        if (path && *path) fprintf(stderr, "%s:%d: ", path, kwLine);
        fprintf(stderr, "[feature] tuple destructuring target is not a known tuple-return function.\n");
        p->hadError = true;
        for (size_t i = 0; i < nameCount; i++) free(names[i]);
        return NULL;
    }
    if (sig->itemCount != nameCount) {
        const char *path = aetherSemanticGetSourcePath();
        if (path && *path) fprintf(stderr, "%s:%d: ", path, kwLine);
        fprintf(stderr, "[feature] tuple destructuring arity does not match the function return tuple.\n");
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

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */
    /* The call statement. */
    AST *callStmt = newASTNode(AST_EXPR_STMT, call->token);
    setLeft(callStmt, call);
    addChild(outer, callStmt);
    /* One typed local per binding, reading the matching slot global, and record
     * the binding's Aether type for downstream inference. */
    for (size_t i = 0; i < nameCount; i++) {
        char slot[80];
        snprintf(slot, sizeof(slot), "__aether_tuple_%d_item%zu", sig->typeId, i);
        VarType vt = TYPE_UNKNOWN;
        AST *typeNode = buildTypeNode(sig->itemTypes[i], strlen(sig->itemTypes[i]), kwLine, &vt);
        Token *nameTok = newToken(TOKEN_IDENTIFIER, names[i], kwLine, 0);
        AST *var = newASTNode(AST_VARIABLE, nameTok);
        setTypeAST(var, vt);
        Token *slotTok = newToken(TOKEN_IDENTIFIER, slot, kwLine, 0);
        AST *slotVar = newASTNode(AST_VARIABLE, slotTok);
        setTypeAST(slotVar, vt);
        AST *decl = newASTNode(AST_VAR_DECL, NULL);
        addChild(decl, var);
        setLeft(decl, slotVar);
        setRight(decl, typeNode);
        setTypeAST(decl, vt);
        addChild(outer, decl);
        bindingTableSet((AetherBindingTable *)p->bindings, names[i], sig->itemTypes[i]);
        free(names[i]);
    }
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
        fprintf(stderr, "L%d: a tuple-return function must return a tuple literal `(...)`.\n", line);
        p->hadError = true;
        return NULL;
    }

    aetherAdvance(p); /* consume 'ret' */
    AST *value = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON && p->current.type != REA_TOKEN_RIGHT_BRACE &&
        p->current.type != REA_TOKEN_EOF) {
        value = parseExpr(p);
    } else if (p->currentFunctionType != TYPE_VOID) {
        fprintf(stderr, "L%d: return requires a value.\n", line);
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
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        aetherAdvance(p);
    }
    AST *condition = parseExpr(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
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


/* Parse a range bound expression up to (but not consuming) the AE_TOKEN_DOTDOT
 * or the body-opening '{'. Bounds may be arbitrary expressions (numbers,
 * identifiers, calls, arithmetic), so reuse the full expression parser, stopping
 * the precedence ladder at the range/brace boundary. The expression parser
 * naturally stops at '{' and AE_TOKEN_DOTDOT (neither is an operator it
 * recognizes), so a plain parseExpr() call suffices. */

/* loop NAME in LOW..HIGH { body }
 *
 * The rewriter lowers this to a C-style half-open for loop
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
    aetherAdvance(p); /* consume 'loop' */

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected loop variable name.\n", p->current.line);
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
        fprintf(stderr, "L%d: expected 'in' in loop range.\n", p->current.line);
        p->hadError = true;
        free(nameBuf);
        return NULL;
    }
    aetherAdvance(p); /* consume 'in' */

    AST *low = parseExpr(p);
    if (!low || p->current.type != AE_TOKEN_DOTDOT) {
        fprintf(stderr, "L%d: expected '<low>..<high>' in loop range.\n", idLine);
        p->hadError = true;
        if (low) freeAST(low);
        free(nameBuf);
        return NULL;
    }
    aetherAdvance(p); /* consume '..' */
    AST *high = parseExpr(p);
    if (!high) {
        fprintf(stderr, "L%d: could not parse loop range bounds.\n", idLine);
        p->hadError = true;
        freeAST(low);
        free(nameBuf);
        return NULL;
    }

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        fprintf(stderr, "L%d: expected '{' to open loop body.\n", idLine);
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

static AST *parseStatement(AetherParser *p) {
    /* fx { ... } -- effect wrapper. The marker is erased; we return the inner
     * block directly (an AST_COMPOUND), matching the rewriter which strips the
     * `fx` token and leaves the brace block. */
    if (isAetherKeyword(&p->current, "fx")) {
        aetherAdvance(p); /* consume 'fx' */
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            return parseBlock(p);
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
    if (isAetherKeyword(&p->current, "ret")) {
        return parseRet(p);
    }
    if (isAetherKeyword(&p->current, "loop")) {
        /* Only the range form `loop i in a..b` is handled in Milestone 1. */
        return parseLoopRange(p);
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
        return expr; /* assignments act as statements directly */
    }
    AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
    setLeft(stmt, expr);
    return stmt;
}

static AST *parseBlock(AetherParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    aetherAdvance(p); /* consume '{' */
    AST *block = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) break;
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

/* Collect a run of `@pre`/`@post`/`@pure`/`@cost` annotation lines that precede a
 * `fn`/method decl into `p->pending`. The shared Rea lexer yields `@` as
 * REA_TOKEN_UNKNOWN("@"); we read the directive identifier, then capture the rest
 * of the physical line as the raw contract expression (alias/method-scope/tuple
 * rewriting happens when the guard is built). `@pure`/`@cost` carry no codegen --
 * the semantic layer validates them on the source text -- so we just skip them.
 * Detached/empty/misplaced annotations are diagnosed by semantic.c; here we are
 * permissive so the parser produces an AST and the text-based checks fire. */
static void collectPendingAnnotations(AetherParser *p) {
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
        /* Raw expression = trimmed remainder of the line after the directive. */
        const char *exprStart = dEnd;
        while (exprStart < lineEnd && isspace((unsigned char)*exprStart)) exprStart++;
        const char *exprEnd = lineEnd;
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
        }
        /* @pure / @cost: no codegen; presence already in the source for semantic.c */
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

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected function name after 'fn'.\n", p->current.line);
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
        fprintf(stderr, "L%d: expected '(' after function name.\n", p->current.line);
        p->hadError = true;
        freeToken(nameTok);
        return NULL;
    }
    aetherAdvance(p); /* consume '(' */

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
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type != REA_TOKEN_IDENTIFIER) {
            fprintf(stderr, "L%d: expected parameter name.\n", p->current.line);
            p->hadError = true;
            break;
        }
        Token *paramNameTok = currentAsIdentifier(p);
        if (!paramNameTok) { p->hadError = true; break; }
        aetherAdvance(p); /* consume param name */

        if (p->current.type != REA_TOKEN_COLON) {
            fprintf(stderr, "L%d: expected ':' after parameter name.\n", p->current.line);
            p->hadError = true;
            freeToken(paramNameTok);
            break;
        }
        aetherAdvance(p); /* consume ':' */

        if (p->current.type == REA_TOKEN_RIGHT_PAREN || p->current.type == REA_TOKEN_COMMA ||
            p->current.type == REA_TOKEN_EOF) {
            fprintf(stderr, "L%d: expected parameter type.\n", p->current.line);
            p->hadError = true;
            freeToken(paramNameTok);
            break;
        }
        VarType pvtype = TYPE_UNKNOWN;
        AST *ptypeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &pvtype);
        if (!ptypeNode) { freeToken(paramNameTok); p->hadError = true; break; }
        aetherAdvance(p); /* consume param type */

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
    }

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
        return NULL;
    }
    if (p->current.type == REA_TOKEN_ARROW) {
        aetherAdvance(p); /* consume '->' */
        if (p->current.type == REA_TOKEN_LEFT_BRACE || p->current.type == REA_TOKEN_EOF) {
            fprintf(stderr, "L%d: expected return type after '->'.\n", p->current.line);
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
                    reportAetherAstError(aetherSemanticGetSourcePath(), fnLine, "feature",
                                         "tuple return types are currently only supported on top-level functions.",
                                         "return a record/object from methods, or move tuple-return logic to a top-level helper function.");
                    p->hadError = true;
                } else {
                    hasTupleReturn = true;
                    vtype = TYPE_VOID;          /* tuple fns lower to void */
                    returnTypeNode = NULL;
                    /* The tuple signature + globals were registered in the
                     * top-level forward-decl pre-pass; look it up by name. */
                    tupleSig = tupleTableGet(p->tuples, nameTok->value,
                                             nameTok->value ? strlen(nameTok->value) : 0);
                }
                /* Re-sync the lexer past the `(...)` we read straight from source. */
                p->lexer.pos = (size_t)(tupleEnd - p->lexer.source);
                p->queueHead = 0; p->queueCount = 0;
                aetherAdvance(p);
            } else {
                /* Not a tuple type after all: fall through to scalar parsing. */
                retTypeName = (char *)malloc(p->current.length + 1);
                if (retTypeName) {
                    memcpy(retTypeName, p->current.start, p->current.length);
                    retTypeName[p->current.length] = '\0';
                }
                returnTypeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &vtype);
                aetherAdvance(p);
            }
        } else {
            retTypeName = (char *)malloc(p->current.length + 1);
            if (retTypeName) {
                memcpy(retTypeName, p->current.start, p->current.length);
                retTypeName[p->current.length] = '\0';
            }
            returnTypeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &vtype);
            aetherAdvance(p); /* consume return type */
        }
    }

    /* Record the (possibly-mangled) function/method name -> Aether return type
     * so inferred `let x = f(...)` / `x = recv.method(...)` can resolve it, the
     * way the rewriter's function table does. Recorded before the body so a
     * recursive call inside the body could resolve too. */
    if (retTypeName && nameTok->value && p->funcReturns) {
        bindingTableSet(p->funcReturns, nameTok->value, retTypeName);
    }
    free(retTypeName);
    retTypeName = NULL;

    /* --- Contract + tuple body context (MILESTONE 3) --- */
    /* Take ownership of the pending @pre/@post collected before this decl. The
     * post-expr text is rewritten for tuple result slots (`result.N` ->
     * `__aether_tuple_<id>_item<N>`) here so parseRet / the guard builder see a
     * plain expression. Method field-prefix + builtin aliasing happen
     * automatically in parseExprFromText. */
    char *preExpr = p->pending.preExpr;
    char *postExpr = p->pending.postExpr;
    p->pending.preExpr = NULL;
    p->pending.postExpr = NULL;
    if (hasTupleReturn && tupleSig && postExpr) {
        size_t cap = strlen(postExpr) + 64;
        char *rewritten = (char *)malloc(cap);
        if (rewritten) {
            size_t w = 0;
            const char *s = postExpr;
            while (*s) {
                if (strncmp(s, "result.", 7) == 0 &&
                    (s == postExpr ||
                     !(isalnum((unsigned char)s[-1]) || s[-1] == '_' || s[-1] == '.'))) {
                    const char *digits = s + 7;
                    if (isdigit((unsigned char)*digits)) {
                        unsigned long k = strtoul(digits, NULL, 10);
                        const char *dEnd = digits;
                        while (isdigit((unsigned char)*dEnd)) dEnd++;
                        char repl[80];
                        int rl = snprintf(repl, sizeof(repl),
                                          "__aether_tuple_%d_item%lu", tupleSig->typeId, k);
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
            free(postExpr);
            postExpr = rewritten;
        }
    }

    const AetherTupleSig *prevTupleSig = p->currentTupleSig;
    const char *prevPostExpr = p->currentPostExpr;
    const char *prevFnName = p->currentFunctionName;
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
    p->currentPostExpr = postExpr; /* parseRet consumes it for value/tuple returns */

    /* Body. */
    VarType prevType = p->currentFunctionType;
    int prevDepth = p->functionDepth;
    p->currentFunctionType = vtype;
    p->functionDepth++;

    AST *block = NULL;
    bool hasBody = false;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        block = parseBlock(p);
        hasBody = true;
    }

    p->currentFunctionType = prevType;
    p->functionDepth = prevDepth;

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
     * value-returning fns handle @post at each `ret` instead. */
    if (block && postExpr && !p->hadError && vtype == TYPE_VOID && !hasTupleReturn) {
        AST *guard = buildContractGuard(p, postExpr, "post", guardName, fnLine);
        if (guard) addChild(block, guard);
    }

    /* Restore the enclosing function's contract/tuple context. */
    p->currentTupleSig = prevTupleSig;
    p->currentPostExpr = prevPostExpr;
    p->currentFunctionName = prevFnName;
    p->currentFunctionIsMethod = prevIsMethod;
    free(preExpr);
    free(postExpr);

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

    registerFunctionSymbol(func, nameTok->value ? nameTok->value : "", vtype, hasBody);
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

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected name after 'const'.\n", p->current.line);
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
            fprintf(stderr, "L%d: expected type after ':'.\n", p->current.line);
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
        fprintf(stderr, "L%d: const declaration requires '= value'.\n", p->current.line);
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
        bindingTableSet((AetherBindingTable *)p->bindings, nameTok->value, declaredTypeName);

    /* Fold + register the compile-time value (rea parseConstDecl tail). */
    {
        Value v = evaluateCompileTimeValue(value);
        if (v.type != TYPE_VOID && v.type != TYPE_UNKNOWN) {
            if (p->functionDepth == 0) {
                addCompilerConstant(nameTok->value, &v, nameTok->line);
            }
            if (!typeNode) setTypeAST(node, v.type);
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
        fprintf(stderr, "L%d: expected a type name after 'type'.\n", p->current.line);
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
                    fprintf(stderr, "L%d: expected ':' after field name in type.\n",
                            p->current.line);
                    p->hadError = true;
                    freeToken(fieldTok);
                    break;
                }
                aetherAdvance(p); /* consume ':' */
                VarType fvtype = TYPE_UNKNOWN;
                AST *ftypeNode = buildTypeNode(p->current.start, p->current.length,
                                               p->current.line, &fvtype);
                if (!ftypeNode) { freeToken(fieldTok); p->hadError = true; break; }
                aetherAdvance(p); /* consume field type */
                AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
                setTypeAST(fieldVar, fvtype);
                AST *fieldDecl = newASTNode(AST_VAR_DECL, NULL);
                addChild(fieldDecl, fieldVar);
                setRight(fieldDecl, ftypeNode);
                setTypeAST(fieldDecl, fvtype);
                addChild(recordAst, fieldDecl);
                if (p->current.type == REA_TOKEN_SEMICOLON) {
                    aetherAdvance(p);
                }
            } else if (p->current.type == REA_TOKEN_SEMICOLON) {
                aetherAdvance(p); /* tolerate stray semicolons */
            } else {
                fprintf(stderr, "L%d: unexpected token in type body.\n", p->current.line);
                p->hadError = true;
                break;
            }
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
            aetherAdvance(p);
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
        node->type == AST_CONST_DECL) {
        addChild(decls, node);
    } else {
        addChild(stmts, node);
    }
}

/* Build a global variable declaration `<reaType> <name>;` AST node (the lowering
 * for a tuple slot global). Mirrors rea's parseVarDecl for a typed, uninitialized
 * scalar at top level. */
static AST *buildGlobalTupleSlotDecl(const char *aetherType, const char *name, int line) {
    VarType vt = TYPE_UNKNOWN;
    AST *typeNode = buildTypeNode(aetherType, strlen(aetherType), line, &vt);
    if (!typeNode) return NULL;
    Token *nTok = newToken(TOKEN_IDENTIFIER, name, line, 0);
    AST *var = newASTNode(AST_VARIABLE, nTok);
    setTypeAST(var, vt);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setRight(decl, typeNode);
    setTypeAST(decl, vt);
    return decl;
}

/* Pre-pass over the (TOON-preprocessed) source that registers tuple-return
 * function signatures and emits the per-slot global var-decls into `decls`, the
 * way translate.c appendTopLevelForwardDeclarations does. Scans only top-level
 * (column-0) `fn NAME(...) -> (...)` lines; method tuple returns are unsupported
 * (handled/diagnosed in parseFnDecl). Returns false on allocation failure. */
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
                        for (size_t i = 0; i < itemCount; i++) {
                            char slot[80];
                            snprintf(slot, sizeof(slot), "__aether_tuple_%d_item%zu", typeId, i);
                            AST *g = buildGlobalTupleSlotDecl(items[i], slot, lineNumber);
                            if (g) addChild(decls, g);
                        }
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

AST *parseAetherAst(const char *rawSource) {
    if (!rawSource) return NULL;

    /* TOON pre-pass: reuse the rewriter's preprocessToonBlocks so `toon:` blocks
     * become escaped string literals before the lexer runs (roadmap mandate: do
     * not re-implement TOON). On failure a diagnostic is already reported. */
    const char *sourcePath = aetherSemanticGetSourcePath();
    char *toonSource = aetherPreprocessToonBlocksForAst(rawSource, sourcePath);
    if (!toonSource) return NULL;
    /* Builtin-alias pre-pass: lower stdlib/TOON/capability call spellings to the
     * canonical pscal builtins (toon_*->Yyjson*, has_toon/string_eq/...), again
     * reusing the rewriter's machinery rather than re-implementing it. Runs after
     * TOON-block extraction so `let d: TOON = "..."` literal bindings are visible
     * to the toon_parse(d) lowering. */
    char *source = aetherPreprocessBuiltinsForAst(toonSource);
    free(toonSource);
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

    while (p.current.type != REA_TOKEN_EOF && !p.hadError) {
        /* Contract annotations (`@pre/@post/@pure/@cost`) precede a `fn`. */
        collectPendingAnnotations(&p);
        if (p.hadError || p.current.type == REA_TOKEN_EOF) break;
        AST *decl = NULL;
        if (isAetherKeyword(&p.current, "fn")) {
            decl = parseFnDecl(&p);
        } else if (p.current.type == REA_TOKEN_TYPE || isAetherKeyword(&p.current, "type")) {
            decl = parseTypeDecl(&p);
        } else if (p.current.type == REA_TOKEN_CONST || isAetherKeyword(&p.current, "const")) {
            decl = parseConstDeclTop(&p);
        } else {
            const char *path = aetherSemanticGetSourcePath();
            if (path && *path) {
                fprintf(stderr, "%s:%d: ", path, p.current.line);
            }
            fprintf(stderr,
                    "Unexpected token '%.*s' at line %d (expected a top-level 'fn', 'type' or 'const' declaration).\n",
                    (int)p.current.length, p.current.start, p.current.line);
            p.hadError = true;
            break;
        }
        if (!decl) {
            p.hadError = true;
            break;
        }
        appendTopLevelDecl(decls, stmts, decl);
    }

    aetherFreePending(&p.pending);
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
