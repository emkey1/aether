/*
 * types.c -- the Aether type oracle and the typed pass (W7-24a). See types.h.
 *
 * The pass runs after reaPerformSemanticAnalysis, over the lowered AST the
 * compiler will build, so it sees every imported module and every lowering
 * (loops, tuple returns, the array un-alias prologue). It keeps its own scope
 * environment built from declaration type nodes -- the one place the AST's
 * types are trustworthy -- and asks nothing of rea's resolver.
 *
 * The core hosts no rules. With AETHER_DUMP_TYPES unset it does not run at all.
 */

#include "aether/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/globals.h"
#include "core/types.h"
#include "core/utils.h"
#include "aether/experiment.h"
#include "aether/parser.h"
#include "aether/semantic.h"

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

static AetherType mk(AetherTypeKind k) {
    AetherType t;
    memset(&t, 0, sizeof(t));
    t.kind = k;
    return t;
}

static AetherType mkNamed(AetherTypeKind k, const char *name) {
    AetherType t = mk(k);
    t.name = name;
    return t;
}

int aetherOracleIsInt(AetherType t) { return t.kind == AETHER_T_INT; }
int aetherOracleIsReal(AetherType t) { return t.kind == AETHER_T_REAL; }

static int isNumeric(AetherType t) { return t.kind == AETHER_T_INT || t.kind == AETHER_T_REAL; }
static int isTextual(AetherType t) { return t.kind == AETHER_T_TEXT || t.kind == AETHER_T_CHAR; }

static AetherType fromVarType(VarType vt) {
    switch (vt) {
        case TYPE_INT64: case TYPE_INT32: case TYPE_INT16: case TYPE_INT8:
        case TYPE_UINT64: case TYPE_UINT32: case TYPE_UINT16: case TYPE_UINT8:
        case TYPE_WORD: case TYPE_BYTE:
            return mk(AETHER_T_INT);
        case TYPE_DOUBLE: case TYPE_FLOAT: case TYPE_LONG_DOUBLE:
            return mk(AETHER_T_REAL);
        case TYPE_STRING: case TYPE_UNICODE_STRING:
            return mk(AETHER_T_TEXT);
        case TYPE_CHAR: case TYPE_WIDECHAR:
            return mk(AETHER_T_CHAR);
        case TYPE_BOOLEAN:
            return mk(AETHER_T_BOOL);
        case TYPE_MEMORYSTREAM:
            return mkNamed(AETHER_T_HANDLE, "MStream");
        case TYPE_FILE:
            return mkNamed(AETHER_T_HANDLE, "File");
        case TYPE_VOID:
            return mk(AETHER_T_VOID);
        case TYPE_NIL:
            return mk(AETHER_T_NIL);
        case TYPE_ARRAY: {
            AetherType t = mk(AETHER_T_ARRAY);
            t.rank = 1;
            return t;
        }
        case TYPE_POINTER: case TYPE_RECORD:
            return mk(AETHER_T_RECORD);
        default:
            return mk(AETHER_T_UNKNOWN);
    }
}

static AetherType scalarFromTypeName(const char *n, VarType fallback) {
    if (!n) return fromVarType(fallback);
    if (!strcasecmp(n, "int") || !strcasecmp(n, "integer") || !strcasecmp(n, "int64") ||
        !strcasecmp(n, "longint") || !strcasecmp(n, "byte") || !strcasecmp(n, "word"))
        return mk(AETHER_T_INT);
    if (!strcasecmp(n, "float") || !strcasecmp(n, "real") || !strcasecmp(n, "double"))
        return mk(AETHER_T_REAL);
    if (!strcasecmp(n, "str") || !strcasecmp(n, "string") || !strcasecmp(n, "text"))
        return mk(AETHER_T_TEXT);
    if (!strcasecmp(n, "bool") || !strcasecmp(n, "boolean")) return mk(AETHER_T_BOOL);
    if (!strcasecmp(n, "char")) return mk(AETHER_T_CHAR);
    if (!strcasecmp(n, "mstream")) return mkNamed(AETHER_T_HANDLE, "MStream");
    if (!strcasecmp(n, "file") || !strcasecmp(n, "text_file")) return mkNamed(AETHER_T_HANDLE, "File");
    if (fallback == TYPE_RECORD || fallback == TYPE_POINTER) return mkNamed(AETHER_T_RECORD, n);
    {
        AetherType t = fromVarType(fallback);
        if (t.kind == AETHER_T_UNKNOWN || t.kind == AETHER_T_RECORD) return mkNamed(AETHER_T_RECORD, n);
        return t;
    }
}

AetherType aetherTypeFromTypeNode(const AST *n, VarType fallback) {
    if (!n) return fromVarType(fallback);
    switch (n->type) {
        case AST_TYPE_IDENTIFIER:
            return scalarFromTypeName(n->token ? n->token->value : NULL, n->var_type);
        case AST_TYPE_REFERENCE:
            if (n->token && n->token->value) {
                if (n->var_type == TYPE_RECORD || n->var_type == TYPE_POINTER ||
                    n->var_type == TYPE_UNKNOWN)
                    return mkNamed(AETHER_T_RECORD, n->token->value);
                return scalarFromTypeName(n->token->value, n->var_type);
            }
            return fromVarType(n->var_type);
        case AST_POINTER_TYPE:
            if (n->right) return aetherTypeFromTypeNode(n->right, TYPE_RECORD);
            return mk(AETHER_T_RECORD);
        case AST_ARRAY_TYPE: {
            const AST *e = n;
            int rank = 0;
            while (e && e->type == AST_ARRAY_TYPE) {
                rank++;
                e = e->right;
            }
            AetherType el = e ? aetherTypeFromTypeNode(e, e->var_type) : mk(AETHER_T_UNKNOWN);
            AetherType t = mk(AETHER_T_ARRAY);
            t.rank = rank;
            t.elem = el.kind;
            t.name = el.name;
            return t;
        }
        default:
            return fromVarType(fallback != TYPE_UNKNOWN ? fallback : n->var_type);
    }
}

static const char *kindName(AetherTypeKind k) {
    switch (k) {
        case AETHER_T_VOID: return "Void";
        case AETHER_T_INT: return "Int";
        case AETHER_T_REAL: return "Real";
        case AETHER_T_TEXT: return "Text";
        case AETHER_T_CHAR: return "Char";
        case AETHER_T_BOOL: return "Bool";
        case AETHER_T_NIL: return "nil";
        case AETHER_T_HANDLE: return "Handle";
        case AETHER_T_RECORD: return "Record";
        case AETHER_T_ARRAY: return "Array";
        default: return "?";
    }
}

const char *aetherTypeFormat(AetherType t, char *buf, size_t n) {
    if (!buf || n == 0) return "";
    if (t.kind == AETHER_T_ARRAY) {
        const char *el = (t.elem == AETHER_T_RECORD || t.elem == AETHER_T_HANDLE) && t.name
                             ? t.name : kindName(t.elem);
        size_t used = (size_t)snprintf(buf, n, "%s", el);
        for (int i = 0; i < t.rank && used + 2 < n; i++) {
            buf[used++] = '[';
            buf[used++] = ']';
            buf[used] = '\0';
        }
        return buf;
    }
    if ((t.kind == AETHER_T_RECORD || t.kind == AETHER_T_HANDLE) && t.name) {
        snprintf(buf, n, "%s", t.name);
        return buf;
    }
    snprintf(buf, n, "%s", kindName(t.kind));
    return buf;
}

