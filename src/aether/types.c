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

/* ------------------------------------------------------------------ */
/* Map types (D68): "Map[Text, T]" names, interned for the program     */
/* ------------------------------------------------------------------ */

static const char *internName(const char *n, size_t len) {
    static char **names = NULL;
    static int count = 0, cap = 0;
    for (int i = 0; i < count; i++)
        if (strlen(names[i]) == len && strncmp(names[i], n, len) == 0) return names[i];
    if (count == cap) {
        int ncap = cap ? cap * 2 : 16;
        char **g = (char **)realloc(names, (size_t)ncap * sizeof(char *));
        if (!g) return NULL;
        names = g;
        cap = ncap;
    }
    char *c = (char *)malloc(len + 1);
    if (!c) return NULL;
    memcpy(c, n, len);
    c[len] = '\0';
    names[count++] = c;
    return c;
}

static int isMapName(const char *n, size_t len) {
    return n && len > 5 && strncmp(n, "Map[", 4) == 0 && n[len - 1] == ']';
}

static AetherType scalarFromTypeName(const char *n, VarType fallback);

/* An Aether type from its spelling ("Int", "P[]", "Map[Text, Int[]]"). */
static AetherType typeFromAetherName(const char *n, size_t len) {
    if (!n || len == 0) return mk(AETHER_T_UNKNOWN);
    int rank = 0;
    while (len >= 2 && n[len - 2] == '[' && n[len - 1] == ']') {
        rank++;
        len -= 2;
    }
    AetherType base;
    if (isMapName(n, len)) {
        base = mkNamed(AETHER_T_MAP, internName(n, len));
    } else {
        char buf[128];
        if (len >= sizeof(buf)) return mk(AETHER_T_UNKNOWN);
        memcpy(buf, n, len);
        buf[len] = '\0';
        if (!strcmp(buf, "Int")) base = mk(AETHER_T_INT);
        else if (!strcmp(buf, "Real") || !strcmp(buf, "Float")) base = mk(AETHER_T_REAL);
        else if (!strcmp(buf, "Text") || !strcmp(buf, "String")) base = mk(AETHER_T_TEXT);
        else if (!strcmp(buf, "Bool")) base = mk(AETHER_T_BOOL);
        else if (!strcmp(buf, "MStream") || !strcmp(buf, "ToonDoc") || !strcmp(buf, "ToonNode"))
            base = mkNamed(AETHER_T_HANDLE, internName(buf, len));
        else base = mkNamed(AETHER_T_RECORD, internName(buf, len));
    }
    if (rank == 0) return base;
    AetherType t = mk(AETHER_T_ARRAY);
    t.rank = rank;
    t.elem = base.kind;
    t.name = base.name;
    return t;
}

/* The value type of a map type; UNKNOWN when the map's name is unknown. */
static AetherType mapValueType(AetherType m) {
    if (m.kind != AETHER_T_MAP || !m.name) return mk(AETHER_T_UNKNOWN);
    const char *n = m.name;
    size_t len = strlen(n);
    const char *end = n + len - 1;
    int depth = 0;
    const char *comma = NULL;
    for (const char *c = n + 4; c < end; c++) {
        if (*c == '[') depth++;
        else if (*c == ']') depth--;
        else if (*c == ',' && depth == 0) { comma = c; break; }
    }
    if (!comma) return mk(AETHER_T_UNKNOWN);
    const char *v = comma + 1;
    while (v < end && *v == ' ') v++;
    return typeFromAetherName(v, (size_t)(end - v));
}

static AetherType mapTypeOfValue(AetherType v) {
    char vn[160];
    if (v.kind == AETHER_T_UNKNOWN || v.kind == AETHER_T_VOID || v.kind == AETHER_T_NIL)
        return mk(AETHER_T_MAP);
    if (v.kind == AETHER_T_CHAR) v.kind = AETHER_T_TEXT;
    aetherTypeFormat(v, vn, sizeof(vn));
    char full[200];
    snprintf(full, sizeof(full), "Map[Text, %s]", vn);
    return mkNamed(AETHER_T_MAP, internName(full, strlen(full)));
}

/* Does a map type (or an array of them) contain a record anywhere? Such a
 * map has no printed form. */
static int typeHoldsRecord(AetherType t, int depth) {
    if (depth > 8) return 0;
    if (t.kind == AETHER_T_RECORD) return 1;
    if (t.kind == AETHER_T_ARRAY) {
        if (t.elem == AETHER_T_RECORD) return 1;
        if (t.elem == AETHER_T_MAP) return typeHoldsRecord(mkNamed(AETHER_T_MAP, t.name), depth + 1);
        return 0;
    }
    if (t.kind == AETHER_T_MAP) return typeHoldsRecord(mapValueType(t), depth + 1);
    return 0;
}
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
        case TYPE_MAP:
            return mk(AETHER_T_MAP);
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
            if (n->var_type == TYPE_MAP) {
                const char *tn = n->token ? n->token->value : NULL;
                return tn ? mkNamed(AETHER_T_MAP, internName(tn, strlen(tn))) : mk(AETHER_T_MAP);
            }
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
        case AETHER_T_MAP: return "Map";
        default: return "?";
    }
}

const char *aetherTypeFormat(AetherType t, char *buf, size_t n) {
    if (!buf || n == 0) return "";
    if (t.kind == AETHER_T_ARRAY) {
        const char *el = (t.elem == AETHER_T_RECORD || t.elem == AETHER_T_HANDLE ||
                          t.elem == AETHER_T_MAP) && t.name
                             ? t.name : kindName(t.elem);
        size_t used = (size_t)snprintf(buf, n, "%s", el);
        for (int i = 0; i < t.rank && used + 2 < n; i++) {
            buf[used++] = '[';
            buf[used++] = ']';
            buf[used] = '\0';
        }
        return buf;
    }
    if ((t.kind == AETHER_T_RECORD || t.kind == AETHER_T_HANDLE || t.kind == AETHER_T_MAP) &&
        t.name) {
        snprintf(buf, n, "%s", t.name);
        return buf;
    }
    snprintf(buf, n, "%s", kindName(t.kind));
    return buf;
}