AetherAgreement aetherTypeAgreement(AetherType want, AetherType have) {
    if (have.kind == AETHER_T_UNKNOWN || want.kind == AETHER_T_UNKNOWN) return AETHER_AGREE_UNKNOWN;
    if (want.kind == have.kind) {
        if (want.kind == AETHER_T_RECORD) {
            if (!want.name || !have.name) return AETHER_AGREE_COMPAT;
            return strcasecmp(want.name, have.name) == 0 ? AETHER_AGREE_EXACT : AETHER_AGREE_MISMATCH;
        }
        if (want.kind == AETHER_T_ARRAY) {
            if (have.elem == AETHER_T_UNKNOWN || want.elem == AETHER_T_UNKNOWN)
                return AETHER_AGREE_COMPAT;
            if (want.rank != have.rank) return AETHER_AGREE_MISMATCH;
            if (want.elem == have.elem) {
                if (want.elem == AETHER_T_RECORD && want.name && have.name &&
                    strcasecmp(want.name, have.name) != 0)
                    return AETHER_AGREE_MISMATCH;
                return AETHER_AGREE_EXACT;
            }
            if ((want.elem == AETHER_T_TEXT && have.elem == AETHER_T_CHAR) ||
                (want.elem == AETHER_T_CHAR && have.elem == AETHER_T_TEXT) ||
                (want.elem == AETHER_T_INT && have.elem == AETHER_T_HANDLE) ||
                (want.elem == AETHER_T_HANDLE && have.elem == AETHER_T_INT))
                return AETHER_AGREE_COMPAT;
            if (want.elem == AETHER_T_REAL && have.elem == AETHER_T_INT) return AETHER_AGREE_WIDEN;
            return AETHER_AGREE_MISMATCH;
        }
        return AETHER_AGREE_EXACT;
    }
    if (want.kind == AETHER_T_REAL && have.kind == AETHER_T_INT) return AETHER_AGREE_WIDEN;
    if (want.kind == AETHER_T_INT && have.kind == AETHER_T_REAL) return AETHER_AGREE_NARROW;
    if (isTextual(want) && isTextual(have)) return AETHER_AGREE_COMPAT;
    if (have.kind == AETHER_T_NIL &&
        (want.kind == AETHER_T_RECORD || want.kind == AETHER_T_ARRAY || want.kind == AETHER_T_HANDLE))
        return AETHER_AGREE_COMPAT;
    /* Opaque handles lower to Int (TOON) or a stream slot; only the source
     * text-flow checks know them by name. */
    if ((want.kind == AETHER_T_HANDLE && have.kind == AETHER_T_INT) ||
        (want.kind == AETHER_T_INT && have.kind == AETHER_T_HANDLE))
        return AETHER_AGREE_COMPAT;
    return AETHER_AGREE_MISMATCH;
}

const char *aetherAgreementName(AetherAgreement a) {
    switch (a) {
        case AETHER_AGREE_EXACT: return "exact";
        case AETHER_AGREE_WIDEN: return "widen";
        case AETHER_AGREE_NARROW: return "narrow";
        case AETHER_AGREE_COMPAT: return "compat";
        case AETHER_AGREE_UNKNOWN: return "unknown";
        default: return "MISMATCH";
    }
}

/* ------------------------------------------------------------------ */
/* Program index: records and function signatures                      */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; const AST *node; } IndexEntry;
typedef struct { IndexEntry *items; int count, cap; } Index;

static Index g_records;   /* name -> RECORD_TYPE */
static Index g_functions; /* name -> FUNCTION_DECL / PROCEDURE_DECL */
static Index g_globals;   /* name -> VAR_DECL / CONST_DECL (type via declTypeOf) */

static void indexAdd(Index *ix, const char *name, const AST *node) {
    if (!name) return;
    if (ix->count == ix->cap) {
        int cap = ix->cap ? ix->cap * 2 : 64;
        IndexEntry *g = (IndexEntry *)realloc(ix->items, (size_t)cap * sizeof(IndexEntry));
        if (!g) return;
        ix->items = g;
        ix->cap = cap;
    }
    ix->items[ix->count].name = name;
    ix->items[ix->count].node = node;
    ix->count++;
}

static const AST *indexGet(const Index *ix, const char *name) {
    if (!name) return NULL;
    /* Later entries win: a definition follows its forward declaration. */
    for (int i = ix->count - 1; i >= 0; i--) {
        if (strcasecmp(ix->items[i].name, name) == 0) return ix->items[i].node;
    }
    return NULL;
}

static void indexFree(Index *ix) {
    free(ix->items);
    memset(ix, 0, sizeof(*ix));
}

static void indexProgram(const AST *n, int topLevel) {
    if (!n) return;
    switch (n->type) {
        case AST_TYPE_DECL:
            if (n->token && n->token->value && n->left && n->left->type == AST_RECORD_TYPE)
                indexAdd(&g_records, n->token->value, n->left);
            break;
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            if (n->token && n->token->value) indexAdd(&g_functions, n->token->value, n);
            topLevel = 0;
            break;
        case AST_CONST_DECL:
            if (topLevel && n->token && n->token->value) indexAdd(&g_globals, n->token->value, n);
            break;
        case AST_VAR_DECL:
            if (topLevel) {
                for (int i = 0; i < n->child_count; i++) {
                    const AST *v = n->children[i];
                    if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                        indexAdd(&g_globals, v->token->value, n);
                }
            }
            return; /* nothing below a declaration declares a global */
        default:
            break;
    }
    indexProgram(n->left, topLevel);
    indexProgram(n->right, topLevel);
    indexProgram(n->extra, topLevel);
    for (int i = 0; i < n->child_count; i++) indexProgram(n->children[i], topLevel);
}

static AetherType declTypeOf(const AST *decl) {
    if (!decl) return mk(AETHER_T_UNKNOWN);
    return aetherTypeFromTypeNode(decl->right, decl->var_type);
}

static AetherType recordFieldType(const char *recName, const char *field) {
    const AST *rec = indexGet(&g_records, recName);
    if (!rec || !field) return mk(AETHER_T_UNKNOWN);
    for (int i = 0; i < rec->child_count; i++) {
        const AST *f = rec->children[i];
        if (!f || f->type != AST_VAR_DECL) continue;
        for (int j = 0; j < f->child_count; j++) {
            const AST *v = f->children[j];
            if (v && v->token && v->token->value && strcasecmp(v->token->value, field) == 0)
                return declTypeOf(f);
        }
    }
    return mk(AETHER_T_UNKNOWN);
}

/* A function's declared parameters: children that are VAR_DECLs. */
static int functionParamTypes(const AST *fn, AetherType *out, int max) {
    int n = 0;
    if (!fn) return 0;
    for (int i = 0; i < fn->child_count && n < max; i++) {
        const AST *p = fn->children[i];
        if (!p || p->type != AST_VAR_DECL) continue;
        AetherType t = declTypeOf(p);
        int names = 0;
        for (int j = 0; j < p->child_count; j++)
            if (p->children[j] && p->children[j]->type == AST_VARIABLE) names++;
        if (names == 0) names = 1;
        for (int j = 0; j < names && n < max; j++) out[n++] = t;
    }
    return n;
}

static AetherType functionReturnType(const AST *fn) {
    if (!fn) return mk(AETHER_T_UNKNOWN);
    if (fn->type == AST_PROCEDURE_DECL) return mk(AETHER_T_VOID);
    return aetherTypeFromTypeNode(fn->right, fn->var_type);
}

/* User function for a call node: exact (possibly `Type.method` or
 * `Module.fn`), else the receiver's type plus the bare name, else the bare
 * name of a qualified call. */
static const AST *lookupCallee(const AST *call, const AetherTypeEnv *env);

/* ------------------------------------------------------------------ */
/* Builtin return types                                                */
/* ------------------------------------------------------------------ */

/* Interim table (W7-07's role until the W7-16 manifest): canonical, lowered
 * call names, matched case-insensitively. Rows agree with builtins_json
 * wherever it states a return type (tools/check_type_oracle.py --builtins).
 * POLY: Real if any argument is Real, Int if all are Int. */
#define RET_POLY ((AetherTypeKind)100)
#define RET_RANDOM ((AetherTypeKind)101)
#define RET_TEXT_ARRAY ((AetherTypeKind)102)
typedef struct { const char *name; AetherTypeKind ret; const char *handle; } BuiltinRet;
static const BuiltinRet kBuiltinReturns[] = {
    /* Int */
    {"length", AETHER_T_INT, NULL}, {"ord", AETHER_T_INT, NULL}, {"pos", AETHER_T_INT, NULL},
    {"round", AETHER_T_INT, NULL}, {"trunc", AETHER_T_INT, NULL}, {"floor", AETHER_T_INT, NULL},
    {"ceil", AETHER_T_INT, NULL}, {"parse_int", AETHER_T_INT, NULL}, {"int", AETHER_T_INT, NULL},
    {"toint", AETHER_T_INT, NULL}, {"getenvint", AETHER_T_INT, NULL},
    {"paramcount", AETHER_T_INT, NULL}, {"yyjsongetlength", AETHER_T_INT, NULL},
    {"yyjsongetint", AETHER_T_INT, NULL}, {"httpsession", AETHER_T_INT, NULL},
    {"httprequest", AETHER_T_INT, NULL}, {"waitforthread", AETHER_T_INT, NULL},
    {"thread_spawn_named", AETHER_T_INT, NULL}, {"thread_pool_submit", AETHER_T_INT, NULL},
    {"thread_lookup", AETHER_T_INT, NULL}, {"thread_get_status", AETHER_T_INT, NULL},
    {"threadpoolsubmit", AETHER_T_INT, NULL}, {"socketcreate", AETHER_T_INT, NULL},
    {"socketaccept", AETHER_T_INT, NULL}, {"sizeof", AETHER_T_INT, NULL},
    /* Not listed: thread_get_result / threadgetresult. builtins_json says Int,
     * but tests/task_deny_pass and thread_pool_deny_pass bind it to Text (the
     * dnslookup worker's payload); until the registry settles it, unknown. */
    /* Real */
    {"sqrt", AETHER_T_REAL, NULL}, {"exp", AETHER_T_REAL, NULL}, {"ln", AETHER_T_REAL, NULL},
    {"log10", AETHER_T_REAL, NULL}, {"sin", AETHER_T_REAL, NULL}, {"cos", AETHER_T_REAL, NULL},
    {"tan", AETHER_T_REAL, NULL}, {"arctan", AETHER_T_REAL, NULL}, {"arcsin", AETHER_T_REAL, NULL},
    {"arccos", AETHER_T_REAL, NULL}, {"atan2", AETHER_T_REAL, NULL}, {"arctan2", AETHER_T_REAL, NULL},
    {"cotan", AETHER_T_REAL, NULL}, {"sinh", AETHER_T_REAL, NULL}, {"cosh", AETHER_T_REAL, NULL},
    {"tanh", AETHER_T_REAL, NULL}, {"pow", AETHER_T_REAL, NULL}, {"power", AETHER_T_REAL, NULL},
    {"real", AETHER_T_REAL, NULL}, {"double", AETHER_T_REAL, NULL}, {"todouble", AETHER_T_REAL, NULL},
    {"float", AETHER_T_REAL, NULL}, {"tofloat", AETHER_T_REAL, NULL},
    {"parse_float", AETHER_T_REAL, NULL}, {"yyjsongetnumber", AETHER_T_REAL, NULL},
    {"realtimeclock", AETHER_T_REAL, NULL},
    /* Text */
    {"inttostr", AETHER_T_TEXT, NULL}, {"itoa", AETHER_T_TEXT, NULL}, {"copy", AETHER_T_TEXT, NULL},
    {"trim", AETHER_T_TEXT, NULL}, {"formatfloat", AETHER_T_TEXT, NULL},
    {"realtostr", AETHER_T_TEXT, NULL}, {"stringofchar", AETHER_T_TEXT, NULL},
    {"getenv", AETHER_T_TEXT, NULL}, {"paramstr", AETHER_T_TEXT, NULL},
    {"getcurrentdir", AETHER_T_TEXT, NULL}, {"mstreambuffer", AETHER_T_TEXT, NULL},
    {"yyjsongetstring", AETHER_T_TEXT, NULL}, {"yyjsongettype", AETHER_T_TEXT, NULL},
    {"aetherbuiltinsjson", AETHER_T_TEXT, NULL}, {"aetherbuiltininfo", AETHER_T_TEXT, NULL},
    {"openaichatcompletions", AETHER_T_TEXT, NULL}, {"threadstatsjson", AETHER_T_TEXT, NULL},
    {"httplasterror", AETHER_T_TEXT, NULL}, {"httpgetheader", AETHER_T_TEXT, NULL},
    {"httpgetlastheaders", AETHER_T_TEXT, NULL},
    /* Char */
    {"chr", AETHER_T_CHAR, NULL}, {"upcase", AETHER_T_CHAR, NULL}, {"readkey", AETHER_T_CHAR, NULL},
    /* Bool */
    {"hasextbuiltin", AETHER_T_BOOL, NULL}, {"yyjsonhaskey", AETHER_T_BOOL, NULL},
    {"yyjsonhasindex", AETHER_T_BOOL, NULL}, {"yyjsongetbool", AETHER_T_BOOL, NULL},
    {"yyjsonisnull", AETHER_T_BOOL, NULL}, {"fileexists", AETHER_T_BOOL, NULL},
    {"eof", AETHER_T_BOOL, NULL}, {"parse_bool", AETHER_T_BOOL, NULL}, {"odd", AETHER_T_BOOL, NULL},
    {"httpisdone", AETHER_T_BOOL, NULL},
    /* Handles */
    {"yyjsonread", AETHER_T_HANDLE, "ToonDoc"}, {"yyjsonreadfile", AETHER_T_HANDLE, "ToonDoc"},
    {"yyjsongetroot", AETHER_T_HANDLE, "ToonNode"}, {"yyjsongetkey", AETHER_T_HANDLE, "ToonNode"},
    {"yyjsongetindex", AETHER_T_HANDLE, "ToonNode"},
    {"mstreamcreate", AETHER_T_HANDLE, "MStream"}, {"mstreamfromstring", AETHER_T_HANDLE, "MStream"},
    {"socketreceive", AETHER_T_HANDLE, "MStream"},
    /* Text[] */
    {"split", RET_TEXT_ARRAY, NULL},
    /* Void */
    {"setlength", AETHER_T_VOID, NULL}, {"halt", AETHER_T_VOID, NULL},
    {"yyjsondocfree", AETHER_T_VOID, NULL}, {"yyjsonfreevalue", AETHER_T_VOID, NULL},
    {"randomize", AETHER_T_VOID, NULL}, {"mstreamfree", AETHER_T_VOID, NULL},
    {"delay", AETHER_T_VOID, NULL}, {"inc", AETHER_T_VOID, NULL}, {"dec", AETHER_T_VOID, NULL},
    /* Argument-typed */
    {"abs", RET_POLY, NULL}, {"sqr", RET_POLY, NULL}, {"min", RET_POLY, NULL},
    {"max", RET_POLY, NULL}, {"clamp", RET_POLY, NULL},
    {"random", RET_RANDOM, NULL},
};

static const BuiltinRet *findBuiltin(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(kBuiltinReturns) / sizeof(kBuiltinReturns[0]); i++) {
        if (strcasecmp(name, kBuiltinReturns[i].name) == 0) return &kBuiltinReturns[i];
    }
    return NULL;
}