/* Two map types agree when their value types do; an unnamed map (`{}`)
 * agrees with any. */
static AetherAgreement mapAgreement(const char *want, const char *have) {
    if (!want || !have) return AETHER_AGREE_COMPAT;
    if (strcmp(want, have) == 0) return AETHER_AGREE_EXACT;
    AetherAgreement a = aetherTypeAgreement(mapValueType(mkNamed(AETHER_T_MAP, want)),
                                            mapValueType(mkNamed(AETHER_T_MAP, have)));
    if (a == AETHER_AGREE_NARROW) return AETHER_AGREE_MISMATCH;
    return a;
}

AetherAgreement aetherTypeAgreement(AetherType want, AetherType have) {
    if (have.kind == AETHER_T_UNKNOWN || want.kind == AETHER_T_UNKNOWN) return AETHER_AGREE_UNKNOWN;
    if (want.kind == have.kind) {
        if (want.kind == AETHER_T_MAP) return mapAgreement(want.name, have.name);
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
                if (want.elem == AETHER_T_MAP) return mapAgreement(want.name, have.name);
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

/* How `field` names a field of record `recName`: 1 exactly, 2 only when case
 * is ignored (the backend's field lookup is exact, so that one fails there),
 * 0 not at all, -1 when the record is not indexed. `*declared` gets the
 * declared spelling for 2, and the field list (comma-separated) for 0. */
static int recordFieldMatch(const char *recName, const char *field, char *declared, size_t n) {
    const AST *rec = indexGet(&g_records, recName);
    if (!rec || !field) return -1;
    int found = 0;
    size_t len = 0;
    if (n) declared[0] = '\0';
    for (int i = 0; i < rec->child_count; i++) {
        const AST *f = rec->children[i];
        if (!f || f->type != AST_VAR_DECL) continue;
        for (int j = 0; j < f->child_count; j++) {
            const AST *v = f->children[j];
            if (!v || !v->token || !v->token->value) continue;
            const char *fname = v->token->value;
            if (strncmp(fname, "__", 2) == 0) continue; /* hidden __vtable */
            if (strcmp(fname, field) == 0) return 1;
            if (strcasecmp(fname, field) == 0) {
                snprintf(declared, n, "%s", fname);
                found = 2;
            }
            if (!found && len < n) {
                int w = snprintf(declared + len, n - len, "%s%s", len ? ", " : "", fname);
                if (w > 0) len += (size_t)w;
            }
        }
    }
    return found;
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
#define RET_ARG0 ((AetherTypeKind)103)  /* the first argument's type */
#define RET_ELEM0 ((AetherTypeKind)104) /* the first argument's element type */
#define RET_ARRAY_OF_ARG0 ((AetherTypeKind)105) /* `[v; n]`: an array of v's type */
typedef struct { const char *name; AetherTypeKind ret; const char *handle; } BuiltinRet;
static const BuiltinRet kBuiltinReturns[] = {
    /* Int */
    {"length", AETHER_T_INT, NULL}, {"ord", AETHER_T_INT, NULL}, {"pos", AETHER_T_INT, NULL},
    {"round", AETHER_T_INT, NULL}, {"trunc", AETHER_T_INT, NULL}, {"floor", AETHER_T_INT, NULL},
    {"ceil", AETHER_T_INT, NULL}, {"parse_int", AETHER_T_INT, NULL}, {"int", AETHER_T_INT, NULL},
    {"toint", AETHER_T_INT, NULL}, {"getenvint", AETHER_T_INT, NULL},
    {"paramcount", AETHER_T_INT, NULL}, {"yyjsongetlength", AETHER_T_INT, NULL},
    {"yyjsongetint", AETHER_T_INT, NULL}, {"yyjsongetintor", AETHER_T_INT, NULL}, {"httpsession", AETHER_T_INT, NULL},
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
    {"parse_float", AETHER_T_REAL, NULL}, {"yyjsongetnumber", AETHER_T_REAL, NULL}, {"yyjsongetnumberor", AETHER_T_REAL, NULL},
    {"realtimeclock", AETHER_T_REAL, NULL},
    /* Text */
    {"inttostr", AETHER_T_TEXT, NULL}, {"itoa", AETHER_T_TEXT, NULL}, {"copy", AETHER_T_TEXT, NULL},
    {"trim", AETHER_T_TEXT, NULL}, {"formatfloat", AETHER_T_TEXT, NULL},
    {"realtostr", AETHER_T_TEXT, NULL}, {"stringofchar", AETHER_T_TEXT, NULL},
    {"getenv", AETHER_T_TEXT, NULL}, {"paramstr", AETHER_T_TEXT, NULL},
    {"getcurrentdir", AETHER_T_TEXT, NULL}, {"mstreambuffer", AETHER_T_TEXT, NULL},
    {"yyjsongetstring", AETHER_T_TEXT, NULL}, {"yyjsongetstringor", AETHER_T_TEXT, NULL}, {"yyjsongettype", AETHER_T_TEXT, NULL},
    {"aetherbuiltinsjson", AETHER_T_TEXT, NULL}, {"aetherbuiltininfo", AETHER_T_TEXT, NULL},
    {"openaichatcompletions", AETHER_T_TEXT, NULL}, {"threadstatsjson", AETHER_T_TEXT, NULL},
    {"httplasterror", AETHER_T_TEXT, NULL}, {"httpgetheader", AETHER_T_TEXT, NULL},
    {"httpgetlastheaders", AETHER_T_TEXT, NULL},
    /* Char */
    {"chr", AETHER_T_CHAR, NULL}, {"upcase", AETHER_T_TEXT, NULL}, {"readkey", AETHER_T_CHAR, NULL},
    /* Bool */
    {"hasextbuiltin", AETHER_T_BOOL, NULL}, {"yyjsonhaskey", AETHER_T_BOOL, NULL},
    {"yyjsonhasindex", AETHER_T_BOOL, NULL}, {"yyjsongetbool", AETHER_T_BOOL, NULL},
    {"yyjsonisnull", AETHER_T_BOOL, NULL}, {"yyjsongetboolor", AETHER_T_BOOL, NULL},
    {"fileexists", AETHER_T_BOOL, NULL},
    {"eof", AETHER_T_BOOL, NULL}, {"parse_bool", AETHER_T_BOOL, NULL}, {"odd", AETHER_T_BOOL, NULL},
    {"httpisdone", AETHER_T_BOOL, NULL},
    /* Handles */
    {"yyjsonread", AETHER_T_HANDLE, "ToonDoc"}, {"yyjsonreadfile", AETHER_T_HANDLE, "ToonDoc"},
    {"aether_try_toon_read_file", AETHER_T_HANDLE, "ToonDoc"},
    {"yyjsongetroot", AETHER_T_HANDLE, "ToonNode"}, {"yyjsongetkey", AETHER_T_HANDLE, "ToonNode"},
    {"yyjsongetindex", AETHER_T_HANDLE, "ToonNode"},
    {"mstreamcreate", AETHER_T_HANDLE, "MStream"}, {"mstreamfromstring", AETHER_T_HANDLE, "MStream"},
    {"socketreceive", AETHER_T_HANDLE, "MStream"},
    /* Text[] */
    {"split", RET_TEXT_ARRAY, NULL}, {"aether_read_lines", RET_TEXT_ARRAY, NULL},
    {"aether_try_read_lines_list", RET_TEXT_ARRAY, NULL},
    {"aether_args", RET_TEXT_ARRAY, NULL},
    /* The modern standard surface (pscal-core ext_builtins/strings/stdlib.c) */
    {"aether_upper", AETHER_T_TEXT, NULL}, {"aether_lower", AETHER_T_TEXT, NULL},
    {"aether_replace", AETHER_T_TEXT, NULL}, {"aether_repeat", AETHER_T_TEXT, NULL},
    {"aether_join", AETHER_T_TEXT, NULL}, {"aether_read_file", AETHER_T_TEXT, NULL},
    {"aether_try_read_file_text", AETHER_T_TEXT, NULL},
    {"aether_read_line", AETHER_T_TEXT, NULL}, {"aether_read_all_stdin", AETHER_T_TEXT, NULL},
    {"aether_now_text", AETHER_T_TEXT, NULL},
    {"aether_contains", AETHER_T_BOOL, NULL}, {"aether_starts_with", AETHER_T_BOOL, NULL},
    {"aether_ends_with", AETHER_T_BOOL, NULL}, {"aether_delete_file", AETHER_T_BOOL, NULL},
    {"aether_rename_file", AETHER_T_BOOL, NULL}, {"aetherparseintok", AETHER_T_BOOL, NULL},
    {"aetherparserealok", AETHER_T_BOOL, NULL},
    {"aether_same", AETHER_T_BOOL, NULL}, {"aether_last_io_ok", AETHER_T_BOOL, NULL},
    {"aether_is_digit", AETHER_T_BOOL, NULL}, {"aether_is_alpha", AETHER_T_BOOL, NULL},
    {"aether_is_alnum", AETHER_T_BOOL, NULL}, {"aether_is_space", AETHER_T_BOOL, NULL},
    {"aether_is_upper", AETHER_T_BOOL, NULL}, {"aether_is_lower", AETHER_T_BOOL, NULL},
    {"aether_index_of", AETHER_T_INT, NULL}, {"aether_count", AETHER_T_INT, NULL},
    {"aether_clock_ms", AETHER_T_INT, NULL}, {"aether_mstream_length", AETHER_T_INT, NULL},
    {"aetherparseintvalue", AETHER_T_INT, NULL},
    {"aether_round_to", AETHER_T_REAL, NULL}, {"aetherparserealvalue", AETHER_T_REAL, NULL},
    {"aether_mstream_load", AETHER_T_HANDLE, "MStream"},
    {"aether_write_file", AETHER_T_VOID, NULL}, {"aether_append_file", AETHER_T_VOID, NULL},
    {"aether_mstream_append_text", AETHER_T_VOID, NULL},
    {"aether_sorted", RET_ARG0, NULL}, {"aether_sorted_desc", RET_ARG0, NULL},
    {"aether_sorted_by", RET_ARG0, NULL}, {"aether_sorted_by_desc", RET_ARG0, NULL},
    {"aether_reversed", RET_ARG0, NULL},
    {"aether_repeat_array", RET_ARRAY_OF_ARG0, NULL},
    {"aether_sum", RET_ELEM0, NULL}, {"aether_min_of", RET_ELEM0, NULL}, {"aether_max_of", RET_ELEM0, NULL},
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

static AetherType typeOfMapCall(const AST *e, const AetherTypeEnv *env, int *handled) {
    const char *n = callName(e);
    *handled = 0;
    if (!n || e->left || strncmp(n, "aether_map_", 11) != 0) return mk(AETHER_T_UNKNOWN);
    *handled = 1;
    if (!strcmp(n, "aether_map_of")) {
        if (e->child_count < 3) return mk(AETHER_T_MAP);
        AetherType v = aetherTypeOf(e->children[2], env);
        for (int i = 4; i < e->child_count && v.kind == AETHER_T_INT; i += 2)
            if (aetherTypeOf(e->children[i], env).kind == AETHER_T_REAL) v = mk(AETHER_T_REAL);
        return mapTypeOfValue(v);
    }
    AetherType m = e->child_count >= 1 ? aetherTypeOf(e->children[0], env) : mk(AETHER_T_UNKNOWN);
    if (!strcmp(n, "aether_map_get") || !strcmp(n, "aether_map_get_or")) return mapValueType(m);
    if (!strcmp(n, "aether_map_has")) return mk(AETHER_T_BOOL);
    if (!strcmp(n, "aether_map_length")) return mk(AETHER_T_INT);
    if (!strcmp(n, "aether_map_keys")) {
        AetherType t = mk(AETHER_T_ARRAY);
        t.rank = 1;
        t.elem = AETHER_T_TEXT;
        return t;
    }
    if (!strcmp(n, "aether_map_values")) {
        AetherType v = mapValueType(m);
        if (v.kind == AETHER_T_UNKNOWN) return v;
        AetherType t = mk(AETHER_T_ARRAY);
        if (v.kind == AETHER_T_ARRAY) {
            t.rank = v.rank + 1;
            t.elem = v.elem;
        } else {
            t.rank = 1;
            t.elem = v.kind;
        }
        t.name = v.name;
        return t;
    }
    return mk(AETHER_T_VOID);
}

static AetherType typeOfCall(const AST *e, const AetherTypeEnv *env) {
    const AST *fn = lookupCallee(e, env);
    if (fn) return functionReturnType(fn);
    {
        int handled = 0;
        AetherType mt = typeOfMapCall(e, env, &handled);
        if (handled) return mt;
    }
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
    if (b->ret == RET_ARRAY_OF_ARG0) {
        AetherType a = e->child_count >= 1 ? aetherTypeOf(e->children[0], env)
                                           : mk(AETHER_T_UNKNOWN);
        if (a.kind == AETHER_T_UNKNOWN) return a;
        AetherType t = mk(AETHER_T_ARRAY);
        if (a.kind == AETHER_T_ARRAY) {
            t.elem = a.elem;
            t.rank = a.rank + 1;
        } else {
            t.elem = a.kind;
            t.rank = 1;
        }
        t.name = a.name;
        return t;
    }
    if (b->ret == RET_ARG0)
        return e->child_count >= 1 ? aetherTypeOf(e->children[0], env) : mk(AETHER_T_UNKNOWN);
    if (b->ret == RET_ELEM0) {
        AetherType a = e->child_count >= 1 ? aetherTypeOf(e->children[0], env)
                                           : mk(AETHER_T_UNKNOWN);
        if (a.kind == AETHER_T_ARRAY && a.rank == 1 &&
            (a.elem == AETHER_T_INT || a.elem == AETHER_T_REAL || a.elem == AETHER_T_TEXT))
            return mk(a.elem);
        return mk(AETHER_T_UNKNOWN);
    }
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

/* The Aether type name of an expression with no environment (literals,
 * builtin calls, operators over those), for a hint; NULL when unknown. A Char
 * reads as Text, the only character type a program can write. Used by the
 * parser's TYPE-001 "cannot infer" hint (W6-06), before any pass has run. */
const char *aetherHintTypeName(const AST *e, char *buf, size_t n) {
    AetherType t = aetherTypeOf(e, NULL);
    if (t.kind == AETHER_T_CHAR) t.kind = AETHER_T_TEXT;
    if (t.kind == AETHER_T_ARRAY && t.elem == AETHER_T_CHAR) t.elem = AETHER_T_TEXT;
    if (t.kind == AETHER_T_UNKNOWN || t.kind == AETHER_T_VOID || t.kind == AETHER_T_NIL)
        return NULL;
    if (t.kind == AETHER_T_ARRAY && t.elem == AETHER_T_UNKNOWN) return NULL;
    if ((t.kind == AETHER_T_RECORD || t.kind == AETHER_T_HANDLE) && !t.name) return NULL;
    return aetherTypeFormat(t, buf, n);
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

/* D68: a map builtin's value argument is a sink of the map's value type. */
static int walkMapCallArgs(Walker *w, AST *e, AetherUse use) {
    const char *name = callName(e);
    if (e->type != AST_PROCEDURE_CALL || e->left || !name || strncmp(name, "aether_map_", 11) != 0)
        return 0;
    if (!strcmp(name, "aether_map_of")) {
        AetherType vt = mk(AETHER_T_UNKNOWN);
        if (use.kind == AETHER_USE_SINK && use.want.kind == AETHER_T_MAP) vt = mapValueType(use.want);
        for (int i = 0; i < e->child_count; i++) {
            AST *a = e->children[i];
            if (!a) continue;
            if (i >= 2 && i % 2 == 0 && vt.kind != AETHER_T_UNKNOWN)
                walkValue(w, e, a, sinkUse(vt, "map-literal"));
            else
                walkExpr(w, a, useOf(AETHER_USE_OTHER));
        }
        return 1;
    }
    AetherType vt = e->child_count >= 1 ? mapValueType(aetherTypeOf(e->children[0], &w->env))
                                        : mk(AETHER_T_UNKNOWN);
    for (int i = 0; i < e->child_count; i++) {
        AST *a = e->children[i];
        if (!a) continue;
        if (i == 2 && vt.kind != AETHER_T_UNKNOWN && !strcmp(name, "aether_map_set"))
            walkValue(w, e, a, sinkUse(vt, "map-value"));
        else if (i == 2 && vt.kind != AETHER_T_UNKNOWN && !strcmp(name, "aether_map_get_or"))
            walkValue(w, e, a, sinkUse(vt, "map-default"));
        else
            walkExpr(w, a, useOf(AETHER_USE_BUILTIN_ARG));
    }
    return 1;
}

static void walkCallArgs(Walker *w, AST *e, AetherUse use) {
    if (walkMapCallArgs(w, e, use)) return;
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
            walkCallArgs(w, e, use);
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
                /* The target itself, so a rule can check the field name. */
                if (w->v->onExpr) {
                    AetherUse u = useOf(AETHER_USE_OTHER);
                    w->v->onExpr(w->v->ctx, s->left, &u, &w->env);
                }
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
        if (!m || m == root) continue;
        /* A rule's diagnostic names the module's file, not the importer's. */
        const char *path = aetherGetModulePath(i);
        const char *saved = path ? aetherSemanticSwapReportPath(path) : NULL;
        walkStmt(&w, m);
        if (path) aetherSemanticSwapReportPath(saved);
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
    int fileLines[32]; /* W4-32: a forward declaration repeats its params */
    int fileLineCount;
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

/* W4-17: a value whose type the oracle knows, stored where a different type is
 * declared. `fn g() -> Text { ret 5; }`, `xs[0] = "z"` on an Int[], a `Q`
 * passed for a `P` parameter and `let a: A = new B { ... }` all compiled; the
 * VM then stored the wrong value (a record read by slot handed back B's data
 * through A's field) or stopped later with an uncoded "Type mismatch". Only
 * an outright MISMATCH is judged: Int into Real widens, Real into Int keeps
 * its D1 truncation (NARROW-001), and anything the oracle cannot type passes. */
static const char *sinkWhere(const char *label) {
    if (!label) return NULL;
    if (!strcmp(label, "let") || !strcmp(label, "const")) return "this binding is declared";
    if (!strcmp(label, "assign")) return "the assignment target is";
    if (!strcmp(label, "ret")) return "the function returns";
    if (!strcmp(label, "arg")) return "this parameter is";
    if (!strcmp(label, "element")) return "the array's elements are";
    if (!strcmp(label, "field-init")) return "this field is";
    if (!strcmp(label, "map-value") || !strcmp(label, "map-literal") ||
        !strcmp(label, "map-default"))
        return "the map's values are";
    return NULL;
}

static int isMapLiteralCall(const AST *e) {
    const char *n = callName(e);
    return e && e->type == AST_PROCEDURE_CALL && !e->left && n && !strcmp(n, "aether_map_of");
}

static VarType varTypeOfOracle(AetherType t) {
    switch (t.kind) {
        case AETHER_T_INT: return TYPE_INT64;
        case AETHER_T_REAL: return TYPE_DOUBLE;
        case AETHER_T_TEXT: case AETHER_T_CHAR: return TYPE_UNICODE_STRING;
        case AETHER_T_BOOL: return TYPE_BOOLEAN;
        case AETHER_T_ARRAY: return TYPE_ARRAY;
        case AETHER_T_MAP: return TYPE_MAP;
        case AETHER_T_RECORD: return TYPE_POINTER;
        default: return TYPE_UNKNOWN;
    }
}

/* D68: a map literal bound to a declared map type stores that value type
 * (aether_map_of's first argument), so `{}` and Int values into a Real map
 * are typed by the declaration. */
static void fixMapLiteralCode(const AST *value, AetherType want) {
    if (!isMapLiteralCall(value) || want.kind != AETHER_T_MAP || !want.name) return;
    VarType vt = varTypeOfOracle(mapValueType(want));
    if (vt == TYPE_UNKNOWN || value->child_count < 1 || !value->children[0]) return;
    AST *code = value->children[0];
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)vt);
    if (code->token) {
        free(code->token->value);
        code->token->value = strdup(buf);
        code->token->length = strlen(buf);
    }
    code->i_val = (int)vt;
}

static void ruleSinkMismatch(RuleCtx *c, const AST *value, const AetherUse *use,
                             const AetherTypeEnv *env) {
    fixMapLiteralCode(value, use->want);
    /* A map literal's values are each judged against the value type. */
    if (isMapLiteralCall(value) && use->want.kind == AETHER_T_MAP) return;
    const char *where = sinkWhere(use->sinkLabel);
    if (!where) return;
    AetherType have = aetherTypeOf(value, env);
    if (aetherTypeAgreement(use->want, have) != AETHER_AGREE_MISMATCH) return;
    char want[160], got[160], detail[640];
    aetherTypeFormat(use->want, want, sizeof(want));
    aetherTypeFormat(have, got, sizeof(got));
    int scalars = use->want.kind != AETHER_T_RECORD && use->want.kind != AETHER_T_ARRAY &&
                  have.kind != AETHER_T_RECORD && have.kind != AETHER_T_ARRAY;
    snprintf(detail, sizeof(detail), "type mismatch: %s %s, but this value is %s. %s",
             where, want, got,
             scalars ? "Convert it explicitly (int_to_text, parse_int, int, ...) or fix "
                       "the declaration."
                     : "Use a value of the declared type, or fix the declaration.");
    ruleReport(c, value, detail);
}

static void ruleSink(void *vctx, const AST *site, const AST *value, const AetherUse *use,
                     const AetherTypeEnv *env) {
    RuleCtx *c = (RuleCtx *)vctx;
    ruleSinkMismatch(c, value, use, env);
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

/* FIELD-002 for a field the record does not declare. The backend reported
 * these itself, with no code and no file: "L2: Compiler error: Unknown field
 * 'Count'." (a case slip on `count`) and "field 'y' not found in record
 * initializer for 'P'". */
static void ruleFieldName(RuleCtx *c, const AST *at, const char *rec, const char *field) {
    if (!rec || !field) return;
    char declared[256];
    int m = recordFieldMatch(rec, field, declared, sizeof(declared));
    if (m != 0 && m != 2) return;
    char detail[512];
    if (m == 2)
        snprintf(detail, sizeof(detail),
                 "Unknown field '%s' on type '%s': field names are case-sensitive, and the "
                 "field is spelled '%s'.",
                 field, rec, declared);
    else
        snprintf(detail, sizeof(detail),
                 "Unknown field '%s' on type '%s'. Its fields are: %s.", field, rec,
                 declared[0] ? declared : "(none)");
    int line = nodeLine(at);
    aetherSemanticReportCoded("FIELD-002", "field", line > 0 ? line : 1, detail, 1);
    c->reported++;
}

/* True if record type `name` can reach itself through its fields (directly, or
 * through a field of another record type or an array of records). `==` on such
 * a type could recurse without end, so it is REC-001 (D24). */
static int recordReaches(const char *from, const char *target, const char **seen, int nseen) {
    const AST *rec = indexGet(&g_records, from);
    if (!rec || nseen >= 64) return 0;
    for (int i = 0; i < nseen; i++)
        if (strcasecmp(seen[i], from) == 0) return 0;
    seen[nseen++] = from;
    for (int i = 0; i < rec->child_count; i++) {
        const AST *f = rec->children[i];
        if (!f || f->type != AST_VAR_DECL) continue;
        AetherType ft = declTypeOf(f);
        const char *next = NULL;
        if (ft.kind == AETHER_T_RECORD) next = ft.name;
        else if (ft.kind == AETHER_T_ARRAY && ft.elem == AETHER_T_RECORD) next = ft.name;
        if (!next) continue;
        if (strcasecmp(next, target) == 0) return 1;
        if (recordReaches(next, target, seen, nseen)) return 1;
    }
    return 0;
}

static int recordIsSelfReferential(const char *name);

/* D68: map keys are Text; a map is not an array, a number or another type. */
static int ruleMapExpr(RuleCtx *c, AST *e, const AetherTypeEnv *env) {
    const char *n = callName(e);
    if (e->type == AST_PROCEDURE_CALL && !e->left && n && strncmp(n, "aether_map_", 11) == 0 &&
        strcmp(n, "aether_map_of") != 0 && e->child_count >= 2 &&
        (!strcmp(n, "aether_map_get") || !strcmp(n, "aether_map_get_or") ||
         !strcmp(n, "aether_map_has") || !strcmp(n, "aether_map_set") ||
         !strcmp(n, "aether_map_remove") || !strcmp(n, "aether_map_take"))) {
        AetherType m = aetherTypeOf(e->children[0], env);
        AetherType k = aetherTypeOf(e->children[1], env);
        if (m.kind != AETHER_T_MAP && m.kind != AETHER_T_UNKNOWN) {
            char got[160], detail[320];
            aetherTypeFormat(m, got, sizeof(got));
            snprintf(detail, sizeof(detail),
                     "%s works on a map (`Map[Text, T]`), but this value is %s.",
                     !strcmp(n, "aether_map_has") ? "has" : !strcmp(n, "aether_map_remove")
                         ? "remove" : !strcmp(n, "aether_map_get_or") ? "get" : "this",
                     got);
            ruleReport(c, e->children[0], detail);
            return 1;
        }
        if (k.kind != AETHER_T_UNKNOWN && !isTextual(k)) {
            char got[160], mt[160], detail[480];
            aetherTypeFormat(k, got, sizeof(got));
            aetherTypeFormat(m.kind == AETHER_T_MAP && m.name ? m : mkNamed(AETHER_T_MAP, "Map[Text, T]"),
                             mt, sizeof(mt));
            snprintf(detail, sizeof(detail),
                     "a map key is Text (`%s`), but this key is %s.%s", mt, got,
                     k.kind == AETHER_T_INT ? " Convert it with int_to_text(k)." : "");
            ruleReport(c, e->children[1], detail);
            return 1;
        }
        return 0;
    }
    if (e->type == AST_ARRAY_ACCESS && aetherTypeOf(e->left, env).kind == AETHER_T_MAP) {
        ruleReport(c, e,
                   "this map is indexed where its type cannot be named at parse time; bind it "
                   "to a typed local first: `let m: Map[Text, T] = ...; m[k]`.");
        return 1;
    }
    if (e->type == AST_BINARY_OP && e->token) {
        AetherType l = aetherTypeOf(e->left, env), r = aetherTypeOf(e->right, env);
        if (l.kind != AETHER_T_MAP && r.kind != AETHER_T_MAP) return 0;
        TokenType tt = e->token->type;
        char lt[160], rt[160], detail[640];
        aetherTypeFormat(l, lt, sizeof(lt));
        aetherTypeFormat(r, rt, sizeof(rt));
        if (tt == TOKEN_EQUAL || tt == TOKEN_NOT_EQUAL) {
            if (l.kind == AETHER_T_UNKNOWN || r.kind == AETHER_T_UNKNOWN) return 0;
            if (aetherTypeAgreement(l, r) != AETHER_AGREE_MISMATCH &&
                aetherTypeAgreement(r, l) != AETHER_AGREE_MISMATCH) {
                /* D24: values compare structurally, so a map of a
                 * self-referential record type is REC-001, as the record is. */
                AetherType v = mapValueType(l.kind == AETHER_T_MAP ? l : r);
                while (v.kind == AETHER_T_MAP) v = mapValueType(v);
                const char *rn = (v.kind == AETHER_T_RECORD ||
                                  (v.kind == AETHER_T_ARRAY && v.elem == AETHER_T_RECORD)) ? v.name : NULL;
                if (rn && recordIsSelfReferential(rn)) {
                    snprintf(detail, sizeof(detail),
                             "`==` compares maps by their values, and the values' type %s reaches "
                             "itself through its fields, so the comparison could recurse without "
                             "end. Compare the keys and the fields you mean.",
                             rn);
                    int line = nodeLine(e);
                    aetherSemanticReportCoded("REC-001", "record-compare", line > 0 ? line : 1,
                                              detail, 1);
                    c->reported++;
                    return 1;
                }
                return 0;
            }
            snprintf(detail, sizeof(detail),
                     "cannot compare %s with %s: `==` compares two maps of the same type.", lt, rt);
            ruleReport(c, e, detail);
            return 1;
        }
        if (tt == TOKEN_LESS || tt == TOKEN_LESS_EQUAL || tt == TOKEN_GREATER ||
            tt == TOKEN_GREATER_EQUAL) {
            ruleReport(c, e, "maps have no order: compare them only with `==` and `!=`.");
            return 1;
        }
        if (tt == TOKEN_PLUS || tt == TOKEN_MINUS || tt == TOKEN_MUL || tt == TOKEN_SLASH ||
            tt == TOKEN_INT_DIV || tt == TOKEN_MOD) {
            snprintf(detail, sizeof(detail),
                     "a map is not a number or Text (%s %s %s): to add an entry write "
                     "`m[k] = v;`; to merge maps, loop over the keys of one.",
                     lt, e->token->value ? e->token->value : "?", rt);
            ruleReport(c, e, detail);
            return 1;
        }
    }
    if (isWriteCall(e)) {
        for (int i = 0; i < e->child_count; i++) {
            const AST *a = e->children[i];
            if (a && a->type == AST_FORMATTED_EXPR) a = a->left;
            AetherType t = a ? aetherTypeOf(a, env) : mk(AETHER_T_UNKNOWN);
            if (t.kind == AETHER_T_MAP && typeHoldsRecord(t, 0)) {
                ruleReport(c, e,
                           "a map whose values are records has no printed form: loop over "
                           "its keys and print the fields (`loop k in m { ... m[k].f ... }`).");
                return 1;
            }
        }
    }
    return 0;
}

static int recordIsSelfReferential(const char *name) {
    const char *seen[64];
    return name && recordReaches(name, name, seen, 0);
}

/* D24: `==` / `!=` on records and tuples compares field by field. Two
 * different record types never compare equal, so that is a TYPE-001; a
 * self-referential type is REC-001, with `same(a, b)` as the identity test. */
static void ruleRecordCompare(RuleCtx *c, const AST *e, const AetherTypeEnv *env) {
    AetherType lt = aetherTypeOf(e->left, env);
    AetherType rt = aetherTypeOf(e->right, env);
    const char *ln = NULL, *rn = NULL;
    if (lt.kind == AETHER_T_RECORD) ln = lt.name;
    else if (lt.kind == AETHER_T_ARRAY && lt.elem == AETHER_T_RECORD) ln = lt.name;
    if (rt.kind == AETHER_T_RECORD) rn = rt.name;
    else if (rt.kind == AETHER_T_ARRAY && rt.elem == AETHER_T_RECORD) rn = rt.name;
    if (!ln || !rn) return;
    char detail[320];
    if (strcasecmp(ln, rn) != 0) {
        snprintf(detail, sizeof(detail),
                 "a %s is compared with a %s: values of two different types are never equal. "
                 "Compare the fields you mean.",
                 ln, rn);
        ruleReport(c, e, detail);
        return;
    }
    if (recordIsSelfReferential(ln)) {
        snprintf(detail, sizeof(detail),
                 "`==` compares records field by field, and %s reaches itself through its "
                 "fields, so the comparison could recurse without end. Use `same(a, b)` to "
                 "test whether two values are the same record, or compare the fields you mean.",
                 ln);
        int line = nodeLine(e);
        aetherSemanticReportCoded("REC-001", "record-compare", line > 0 ? line : 1, detail, 1);
        c->reported++;
    }
}

static void ruleExpr(void *vctx, AST *e, const AetherUse *use, const AetherTypeEnv *env) {
    RuleCtx *c = (RuleCtx *)vctx;
    if (ruleMapExpr(c, e, env)) return;
    /* D8 V-strict: a call statement that hands over an array and throws away
     * the array it returns (`sortDesc(xs);`, `sorted(xs);`) leaves xs as it
     * was, because arrays are values. A map (D68) is a value the same way. */
    AetherTypeKind retKind = aetherTypeOf(e, env).kind;
    if (use->kind == AETHER_USE_NONE && e->type == AST_PROCEDURE_CALL &&
        (retKind == AETHER_T_ARRAY || retKind == AETHER_T_MAP)) {
        int isMap = retKind == AETHER_T_MAP;
        for (int i = 0; i < e->child_count; i++) {
            const AST *a = e->children[i];
            if (!a || aetherTypeOf(a, env).kind != retKind) continue;
            char shown[96], detail[768];
            const char *fname = callName(e);
            if (fname && strncasecmp(fname, "aether_", 7) == 0) fname += 7;
            if (a->type == AST_VARIABLE && a->token && a->token->value)
                snprintf(shown, sizeof(shown), "%s", a->token->value);
            else
                snprintf(shown, sizeof(shown), isMap ? "m" : "xs");
            snprintf(detail, sizeof(detail),
                     "the %s %s() returns is thrown away. %s are values, so the call "
                     "cannot change `%s` itself: return it and reassign, `%s = %s(%s%s)`.",
                     isMap ? "map" : "array", fname ? fname : "f", isMap ? "Maps" : "Arrays",
                     shown, shown, fname ? fname : "f", shown, e->child_count > 1 ? ", ..." : "");
            int line = nodeLine(e);
            aetherSemanticReportCoded("ARR-001", "array-mutation", line > 0 ? line : 1, detail, 1);
            c->reported++;
            break;
        }
    }
    if (e->type == AST_BINARY_OP && e->token &&
        (e->token->type == TOKEN_EQUAL || e->token->type == TOKEN_NOT_EQUAL)) {
        ruleRecordCompare(c, e, env);
    }
    /* same(a, b) is identity, which only records have. */
    if (e->type == AST_PROCEDURE_CALL && callName(e) && strcasecmp(callName(e), "aether_same") == 0) {
        for (int i = 0; i < e->child_count; i++) {
            AetherType at = aetherTypeOf(e->children[i], env);
            if (at.kind == AETHER_T_UNKNOWN || at.kind == AETHER_T_RECORD || at.kind == AETHER_T_NIL)
                continue;
            char got[96], detail[256];
            aetherTypeFormat(at, got, sizeof(got));
            snprintf(detail, sizeof(detail),
                     "same() tests whether two records are the same record; this value is %s. "
                     "Compare values with `==`.",
                     got);
            ruleReport(c, e->children[i], detail);
            break;
        }
    }
    if (e->type == AST_FIELD_ACCESS && e->right && e->right->token && e->right->token->value) {
        AetherType rt = aetherTypeOf(e->left, env);
        if (rt.kind == AETHER_T_RECORD && rt.name)
            ruleFieldName(c, e, rt.name, e->right->token->value);
        return;
    }
    if (e->type == AST_NEW && e->extra && e->token && e->token->value) {
        for (int i = 0; i < e->extra->child_count; i++) {
            const AST *fi = e->extra->children[i];
            if (fi && fi->type == AST_ASSIGN && fi->left && fi->left->token && fi->left->token->value)
                ruleFieldName(c, fi, e->token->value, fi->left->token->value);
        }
    }
    /* A condition must be a Bool. `if x = 2 { ... }` assigned 2 to x and took
     * the branch (an assignment is an expression in the shared engine, and an
     * Int is truthy there), and `if n { ... }` on an Int ran as `n != 0`. */
    if (use->kind == AETHER_USE_CONDITION) {
        if (e->type == AST_ASSIGN) {
            ruleReport(c, e,
                       "`=` assigns, it does not compare: write `==` to test equality "
                       "(an assignment is not a condition).");
            return;
        }
        AetherType ct = aetherTypeOf(e, env);
        if (ct.kind != AETHER_T_UNKNOWN && ct.kind != AETHER_T_BOOL) {
            char got[96], detail[256];
            aetherTypeFormat(ct, got, sizeof(got));
            snprintf(detail, sizeof(detail),
                     "a condition must be Bool, but this value is %s: compare it "
                     "explicitly (`n != 0`, `s != \"\"`, `length(xs) > 0`).",
                     got);
            ruleReport(c, e, detail);
            return;
        }
    }
    if (realRulesOn() && (use->kind == AETHER_USE_MOD || use->kind == AETHER_USE_INT_DIV) &&
        aetherTypeOf(e, env).kind == AETHER_T_REAL) {
        reportRealInIntPosition(c, e, use->kind == AETHER_USE_MOD ? "`%`" : "`div`");
        return;
    }
    /* D4 B+: `/` is always Real, so a quotient that reaches an Int-only place
     * is a coded error rather than a silent truncation: an index (array or
     * Text; `xs[n / 2]` used to index 1 for 3.5 / 2 = 1.75) and a range bound
     * (D18: `loop i in 0..n / 2`, its lower bound and its step). */
    if (realRulesOn() && aetherTypeOf(e, env).kind == AETHER_T_REAL) {
        if (use->kind == AETHER_USE_INDEX) {
            reportRealInIntPosition(c, e, "an index");
            return;
        }
        if (use->kind == AETHER_USE_RANGE_BOUND || aetherAstIsRangeBound(e)) {
            reportRealInIntPosition(c, e, "a loop range bound");
            return;
        }
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

/* W4-32 (D34), a stopgap until File is reference-counted (W5): a File copy is
 * not retained (pscal-core utils.c), so a File passed, returned, stored in a
 * record field or copied into a second binding crashed with rc 139 and empty
 * output, or corrupted the heap and exited 0. Delete with W5's File retain. */
/* By VarType: `File` lowers to rea's `text` keyword, whose type node the
 * oracle's name table would read as Text. */
static int isFileDecl(const AST *decl) {
    return decl && (decl->var_type == TYPE_FILE || (decl->right && decl->right->var_type == TYPE_FILE));
}

static void reportFileStopgap(RuleCtx *c, const AST *at, const char *what) {
    int line = nodeLine(at);
    for (int i = 0; i < c->fileLineCount; i++)
        if (c->fileLines[i] == line) return;
    if (c->fileLineCount < 32) c->fileLines[c->fileLineCount++] = line;
    char detail[320];
    snprintf(detail, sizeof(detail),
             "a File cannot be %s yet: its copies are not tracked and the program crashes. "
             "Use one top-level `let f: File;` that helpers read, or pass the path as Text "
             "and open the file in the helper.",
             what);
    ruleReport(c, at, detail);
}

static void fileScan(RuleCtx *c, const AST *n) {
    if (!n) return;
    switch (n->type) {
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            for (int i = 0; i < n->child_count; i++) {
                const AST *prm = n->children[i];
                if (prm && prm->type == AST_VAR_DECL && isFileDecl(prm))
                    reportFileStopgap(c, prm, "a parameter");
            }
            if (n->type == AST_FUNCTION_DECL && n->var_type == TYPE_FILE)
                reportFileStopgap(c, n->right ? n->right : n, "a return value");
            break;
        case AST_TYPE_DECL:
            if (n->left && n->left->type == AST_RECORD_TYPE) {
                for (int i = 0; i < n->left->child_count; i++) {
                    const AST *f = n->left->children[i];
                    if (f && f->type == AST_VAR_DECL && isFileDecl(f))
                        reportFileStopgap(c, f, "a record field");
                }
            }
            return;
        case AST_VAR_DECL:
            if (n->left && isFileDecl(n)) reportFileStopgap(c, n, "copied into a `let`");
            return;
        case AST_ASSIGN:
            if (n->left && n->left->type == AST_VARIABLE && n->left->var_type == TYPE_FILE)
                reportFileStopgap(c, n, "assigned to another File");
            break;
        default:
            break;
    }
    fileScan(c, n->left);
    fileScan(c, n->right);
    fileScan(c, n->extra);
    for (int i = 0; i < n->child_count; i++) fileScan(c, n->children[i]);
}

void aetherTypedRules(AST *root) {
    if (!root) return;
    RuleCtx c;
    memset(&c, 0, sizeof(c));
    fileScan(&c, root);
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