/* Builtin parameters whose type is fixed, for argument uses. Returns the
 * kind or UNKNOWN. */
static AetherTypeKind builtinParamKind(const char *name, int index) {
    if (!name) return AETHER_T_UNKNOWN;
    if (!strcasecmp(name, "inttostr") || !strcasecmp(name, "itoa") || !strcasecmp(name, "chr"))
        return index == 0 ? AETHER_T_INT : AETHER_T_UNKNOWN;
    if (!strcasecmp(name, "copy")) return index >= 1 ? AETHER_T_INT : AETHER_T_UNKNOWN;
    if (!strcasecmp(name, "stringofchar")) return index == 1 ? AETHER_T_INT : AETHER_T_UNKNOWN;
    if (!strcasecmp(name, "setlength")) return index >= 1 ? AETHER_T_INT : AETHER_T_UNKNOWN;
    if (!strcasecmp(name, "random")) return AETHER_T_INT;
    if (!strcasecmp(name, "delay")) return AETHER_T_INT;
    if (!strcasecmp(name, "yyjsongetindex") || !strcasecmp(name, "yyjsonhasindex"))
        return index == 1 ? AETHER_T_INT : AETHER_T_UNKNOWN;
    if (!strcasecmp(name, "formatfloat") || !strcasecmp(name, "realtostr"))
        return index == 0 ? AETHER_T_REAL : AETHER_T_INT;
    if (!strcasecmp(name, "sqrt") || !strcasecmp(name, "exp") || !strcasecmp(name, "ln") ||
        !strcasecmp(name, "sin") || !strcasecmp(name, "cos") || !strcasecmp(name, "tan") ||
        !strcasecmp(name, "arctan") || !strcasecmp(name, "log10") || !strcasecmp(name, "pow") ||
        !strcasecmp(name, "power") || !strcasecmp(name, "round") || !strcasecmp(name, "trunc") ||
        !strcasecmp(name, "floor") || !strcasecmp(name, "ceil"))
        return AETHER_T_REAL;
    return AETHER_T_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; AetherType t; } Binding;

struct AetherTypeEnv {
    Binding *items;
    int count, cap;
    int *marks;
    int depth, markCap;
    AetherType ret;        /* current function's return type, or UNKNOWN */
    const char *selfClass; /* current method's record, or NULL */
    const AST *function;   /* current FUNCTION_DECL / PROCEDURE_DECL */
};

static void envBind(AetherTypeEnv *env, const char *name, AetherType t) {
    if (!name) return;
    if (env->count == env->cap) {
        int cap = env->cap ? env->cap * 2 : 64;
        Binding *g = (Binding *)realloc(env->items, (size_t)cap * sizeof(Binding));
        if (!g) return;
        env->items = g;
        env->cap = cap;
    }
    env->items[env->count].name = name;
    env->items[env->count].t = t;
    env->count++;
}

static void envPush(AetherTypeEnv *env) {
    if (env->depth == env->markCap) {
        int cap = env->markCap ? env->markCap * 2 : 32;
        int *g = (int *)realloc(env->marks, (size_t)cap * sizeof(int));
        if (!g) return;
        env->marks = g;
        env->markCap = cap;
    }
    env->marks[env->depth++] = env->count;
}

static void envPop(AetherTypeEnv *env) {
    if (env->depth > 0) env->count = env->marks[--env->depth];
}

static int envFind(const AetherTypeEnv *env, const char *name, AetherType *out) {
    if (!name) return 0;
    for (int i = env->count - 1; i >= 0; i--) {
        if (strcasecmp(env->items[i].name, name) == 0) {
            if (out) *out = env->items[i].t;
            return 1;
        }
    }
    {
        const AST *g = indexGet(&g_globals, name);
        if (g) {
            if (out) {
                if (g->type == AST_CONST_DECL && !g->right && g->left)
                    *out = aetherTypeOf(g->left, env);
                else
                    *out = declTypeOf(g);
            }
            return 1;
        }
    }
    return 0;
}

void aetherTypeEnvRebind(const AetherTypeEnv *cenv, const char *name, AetherType t) {
    AetherTypeEnv *env = (AetherTypeEnv *)cenv;
    if (!env || !name) return;
    for (int i = env->count - 1; i >= 0; i--) {
        if (strcasecmp(env->items[i].name, name) == 0) {
            env->items[i].t = t;
            return;
        }
    }
}

AetherType aetherTypeEnvReturnType(const AetherTypeEnv *env) {
    return env ? env->ret : mk(AETHER_T_UNKNOWN);
}

/* ------------------------------------------------------------------ */
/* The oracle                                                          */
/* ------------------------------------------------------------------ */

static const char *callName(const AST *call) {
    return (call && call->token) ? call->token->value : NULL;
}

static const AST *lookupCallee(const AST *call, const AetherTypeEnv *env) {
    const char *name = callName(call);
    if (!name) return NULL;
    const AST *fn = indexGet(&g_functions, name);
    if (fn) return fn;
    const char *dot = strrchr(name, '.');
    if (call->left) {
        AetherType rt = aetherTypeOf(call->left, env);
        if (rt.kind == AETHER_T_RECORD && rt.name) {
            char q[256];
            snprintf(q, sizeof(q), "%s.%s", rt.name, dot ? dot + 1 : name);
            fn = indexGet(&g_functions, q);
            if (fn) return fn;
        }
    }
    if (dot && dot[1]) return indexGet(&g_functions, dot + 1);
    return NULL;
}

static AetherType arithmetic(AetherType l, AetherType r) {
    if (l.kind == AETHER_T_REAL && isNumeric(r)) return mk(AETHER_T_REAL);
    if (r.kind == AETHER_T_REAL && isNumeric(l)) return mk(AETHER_T_REAL);
    if (l.kind == AETHER_T_INT && r.kind == AETHER_T_INT) return mk(AETHER_T_INT);
    return mk(AETHER_T_UNKNOWN);
}

static AetherType typeOfBinary(const AST *e, const AetherTypeEnv *env) {
    TokenType tt = e->token ? e->token->type : TOKEN_UNKNOWN;
    const char *lex = (e->token && e->token->value) ? e->token->value : "";
    AetherType l = aetherTypeOf(e->left, env);
    AetherType r = aetherTypeOf(e->right, env);
    switch (tt) {
        case TOKEN_PLUS:
            if (isTextual(l) || isTextual(r)) return mk(AETHER_T_TEXT);
            if (l.kind == AETHER_T_ARRAY) return (l.elem != AETHER_T_UNKNOWN || r.kind != AETHER_T_ARRAY) ? l : r;
            if (r.kind == AETHER_T_ARRAY) return r;
            return arithmetic(l, r);
        case TOKEN_MINUS:
        case TOKEN_MUL:
            return arithmetic(l, r);
        case TOKEN_SLASH:
            /* `/` is Real-valued today (ast_parser.c parseMul forceReal; D4 open). */
            return (isNumeric(l) && isNumeric(r)) ? mk(AETHER_T_REAL) : mk(AETHER_T_UNKNOWN);
        case TOKEN_INT_DIV:
        case TOKEN_MOD:
        case TOKEN_SHL:
        case TOKEN_SHR:
            return (l.kind == AETHER_T_INT && r.kind == AETHER_T_INT) ? mk(AETHER_T_INT)
                                                                   : mk(AETHER_T_UNKNOWN);
        case TOKEN_EQUAL: case TOKEN_NOT_EQUAL: case TOKEN_LESS: case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER: case TOKEN_GREATER_EQUAL: case TOKEN_IN: case TOKEN_IS:
            return mk(AETHER_T_BOOL);
        case TOKEN_AND:
        case TOKEN_OR:
        case TOKEN_XOR:
            if (strcmp(lex, "&&") == 0 || strcmp(lex, "||") == 0) return mk(AETHER_T_BOOL);
            if (l.kind == AETHER_T_BOOL && r.kind == AETHER_T_BOOL) return mk(AETHER_T_BOOL);
            if (l.kind == AETHER_T_INT && r.kind == AETHER_T_INT) return mk(AETHER_T_INT);
            return mk(AETHER_T_UNKNOWN);
        default:
            return mk(AETHER_T_UNKNOWN);
    }
}

static AetherType typeOfCall(const AST *e, const AetherTypeEnv *env) {
    const AST *fn = lookupCallee(e, env);
    if (fn) return functionReturnType(fn);
    const BuiltinRet *b = findBuiltin(callName(e));
    if (!b) return mk(AETHER_T_UNKNOWN);
    if (b->ret == RET_TEXT_ARRAY) {
        AetherType t = mk(AETHER_T_ARRAY);
        t.rank = 1;
        t.elem = AETHER_T_TEXT;
        return t;
    }
    if (b->ret == RET_RANDOM)
        return e->child_count == 0 ? mk(AETHER_T_REAL) : mk(AETHER_T_INT);
    if (b->ret == RET_POLY) {
        int anyReal = 0, allInt = e->child_count > 0;
        for (int i = 0; i < e->child_count; i++) {
            AetherType a = aetherTypeOf(e->children[i], env);
            if (a.kind == AETHER_T_REAL) anyReal = 1;
            if (a.kind != AETHER_T_INT) allInt = 0;
        }
        if (anyReal) return mk(AETHER_T_REAL);
        return allInt ? mk(AETHER_T_INT) : mk(AETHER_T_UNKNOWN);
    }
    if (b->ret == AETHER_T_HANDLE) return mkNamed(AETHER_T_HANDLE, b->handle);
    return mk(b->ret);
}

AetherType aetherTypeOf(const AST *e, const AetherTypeEnv *env) {
    if (!e) return mk(AETHER_T_UNKNOWN);
    switch (e->type) {
        case AST_NUMBER:
            if (e->token && e->token->type == TOKEN_REAL_CONST) return mk(AETHER_T_REAL);
            if (e->token && (e->token->type == TOKEN_INTEGER_CONST || e->token->type == TOKEN_HEX_CONST))
                return mk(AETHER_T_INT);
            return fromVarType(e->var_type);
        case AST_STRING:
            return (e->var_type == TYPE_CHAR) ? mk(AETHER_T_CHAR) : mk(AETHER_T_TEXT);
        case AST_BOOLEAN:
            return mk(AETHER_T_BOOL);
        case AST_NIL:
            return mk(AETHER_T_NIL);
        case AST_VARIABLE: {
            const char *name = e->token ? e->token->value : NULL;
            AetherType t;
            if (name && env && envFind(env, name, &t)) return t;
            if (name && env && env->selfClass && strcasecmp(name, "myself") == 0)
                return mkNamed(AETHER_T_RECORD, env->selfClass);
            return mk(AETHER_T_UNKNOWN);
        }
        case AST_BINARY_OP:
            return typeOfBinary(e, env);
        case AST_UNARY_OP: {
            AetherType t = aetherTypeOf(e->left, env);
            if (e->token && e->token->type == TOKEN_NOT)
                return (t.kind == AETHER_T_BOOL || t.kind == AETHER_T_INT) ? t : mk(AETHER_T_UNKNOWN);
            return isNumeric(t) ? t : mk(AETHER_T_UNKNOWN);
        }
        case AST_TERNARY: {
            AetherType a = aetherTypeOf(e->right, env);
            AetherType b = aetherTypeOf(e->extra, env);
            if (a.kind == b.kind && a.kind != AETHER_T_RECORD && a.kind != AETHER_T_ARRAY) return a;
            if (isNumeric(a) && isNumeric(b)) return mk(AETHER_T_REAL);
            if (isTextual(a) && isTextual(b)) return mk(AETHER_T_TEXT);
            if (a.kind == AETHER_T_NIL) return b;
            if (b.kind == AETHER_T_NIL) return a;
            if (aetherTypeAgreement(a, b) == AETHER_AGREE_EXACT) return a;
            return mk(AETHER_T_UNKNOWN);
        }
        case AST_PROCEDURE_CALL:
            return typeOfCall(e, env);
        case AST_WRITE:
        case AST_WRITELN:
            return mk(AETHER_T_VOID);
        case AST_ARRAY_ACCESS: {
            AetherType base = aetherTypeOf(e->left, env);
            int k = e->child_count > 0 ? e->child_count : 1;
            if (base.kind == AETHER_T_ARRAY) {
                if (base.rank > k) {
                    base.rank -= k;
                    return base;
                }
                if (base.rank == k) {
                    AetherType el = mk(base.elem);
                    el.name = base.name;
                    return el;
                }
                return mk(AETHER_T_UNKNOWN);
            }
            if (isTextual(base) && k == 1) return mk(AETHER_T_CHAR);
            return mk(AETHER_T_UNKNOWN);
        }
        case AST_FIELD_ACCESS: {
            AetherType rt = aetherTypeOf(e->left, env);
            const char *field = e->token ? e->token->value : NULL;
            if (rt.kind == AETHER_T_RECORD && rt.name) return recordFieldType(rt.name, field);
            if ((rt.kind == AETHER_T_ARRAY || isTextual(rt)) && field && strcasecmp(field, "length") == 0)
                return mk(AETHER_T_INT);
            return mk(AETHER_T_UNKNOWN);
        }
        case AST_ARRAY_LITERAL: {
            AetherType t = mk(AETHER_T_ARRAY);
            t.rank = 1;
            if (e->child_count == 0) return t;
            AetherType el = aetherTypeOf(e->children[0], env);
            for (int i = 1; i < e->child_count && el.kind == AETHER_T_INT; i++) {
                AetherType o = aetherTypeOf(e->children[i], env);
                if (o.kind == AETHER_T_REAL) el = o;
            }
            if (el.kind == AETHER_T_ARRAY) {
                el.rank += 1;
                return el;
            }
            t.elem = el.kind;
            t.name = el.name;
            return t;
        }
        case AST_NEW:
            return (e->token && e->token->value) ? mkNamed(AETHER_T_RECORD, e->token->value)
                                                 : mk(AETHER_T_RECORD);
        case AST_FORMATTED_EXPR:
            return mk(AETHER_T_TEXT);
        case AST_ASSIGN:
            return aetherTypeOf(e->left, env);
        default:
            return mk(AETHER_T_UNKNOWN);
    }
}

/* ------------------------------------------------------------------ */
/* The walker                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    AetherTypeEnv env;
    const AetherTypedVisitor *v;
} Walker;

static AetherUse useOf(AetherUseKind k) {
    AetherUse u;
    memset(&u, 0, sizeof(u));
    u.kind = k;
    return u;
}

static AetherUse sinkUse(AetherType want, const char *label) {
    AetherUse u = useOf(AETHER_USE_SINK);
    u.want = want;
    u.sinkLabel = label;
    return u;
}

static void walkExpr(Walker *w, AST *e, AetherUse use);
static void walkStmt(Walker *w, AST *s);

static void emitSink(Walker *w, const AST *site, AST *value, AetherUse use) {
    if (w->v->onSink && value && use.kind == AETHER_USE_SINK)
        w->v->onSink(w->v->ctx, site, value, &use, &w->env);
}

static void walkValue(Walker *w, const AST *site, AST *value, AetherUse use) {
    if (!value) return;
    emitSink(w, site, value, use);
    walkExpr(w, value, use);
}

static int isWriteCall(const AST *e) {
    return e && (e->type == AST_WRITE || e->type == AST_WRITELN);
}

static void walkCallArgs(Walker *w, AST *e) {
    const AST *fn = (e->type == AST_PROCEDURE_CALL) ? lookupCallee(e, &w->env) : NULL;
    AetherType params[64];
    int np = fn ? functionParamTypes(fn, params, 64) : 0;
    const char *name = callName(e);
    if (e->left && !(e->child_count > 0 && e->children[0] == e->left))
        walkExpr(w, e->left, useOf(AETHER_USE_OTHER));
    for (int i = 0; i < e->child_count; i++) {
        AST *a = e->children[i];
        if (!a) continue;
        if (isWriteCall(e)) {
            walkExpr(w, a, useOf(AETHER_USE_PRINT));
        } else if (fn) {
            if (i < np) walkValue(w, e, a, sinkUse(params[i], "arg"));
            else walkExpr(w, a, useOf(AETHER_USE_CALL_ARG));
        } else {
            AetherTypeKind k = builtinParamKind(name, i);
            if (k != AETHER_T_UNKNOWN) walkValue(w, e, a, sinkUse(mk(k), "builtin-arg"));
            else walkExpr(w, a, useOf(AETHER_USE_BUILTIN_ARG));
        }
    }
}

static void walkExprInner(Walker *w, AST *e, AetherUse use);

static void walkExpr(Walker *w, AST *e, AetherUse use) {
    if (!e) return;
    if (w->v->onExpr) w->v->onExpr(w->v->ctx, e, &use, &w->env);
    walkExprInner(w, e, use);
    if (w->v->onExprPost) w->v->onExprPost(w->v->ctx, e, &use, &w->env);
}

static void walkExprInner(Walker *w, AST *e, AetherUse use) {
    switch (e->type) {
        case AST_BINARY_OP: {
            TokenType tt = e->token ? e->token->type : TOKEN_UNKNOWN;
            AetherUse child;
            switch (tt) {
                case TOKEN_PLUS:
                case TOKEN_MINUS: {
                    AetherType self = aetherTypeOf(e, &w->env);
                    if (tt == TOKEN_PLUS && self.kind == AETHER_T_TEXT) child = useOf(AETHER_USE_PRINT);
                    else if (tt == TOKEN_PLUS && self.kind == AETHER_T_ARRAY) child = useOf(AETHER_USE_OTHER);
                    else child = use; /* a +/- chain carries its parent's use */
                    break;
                }
                case TOKEN_MUL: child = useOf(AETHER_USE_MUL); break;
                case TOKEN_SLASH: child = useOf(AETHER_USE_DIV); break;
                case TOKEN_INT_DIV: child = useOf(AETHER_USE_INT_DIV); break;
                case TOKEN_MOD: child = useOf(AETHER_USE_MOD); break;
                case TOKEN_EQUAL: case TOKEN_NOT_EQUAL: case TOKEN_LESS: case TOKEN_LESS_EQUAL:
                case TOKEN_GREATER: case TOKEN_GREATER_EQUAL:
                    child = useOf(AETHER_USE_COMPARE);
                    break;
                default: child = useOf(AETHER_USE_OTHER); break;
            }
            /* The upper bound of a lowered `loop i in a..b` is the right
             * operand of the loop's `i < b` / `i > b` test. */
            if (child.kind == AETHER_USE_COMPARE && aetherAstIsRangeBound(e->right)) {
                walkExpr(w, e->left, child);
                walkExpr(w, e->right, useOf(AETHER_USE_RANGE_BOUND));
                return;
            }
            walkExpr(w, e->left, child);
            walkExpr(w, e->right, child);
            return;
        }
        case AST_UNARY_OP:
            if (e->token && e->token->type == TOKEN_NOT) walkExpr(w, e->left, useOf(AETHER_USE_OTHER));
            else walkExpr(w, e->left, use);
            return;
        case AST_TERNARY:
            walkExpr(w, e->left, useOf(AETHER_USE_CONDITION));
            walkExpr(w, e->right, use);
            walkExpr(w, e->extra, use);
            return;
        case AST_PROCEDURE_CALL:
        case AST_WRITE:
        case AST_WRITELN:
            walkCallArgs(w, e);
            return;
        case AST_ARRAY_ACCESS:
            walkExpr(w, e->left, useOf(AETHER_USE_OTHER));
            for (int i = 0; i < e->child_count; i++) walkExpr(w, e->children[i], useOf(AETHER_USE_INDEX));
            return;
        case AST_FIELD_ACCESS:
            walkExpr(w, e->left, useOf(AETHER_USE_OTHER));
            return; /* right is the field name */
        case AST_ARRAY_LITERAL: {
            AetherUse child = useOf(AETHER_USE_OTHER);
            if (use.kind == AETHER_USE_SINK && use.want.kind == AETHER_T_ARRAY) {
                AetherType el = use.want;
                if (el.rank > 1) el.rank--;
                else {
                    AetherType s = mk(use.want.elem);
                    s.name = use.want.name;
                    el = s;
                }
                if (el.kind != AETHER_T_UNKNOWN) child = sinkUse(el, "element");
            }
            for (int i = 0; i < e->child_count; i++) walkValue(w, e, e->children[i], child);
            return;
        }
        case AST_NEW:
            /* `new T { f: v, ... }`: extra holds ASSIGN(':') field initializers. */
            if (e->extra) {
                const char *rec = e->token ? e->token->value : NULL;
                for (int i = 0; i < e->extra->child_count; i++) {
                    AST *fi = e->extra->children[i];
                    if (fi && fi->type == AST_ASSIGN && fi->left && fi->left->token) {
                        AetherType ft = recordFieldType(rec, fi->left->token->value);
                        walkValue(w, fi, fi->right,
                                  ft.kind != AETHER_T_UNKNOWN ? sinkUse(ft, "field-init")
                                                              : useOf(AETHER_USE_OTHER));
                    } else {
                        walkStmt(w, fi);
                    }
                }
            }
            for (int i = 0; i < e->child_count; i++) walkExpr(w, e->children[i], useOf(AETHER_USE_CALL_ARG));
            return;
        case AST_FORMATTED_EXPR:
            walkExpr(w, e->left, useOf(AETHER_USE_PRINT));
            return;
        case AST_ASSIGN:
            walkStmt(w, e);
            return;
        default:
            walkExpr(w, e->left, useOf(AETHER_USE_OTHER));
            walkExpr(w, e->right, useOf(AETHER_USE_OTHER));
            walkExpr(w, e->extra, useOf(AETHER_USE_OTHER));
            for (int i = 0; i < e->child_count; i++) walkExpr(w, e->children[i], useOf(AETHER_USE_OTHER));
            return;
    }
}

static void bindDecl(Walker *w, AST *d, AetherType t) {
    for (int i = 0; i < d->child_count; i++) {
        AST *v = d->children[i];
        if (v && v->type == AST_VARIABLE && v->token && v->token->value)
            envBind(&w->env, v->token->value, t);
    }
}

static void walkFunction(Walker *w, AST *fn) {
    if (!fn->right && !fn->extra) return; /* forward declaration */
    AetherTypeEnv saved = w->env;
    AetherType ret = functionReturnType(fn);
    const char *name = fn->token ? fn->token->value : NULL;
    const char *dot = name ? strrchr(name, '.') : NULL;
    char selfBuf[256];
    envPush(&w->env);
    w->env.ret = ret;
    w->env.function = fn;
    w->env.selfClass = NULL;
    for (int i = 0; i < fn->child_count; i++) {
        AST *p = fn->children[i];
        if (p && p->type == AST_VAR_DECL) bindDecl(w, p, declTypeOf(p));
    }
    /* The @post lowering stores the value in Pascal's implicit `result`. */
    if (ret.kind != AETHER_T_VOID && ret.kind != AETHER_T_UNKNOWN) envBind(&w->env, "result", ret);
    if (dot) {
        size_t n = (size_t)(dot - name);
        if (n < sizeof(selfBuf)) {
            memcpy(selfBuf, name, n);
            selfBuf[n] = '\0';
            if (indexGet(&g_records, selfBuf)) {
                /* The receiver's name is stable for the walk: index it. */
                const AST *rec = indexGet(&g_records, selfBuf);
                for (int i = 0; i < g_records.count; i++)
                    if (g_records.items[i].node == rec) w->env.selfClass = g_records.items[i].name;
            }
        }
    }
    AST *body = (fn->type == AST_FUNCTION_DECL) ? fn->extra : fn->right;
    if (!body) body = fn->extra ? fn->extra : fn->right;
    walkStmt(w, body);
    envPop(&w->env);
    w->env.ret = saved.ret;
    w->env.function = saved.function;
    w->env.selfClass = saved.selfClass;
}

static void walkStmt(Walker *w, AST *s) {
    if (!s) return;
    switch (s->type) {
        case AST_PROGRAM:
            walkStmt(w, s->right);
            walkStmt(w, s->left);
            return;
        case AST_BLOCK:
            for (int i = 0; i < s->child_count; i++) walkStmt(w, s->children[i]);
            return;
        case AST_COMPOUND:
            envPush(&w->env);
            for (int i = 0; i < s->child_count; i++) walkStmt(w, s->children[i]);
            envPop(&w->env);
            return;
        case AST_VAR_DECL: {
            AetherType t = declTypeOf(s);
            if (s->left) {
                if (aetherAstDeclHasExplicitType(s)) walkValue(w, s, s->left, sinkUse(t, "let"));
                else walkExpr(w, s->left, useOf(AETHER_USE_INFERRED));
            }
            bindDecl(w, s, t);
            if (w->v->onDecl) w->v->onDecl(w->v->ctx, s, &w->env);
            return;
        }
        case AST_CONST_DECL: {
            if (s->left) {
                if (s->right) walkValue(w, s, s->left, sinkUse(declTypeOf(s), "const"));
                else walkExpr(w, s->left, useOf(AETHER_USE_INFERRED));
            }
            if (s->token && s->token->value)
                envBind(&w->env, s->token->value,
                        s->right ? declTypeOf(s) : aetherTypeOf(s->left, &w->env));
            return;
        }
        case AST_TYPE_DECL:
            if (s->left && s->left->type == AST_RECORD_TYPE) {
                for (int i = 0; i < s->left->child_count; i++) {
                    AST *f = s->left->children[i];
                    if (f && f->type == AST_VAR_DECL && f->left)
                        walkValue(w, f, f->left, sinkUse(declTypeOf(f), "field-default"));
                }
            }
            return;
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            walkFunction(w, s);
            return;
        case AST_ASSIGN: {
            AetherType lt = aetherTypeOf(s->left, &w->env);
            TokenType op = s->token ? s->token->type : TOKEN_ASSIGN;
            /* lvalue: only its index expressions are values */
            if (s->left && s->left->type == AST_ARRAY_ACCESS) {
                walkExpr(w, s->left->left, useOf(AETHER_USE_OTHER));
                for (int i = 0; i < s->left->child_count; i++)
                    walkExpr(w, s->left->children[i], useOf(AETHER_USE_INDEX));
            } else if (s->left && s->left->type == AST_FIELD_ACCESS) {
                walkExpr(w, s->left->left, useOf(AETHER_USE_OTHER));
            }
            if (op == TOKEN_ASSIGN && lt.kind != AETHER_T_UNKNOWN)
                walkValue(w, s, s->right, sinkUse(lt, "assign"));
            else if (op == TOKEN_ASSIGN)
                walkExpr(w, s->right, useOf(AETHER_USE_OTHER));
            else if (op == TOKEN_MUL)
                walkExpr(w, s->right, useOf(AETHER_USE_MUL));
            else if (op == TOKEN_SLASH)
                walkExpr(w, s->right, useOf(AETHER_USE_DIV));
            else if (op == TOKEN_MOD)
                walkExpr(w, s->right, useOf(AETHER_USE_MOD));
            else
                walkExpr(w, s->right, lt.kind != AETHER_T_UNKNOWN ? sinkUse(lt, "compound")
                                                                  : useOf(AETHER_USE_OTHER));
            return;
        }
        case AST_RETURN:
            if (s->left) {
                if (w->env.ret.kind != AETHER_T_UNKNOWN && w->env.ret.kind != AETHER_T_VOID)
                    walkValue(w, s, s->left, sinkUse(w->env.ret, "ret"));
                else
                    walkExpr(w, s->left, useOf(AETHER_USE_OTHER));
            }
            return;
        case AST_IF:
            walkExpr(w, s->left, useOf(AETHER_USE_CONDITION));
            walkStmt(w, s->right);
            walkStmt(w, s->extra);
            return;
        case AST_WHILE:
        case AST_REPEAT:
            walkExpr(w, s->left, useOf(AETHER_USE_CONDITION));
            walkStmt(w, s->right);
            for (int i = 0; i < s->child_count; i++) walkStmt(w, s->children[i]);
            return;
        case AST_EXPR_STMT:
            walkExpr(w, s->left, useOf(AETHER_USE_NONE));
            for (int i = 0; i < s->child_count; i++) walkExpr(w, s->children[i], useOf(AETHER_USE_NONE));
            return;
        case AST_PROCEDURE_CALL:
        case AST_WRITE:
        case AST_WRITELN:
        case AST_BINARY_OP:
        case AST_UNARY_OP:
        case AST_VARIABLE:
        case AST_NUMBER:
        case AST_STRING:
        case AST_ARRAY_ACCESS:
        case AST_FIELD_ACCESS:
        case AST_TERNARY:
            walkExpr(w, s, useOf(AETHER_USE_NONE));
            return;
        default:
            walkStmt(w, s->left);
            walkStmt(w, s->right);
            walkStmt(w, s->extra);
            for (int i = 0; i < s->child_count; i++) walkStmt(w, s->children[i]);
            return;
    }
}

void aetherTypedWalk(AST *root, const AetherTypedVisitor *visitor) {
    Walker w;
    memset(&w, 0, sizeof(w));
    w.v = visitor;
    indexFree(&g_records);
    indexFree(&g_functions);
    indexFree(&g_globals);
    int modules = aetherGetLoadedModuleCount();
    for (int i = 0; i < modules; i++) indexProgram(aetherGetModuleAST(i), 1);
    indexProgram(root, 1);
    for (int i = 0; i < modules; i++) {
        AST *m = aetherGetModuleAST(i);
        if (m && m != root) walkStmt(&w, m);
    }
    walkStmt(&w, root);
    free(w.env.items);
    free(w.env.marks);
    indexFree(&g_records);
    indexFree(&g_functions);
    indexFree(&g_globals);
}

/* ------------------------------------------------------------------ */
/* Rules hosted on the oracle (release L1)                             */
/* ------------------------------------------------------------------ */

/* The first rules on the typed pass. Each judges a POSITIVE static type only:
 * an operand the oracle cannot type is never reported. They run after rea's
 * semantic pass and the AETHER_EXPERIMENT arms (an arm may rewrite `/`). */

typedef struct {
    int reported;
} RuleCtx;

static int nodeLine(const AST *n);

static void ruleReport(RuleCtx *c, const AST *at, const char *detail) {
    int line = nodeLine(at);
    aetherSemanticReportCoded("TYPE-001", "type", line > 0 ? line : 1, detail, 1);
    c->reported++;
}

/* int(x) / real(x) and their cast spellings, one argument. */
static const char *numberCastName(const AST *e) {
    const char *n = callName(e);
    if (!n || e->type != AST_PROCEDURE_CALL || e->child_count != 1) return NULL;
    if (!strcasecmp(n, "int") || !strcasecmp(n, "toint")) return "int";
    if (!strcasecmp(n, "real") || !strcasecmp(n, "double") || !strcasecmp(n, "todouble") ||
        !strcasecmp(n, "float") || !strcasecmp(n, "tofloat"))
        return "real";
    return NULL;
}

/* W4-19: a statically Real value where only an Int works. `/` is always real
 * division, so `n / 10 % 10`, `int_to_text(a / b)` and `s[n / 2]` compiled and
 * then stopped with an uncoded VM error ("Operands for 'mod' must be
 * integers", "IntToStr requires an integer-compatible argument", "String/Char
 * index must be an integer"). A Real into an Int `let`/assign/ret/argument
 * keeps its D1 sink truncation (NARROW-001) and is not judged here. Off while
 * an AETHER_EXPERIMENT div arm owns `/`. */
static void reportRealInIntPosition(RuleCtx *c, const AST *at, const char *where) {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "this value is Real, and %s needs an Int: write `a div b` for an integer "
             "quotient (`/` is real division), or trunc(x).",
             where);
    ruleReport(c, at, detail);
}

static int realRulesOn(void) {
    return aetherExperiment()->div == AETHER_DIV_CURRENT;
}

static void ruleSink(void *vctx, const AST *site, const AST *value, const AetherUse *use,
                     const AetherTypeEnv *env) {
    RuleCtx *c = (RuleCtx *)vctx;
    if (!realRulesOn() || !use->sinkLabel || strcmp(use->sinkLabel, "builtin-arg") != 0 ||
        use->want.kind != AETHER_T_INT)
        return;
    const char *n = callName(site);
    const char *shown = NULL;
    if (n && (!strcasecmp(n, "inttostr") || !strcasecmp(n, "itoa"))) shown = "int_to_text";
    else if (n && !strcasecmp(n, "chr")) shown = "chr";
    else if (n && !strcasecmp(n, "copy")) shown = "copy";
    if (!shown || aetherTypeOf(value, env).kind != AETHER_T_REAL) return;
    char where[48];
    snprintf(where, sizeof(where), "%s()", shown);
    reportRealInIntPosition(c, value, where);
}

static void ruleExpr(void *vctx, AST *e, const AetherUse *use, const AetherTypeEnv *env) {
    RuleCtx *c = (RuleCtx *)vctx;
    if (realRulesOn() && (use->kind == AETHER_USE_MOD || use->kind == AETHER_USE_INT_DIV) &&
        aetherTypeOf(e, env).kind == AETHER_T_REAL) {
        reportRealInIntPosition(c, e, use->kind == AETHER_USE_MOD ? "`%`" : "`div`");
        return;
    }
    /* A Text index only: an ARRAY index truncates a Real today and a replayed
     * passing program relies on it (`xs[n / 2]` in a quick sort), so that one
     * waits for the D4 rule, under which an index is an Int context. */
    if (realRulesOn() && e->type == AST_ARRAY_ACCESS && e->child_count == 1 &&
        isTextual(aetherTypeOf(e->left, env)) &&
        aetherTypeOf(e->children[0], env).kind == AETHER_T_REAL) {
        reportRealInIntPosition(c, e->children[0], "a Text index");
        return;
    }
    if (realRulesOn() && e->type == AST_BINARY_OP && e->token &&
        (e->token->type == TOKEN_SHL || e->token->type == TOKEN_SHR) &&
        (aetherTypeOf(e->left, env).kind == AETHER_T_REAL ||
         aetherTypeOf(e->right, env).kind == AETHER_T_REAL)) {
        reportRealInIntPosition(c, e, e->token->type == TOKEN_SHL ? "`shl`" : "`shr`");
        return;
    }
    /* W8-15 (D14): an array printed the VM's `ARRAY(dims:1, ...)` header and
     * exited 0. There is no array output format for a model to guess. */
    if (isWriteCall(e)) {
        for (int i = 0; i < e->child_count; i++) {
            const AST *a = e->children[i];
            if (a && a->type == AST_FORMATTED_EXPR) a = a->left;
            if (!a || aetherTypeOf(a, env).kind != AETHER_T_ARRAY) continue;
            ruleReport(c, e, e->type == AST_WRITELN
                                 ? "println cannot print an array: loop over it and print each "
                                   "element (`loop x in xs { ... }`)."
                                 : "print cannot print an array: loop over it and print each "
                                   "element (`loop x in xs { ... }`).");
            break;
        }
        return;
    }
    /* W8-15 (D14): int(Text) was 0, int(s[i]) the code point ("7" -> 55),
     * real(Text) an uncoded VM error. */
    const char *cast = numberCastName(e);
    if (cast && isTextual(aetherTypeOf(e->children[0], env))) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "%s() converts numbers, not Text: for the number in a Text use %s, for a "
                 "character code use ord(c).",
                 cast, strcmp(cast, "int") == 0 ? "parse_int(t)" : "parse_float(t)");
        ruleReport(c, e, detail);
    }
}

void aetherTypedRules(AST *root) {
    if (!root) return;
    RuleCtx c;
    memset(&c, 0, sizeof(c));
    AetherTypedVisitor v;
    memset(&v, 0, sizeof(v));
    v.ctx = &c;
    v.onExpr = ruleExpr;
    v.onSink = ruleSink;
    aetherTypedWalk(root, &v);
}

/* ------------------------------------------------------------------ */
/* The dump (AETHER_DUMP_TYPES)                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *out;
    const char *path;
} DumpCtx;

static int nodeLine(const AST *n) {
    for (int depth = 0; n && depth < 8; depth++) {
        if (n->token && n->token->line > 0) return n->token->line;
        n = n->left ? n->left : (n->child_count > 0 ? n->children[0] : NULL);
    }
    return 0;
}

static void dumpSink(void *vctx, const AST *site, const AST *value, const AetherUse *use,
                     const AetherTypeEnv *env) {
    DumpCtx *d = (DumpCtx *)vctx;
    char want[96], have[96];
    AetherType t = aetherTypeOf(value, env);
    AetherAgreement a = aetherTypeAgreement(use->want, t);
    int line = nodeLine(value);
    if (line <= 0) line = nodeLine(site);
    fprintf(d->out, "%d\t%s\t%s\t%s\t%s\t%s\n", line, use->sinkLabel ? use->sinkLabel : "sink",
            value->type == AST_NEW ? "NEW" : astTypeToString(value->type), aetherTypeFormat(use->want, want, sizeof(want)),
            aetherTypeFormat(t, have, sizeof(have)), aetherAgreementName(a));
}

void aetherTypedPass(AST *root) {
    const char *target = aetherDumpTypesTarget();
    if (!target || !root) return;
    DumpCtx d;
    int toStdout = strcmp(target, "1") == 0;
    d.out = toStdout ? stdout : fopen(target, "w");
    d.path = aetherSemanticGetSourcePath();
    if (!d.out) {
        fprintf(stderr, "AETHER_DUMP_TYPES: cannot open '%s' for writing.\n", target);
        return;
    }
    fprintf(d.out, "# aether type oracle: line\tsink\texpr\tdeclared\toracle\tagreement\n");
    AetherTypedVisitor v;
    memset(&v, 0, sizeof(v));
    v.ctx = &d;
    v.onSink = dumpSink;
    aetherTypedWalk(root, &v);
    if (toStdout) {
        fflush(stdout);
        exit(pascal_semantic_error_count > 0 ? 1 : 0);
    }
    fclose(d.out);
}
