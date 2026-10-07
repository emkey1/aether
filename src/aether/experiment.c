/*
 * experiment.c -- AETHER_EXPERIMENT parsing and the experiment arms built on
 * the type oracle. See experiment.h.
 */

#include "aether/experiment.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/types.h"
#include "core/utils.h"
#include "aether/ast_internal.h"
#include "aether/parser.h"
#include "aether/semantic.h"
#include "aether/types.h"

static AetherExperiment g_experiment;
static int g_experiment_loaded = 0;

static int lookupValue(const char *value, const char *const *names, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(value, names[i]) == 0) return i;
    }
    return -1;
}

static void usageError(const char *item) {
    fprintf(stderr,
            "AETHER_EXPERIMENT: unknown item '%s'. Expected a comma-separated list of "
            "div=current|int|intplus|realplus|ctx, arrays=value|vstrict|ref.\n",
            item);
    exit(2);
}

static void loadExperiment(void) {
    static const char *const kDiv[] = {"current", "int", "intplus", "realplus", "ctx"};
    static const char *const kArrays[] = {"value", "vstrict", "ref"};
    const char *env = getenv("AETHER_EXPERIMENT");
    g_experiment_loaded = 1;
    memset(&g_experiment, 0, sizeof(g_experiment));
    if (!env || !*env) return;
    char *copy = strdup(env);
    if (!copy) return;
    char *save = NULL;
    for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        while (*item == ' ') item++;
        if (!*item) continue;
        char *eq = strchr(item, '=');
        if (!eq) usageError(item);
        *eq = '\0';
        const char *key = item;
        const char *value = eq + 1;
        int v = -1;
        if (strcmp(key, "div") == 0) {
            v = lookupValue(value, kDiv, 5);
            if (v >= 0) g_experiment.div = (AetherDivRule)v;
        } else if (strcmp(key, "arrays") == 0) {
            v = lookupValue(value, kArrays, 3);
            if (v >= 0) g_experiment.arrays = (AetherArraysRule)v;
        }
        if (v < 0) {
            *eq = '=';
            usageError(item);
        }
    }
    free(copy);
    g_experiment.any = g_experiment.div != AETHER_DIV_CURRENT ||
                       g_experiment.arrays != AETHER_ARRAYS_VALUE;
}

const AetherExperiment *aetherExperiment(void) {
    if (!g_experiment_loaded) loadExperiment();
    return &g_experiment;
}

const char *aetherDumpTypesTarget(void) {
    const char *env = getenv("AETHER_DUMP_TYPES");
    return (env && *env && strcmp(env, "0") != 0) ? env : NULL;
}

/* ------------------------------------------------------------------ */
/* Site log                                                            */
/* ------------------------------------------------------------------ */

static FILE *g_log = NULL;

static FILE *experimentLog(void) {
    static int opened = 0;
    if (!opened) {
        const char *path = getenv("AETHER_EXPERIMENT_LOG");
        opened = 1;
        if (path && *path) g_log = fopen(path, "a");
    }
    return g_log;
}

static const char *useName(AetherUseKind k) {
    switch (k) {
        case AETHER_USE_SINK: return "sink";
        case AETHER_USE_INFERRED: return "inferred";
        case AETHER_USE_INDEX: return "index";
        case AETHER_USE_RANGE_BOUND: return "range-bound";
        case AETHER_USE_MOD: return "mod";
        case AETHER_USE_INT_DIV: return "div";
        case AETHER_USE_MUL: return "mul";
        case AETHER_USE_DIV: return "slash";
        case AETHER_USE_COMPARE: return "compare";
        case AETHER_USE_PRINT: return "print";
        case AETHER_USE_CONDITION: return "condition";
        case AETHER_USE_BUILTIN_ARG: return "builtin-arg";
        case AETHER_USE_CALL_ARG: return "call-arg";
        case AETHER_USE_OTHER: return "other";
        default: return "none";
    }
}

static void logSite(const char *arm, int line, const AetherUse *use, const char *action) {
    FILE *f = experimentLog();
    char want[64];
    if (!f) return;
    const char *path = aetherSemanticGetSourcePath();
    fprintf(f, "%s\t%s\t%d\t%s\t%s\t%s\t%s\n", arm, path ? path : "-", line, useName(use->kind),
            use->sinkLabel ? use->sinkLabel : "-",
            use->kind == AETHER_USE_SINK ? aetherTypeFormat(use->want, want, sizeof(want)) : "-",
            action);
}

/* ------------------------------------------------------------------ */
/* D4: Int/Int `/` (W8-08)                                             */
/* ------------------------------------------------------------------ */

typedef enum { DEMAND_INT, DEMAND_REAL, DEMAND_DIFFER } DivDemand;

/* Does the value's use demand an Int (both readings then agree once the
 * quotient is integral), a Real, or something where the two readings differ? */
static DivDemand divDemand(const AetherUse *use) {
    switch (use->kind) {
        case AETHER_USE_SINK:
            if (use->want.kind == AETHER_T_INT) return DEMAND_INT;
            if (use->want.kind == AETHER_T_REAL) return DEMAND_REAL;
            return DEMAND_DIFFER;
        case AETHER_USE_INDEX:
        case AETHER_USE_RANGE_BOUND:
        case AETHER_USE_MOD:
        case AETHER_USE_INT_DIV:
            return DEMAND_INT;
        default:
            return DEMAND_DIFFER;
    }
}

/* B+ rejects a Real quotient where it reaches `%`, an index, a range bound or
 * an Int builtin argument (int_to_text and friends); Int `let`s keep
 * truncating. */
static int realPlusRejects(const AetherUse *use) {
    switch (use->kind) {
        case AETHER_USE_INDEX:
        case AETHER_USE_RANGE_BOUND:
        case AETHER_USE_MOD:
        case AETHER_USE_INT_DIV:
            return 1;
        case AETHER_USE_SINK:
            return use->want.kind == AETHER_T_INT && use->sinkLabel &&
                   strcmp(use->sinkLabel, "builtin-arg") == 0;
        default:
            return 0;
    }
}

typedef struct {
    AetherDivRule rule;
    const AST **rewritten;
    int count, cap;
} DivCtx;

static void rememberRewrite(DivCtx *c, const AST *e) {
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        const AST **g = (const AST **)realloc((void *)c->rewritten, (size_t)cap * sizeof(*g));
        if (!g) return;
        c->rewritten = g;
        c->cap = cap;
    }
    c->rewritten[c->count++] = e;
}

static int containsRewrite(const DivCtx *c, const AST *n) {
    if (!n) return 0;
    for (int i = 0; i < c->count; i++)
        if (c->rewritten[i] == n) return 1;
    if (containsRewrite(c, n->left) || containsRewrite(c, n->right) || containsRewrite(c, n->extra))
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (containsRewrite(c, n->children[i])) return 1;
    return 0;
}

static void rewriteToIntDiv(DivCtx *c, AST *e) {
    e->token->type = TOKEN_INT_DIV;
    free(e->token->value);
    e->token->value = strdup("div");
    e->token->length = 3;
    e->var_type = TYPE_INT64;
    rememberRewrite(c, e);
}

static void reportDiv001(int line, AetherDivRule rule) {
    const char *why;
    switch (rule) {
        case AETHER_DIV_INTPLUS:
            why = "Int / Int is the integer quotient here, and this use expects a Real";
            break;
        case AETHER_DIV_REALPLUS:
            why = "Int / Int is a Real here, and this use needs an Int";
            break;
        default:
            why = "Int / Int reads as the integer quotient or as a Real, and here the two "
                  "readings give different results";
            break;
    }
    char detail[320];
    snprintf(detail, sizeof(detail),
             "%s. Write `a div b` for the integer quotient, or `real(a) / b` for the Real one.",
             why);
    aetherSemanticReportCoded("DIV-001", "division", line, detail, 1);
}

static void divPost(void *vctx, AST *e, const AetherUse *use, const AetherTypeEnv *env) {
    DivCtx *c = (DivCtx *)vctx;
    if (e->type != AST_BINARY_OP || !e->token || e->token->type != TOKEN_SLASH) return;
    AetherType l = aetherTypeOf(e->left, env);
    AetherType r = aetherTypeOf(e->right, env);
    if (l.kind == AETHER_T_REAL || r.kind == AETHER_T_REAL) return; /* a Real division */
    int line = e->token->line;
    if (l.kind != AETHER_T_INT || r.kind != AETHER_T_INT) {
        logSite("div", line, use, "unknown");
        return;
    }
    DivDemand d = divDemand(use);
    switch (c->rule) {
        case AETHER_DIV_INT:
            rewriteToIntDiv(c, e);
            logSite("div", line, use, "int");
            return;
        case AETHER_DIV_INTPLUS:
            rewriteToIntDiv(c, e);
            if (d == DEMAND_REAL) {
                reportDiv001(line, c->rule);
                logSite("div", line, use, "div001");
            } else {
                logSite("div", line, use, "int");
            }
            return;
        case AETHER_DIV_REALPLUS:
            if (realPlusRejects(use)) {
                reportDiv001(line, c->rule);
                logSite("div", line, use, "div001");
            } else {
                logSite("div", line, use, "real");
            }
            return;
        case AETHER_DIV_CTX:
            if (d == DEMAND_INT) {
                rewriteToIntDiv(c, e);
                logSite("div", line, use, "int");
            } else {
                reportDiv001(line, c->rule);
                logSite("div", line, use, "div001");
            }
            return;
        default:
            logSite("div", line, use, "real");
            return;
    }
}

/* An inferred `let q = a / b;` was typed Real at parse time. Once the quotient
 * is integral the binding follows it, as it would under a real A rule. */
static void divDecl(void *vctx, AST *decl, const AetherTypeEnv *env) {
    DivCtx *c = (DivCtx *)vctx;
    if (c->count == 0 || !decl->left || aetherAstDeclHasExplicitType(decl)) return;
    if (decl->var_type != TYPE_DOUBLE && decl->var_type != TYPE_FLOAT) return;
    if (aetherTypeOf(decl->left, env).kind != AETHER_T_INT || !containsRewrite(c, decl->left)) return;
    AST *intType = newASTNode(AST_TYPE_IDENTIFIER, NULL);
    intType->token = newToken(TOKEN_IDENTIFIER, "int", decl->left->token ? decl->left->token->line : 0, 0);
    setTypeAST(intType, TYPE_INT64);
    if (decl->right) freeAST(decl->right);
    decl->right = NULL;
    setRight(decl, intType);
    setTypeAST(decl, TYPE_INT64);
    AetherType it;
    memset(&it, 0, sizeof(it));
    it.kind = AETHER_T_INT;
    for (int i = 0; i < decl->child_count; i++) {
        AST *v = decl->children[i];
        if (v && v->type == AST_VARIABLE) {
            setTypeAST(v, TYPE_INT64);
            if (v->token) aetherTypeEnvRebind(env, v->token->value, it);
        }
    }
}

/* ------------------------------------------------------------------ */
/* D8: array parameters (W8-09)                                        */
/* ------------------------------------------------------------------ */

static void logRow(const char *arm, int line, const char *a, const char *b, const char *c,
                   const char *verdict) {
    FILE *f = experimentLog();
    if (!f) return;
    const char *path = aetherSemanticGetSourcePath();
    fprintf(f, "%s\t%s\t%d\t%s\t%s\t%s\t%s\n", arm, path ? path : "-", line, a ? a : "-",
            b ? b : "-", c ? c : "-", verdict);
}

static int nodeLineOf(const AST *n) {
    for (int depth = 0; n && depth < 8; depth++) {
        if (n->token && n->token->line > 0) return n->token->line;
        n = n->left ? n->left : (n->child_count > 0 ? n->children[0] : NULL);
    }
    return 0;
}

static const char *varName(const AST *n) {
    return (n && n->type == AST_VARIABLE && n->token) ? n->token->value : NULL;
}

static int sameName(const char *a, const char *b) {
    return a && b && strcasecmp(a, b) == 0;
}

/* The array variable an lvalue chain `p[i][j]` is rooted at, and how many
 * ARRAY_ACCESS levels / indices sit above it. */
static const char *indexRoot(const AST *lv, int *indices) {
    int n = 0;
    while (lv && lv->type == AST_ARRAY_ACCESS) {
        n += lv->child_count > 0 ? lv->child_count : 1;
        lv = lv->left;
    }
    if (indices) *indices = n;
    return varName(lv);
}

/* `setlength(p, length(p))`: the un-alias copy the lowering splices after a
 * store and at function entry. A copy, never a write the caller could see. */
static int isUnaliasCopy(const AST *call, const char *param) {
    if (!call || call->type != AST_PROCEDURE_CALL || !call->token || !call->token->value ||
        strcasecmp(call->token->value, "setlength") != 0 || call->child_count != 2)
        return 0;
    const AST *len = call->children[1];
    return sameName(varName(call->children[0]), param) && len &&
           len->type == AST_PROCEDURE_CALL && len->token && len->token->value &&
           strcasecmp(len->token->value, "length") == 0 && len->child_count == 1 &&
           sameName(varName(len->children[0]), param);
}

/* `p[length(p) - k] = v`: the element store of a lowered `p = p + [v]`,
 * counted once with its setlength. */
static int isAppendStore(const AST *idx, const char *param) {
    if (!idx || idx->type != AST_BINARY_OP || !idx->token || idx->token->type != TOKEN_MINUS)
        return 0;
    const AST *len = idx->left;
    return len && len->type == AST_PROCEDURE_CALL && len->token && len->token->value &&
           strcasecmp(len->token->value, "length") == 0 && len->child_count == 1 &&
           sameName(varName(len->children[0]), param);
}

typedef struct {
    const char *name;
    int rank;
    int returned;
} ArrParam;

static void arrCollectReturns(const AST *n, ArrParam *ps, int np) {
    if (!n) return;
    if (n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) return;
    /* `ret p;`, a tuple slot `__aether_retobj_N.itemK = p;`, or the @post
     * lowering's `result = p;` */
    const char *v = NULL;
    if (n->type == AST_RETURN) v = varName(n->left);
    if (n->type == AST_ASSIGN && n->left && n->right) {
        const AST *lv = n->left;
        if ((lv->type == AST_FIELD_ACCESS && varName(lv->left) &&
             strncmp(varName(lv->left), "__aether_retobj_", 16) == 0) ||
            sameName(varName(lv), "result"))
            v = varName(n->right);
    }
    for (int i = 0; v && i < np; i++)
        if (sameName(ps[i].name, v)) ps[i].returned = 1;
    arrCollectReturns(n->left, ps, np);
    arrCollectReturns(n->right, ps, np);
    arrCollectReturns(n->extra, ps, np);
    for (int i = 0; i < n->child_count; i++) arrCollectReturns(n->children[i], ps, np);
}

typedef struct {
    AetherArraysRule rule;
    const char *fnKind;
} ArrCtx;

static void arrReport(const ArrCtx *c, const ArrParam *p, const char *kind, int line) {
    logRow("arr", line, p->name, kind, c->fnKind, p->returned ? "returned" : "lost");
    if (c->rule != AETHER_ARRAYS_VSTRICT || p->returned) return;
    /* The shipped ARR-001 already warns on a direct index write in a Void fn. */
    if (strcmp(c->fnKind, "void") == 0 && strcmp(kind, "index") == 0) return;
    char detail[320];
    snprintf(detail, sizeof(detail),
             "array parameter '%s' is written here (%s) but never returned, so the caller "
             "never sees the change; under V-strict this is an error. Return it and "
             "reassign (`xs = f(xs)`), or copy it to a local first.",
             p->name, kind);
    aetherSemanticReportCoded("ARR-001", "array-mutation", line, detail, 0);
}

static void arrScanWrites(const AST *n, const ArrCtx *c, ArrParam *ps, int np) {
    if (!n) return;
    if (n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) return;
    if (n->type == AST_ASSIGN && n->left) {
        const AST *lv = n->left;
        int indices = 0;
        const char *root = NULL;
        const char *kind = NULL;
        if (lv->type == AST_ARRAY_ACCESS) {
            root = indexRoot(lv, &indices);
            kind = indices > 1 ? "nested" : "index";
            if (indices == 1 && lv->child_count == 1 && isAppendStore(lv->children[0], root))
                kind = NULL; /* counted with the append's setlength */
        } else if (lv->type == AST_VARIABLE) {
            root = varName(lv);
            kind = "reassign";
        }
        for (int i = 0; kind && i < np; i++) {
            if (!sameName(ps[i].name, root)) continue;
            if (indices == 1 && ps[i].rank > 1) kind = "nested"; /* a whole row of a grid */
            arrReport(c, &ps[i], kind, nodeLineOf(n));
        }
    }
    if (n->type == AST_PROCEDURE_CALL && n->token && n->token->value &&
        strcasecmp(n->token->value, "setlength") == 0 && n->child_count >= 1) {
        int indices = 0;
        const char *root = indexRoot(n->children[0], &indices);
        for (int i = 0; root && i < np; i++) {
            if (!sameName(ps[i].name, root) || isUnaliasCopy(n, ps[i].name)) continue;
            arrReport(c, &ps[i], indices > 0 ? "nested" : "resize", nodeLineOf(n));
        }
    }
    arrScanWrites(n->left, c, ps, np);
    arrScanWrites(n->right, c, ps, np);
    arrScanWrites(n->extra, c, ps, np);
    for (int i = 0; i < n->child_count; i++) arrScanWrites(n->children[i], c, ps, np);
}

static int arrayTypeRank(const AST *typeNode) {
    int r = 0;
    while (typeNode && typeNode->type == AST_ARRAY_TYPE) {
        r++;
        typeNode = typeNode->right;
    }
    return r ? r : 1;
}

static void arrWalkFunctions(const AST *n, AetherArraysRule rule) {
    if (!n) return;
    if ((n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) &&
        (n->right || n->extra)) {
        ArrParam ps[64];
        int np = 0;
        for (int i = 0; i < n->child_count && np < 64; i++) {
            const AST *d = n->children[i];
            if (!d || d->type != AST_VAR_DECL || d->var_type != TYPE_ARRAY) continue;
            for (int j = 0; j < d->child_count && np < 64; j++) {
                const char *nm = varName(d->children[j]);
                if (!nm) continue;
                ps[np].name = nm;
                ps[np].rank = arrayTypeRank(d->right);
                ps[np].returned = 0;
                np++;
            }
        }
        const AST *body = n->type == AST_FUNCTION_DECL ? n->extra : n->right;
        if (np > 0 && body) {
            ArrCtx c;
            c.rule = rule;
            c.fnKind = n->type == AST_FUNCTION_DECL ? "value" : "void";
            arrCollectReturns(body, ps, np);
            arrScanWrites(body, &c, ps, np);
        }
    }
    arrWalkFunctions(n->left, rule);
    arrWalkFunctions(n->right, rule);
    arrWalkFunctions(n->extra, rule);
    for (int i = 0; i < n->child_count; i++) arrWalkFunctions(n->children[i], rule);
}

/* arrays=ref: an rvalue cannot bind to a by-reference parameter, so each one
 * moves into a temporary declared just before its statement. A call in a
 * `while` condition is re-evaluated each turn and cannot be hoisted; it is
 * logged unhoistable and the compiler's error stands. */
typedef struct {
    const AST **fns;
    unsigned long long *resizes; /* per fn: bit k = by-ref param k is setlength'd */
    int count, cap;
    const char **consts;
    int nconst, constCap;
    int nextTemp;
} RefCtx;

/* Does `body` setlength (resize, append, un-alias) the array named `name`? */
static int refBodyResizes(const AST *n, const char *name) {
    if (!n || n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) return 0;
    if (n->type == AST_PROCEDURE_CALL && n->token && n->token->value &&
        strcasecmp(n->token->value, "setlength") == 0 && n->child_count >= 1 &&
        sameName(indexRoot(n->children[0], NULL), name))
        return 1;
    if (refBodyResizes(n->left, name) || refBodyResizes(n->right, name) ||
        refBodyResizes(n->extra, name))
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (refBodyResizes(n->children[i], name)) return 1;
    return 0;
}

/* The VM cannot setlength a literal-built (static) array through a
 * reference ("SetLength expects a string or dynamic array target"), so a call
 * site normalises the caller's storage before passing it to a parameter the
 * callee resizes. Bit k: parameter slot k. */
static unsigned long long refResizeMask(const AST *fn) {
    unsigned long long mask = 0;
    const AST *body = fn->type == AST_FUNCTION_DECL ? fn->extra : fn->right;
    int pi = 0;
    for (int i = 0; i < fn->child_count && pi < 64; i++) {
        const AST *d = fn->children[i];
        if (!d || d->type != AST_VAR_DECL) continue;
        if (d->by_ref && d->child_count > 0 && body && refBodyResizes(body, varName(d->children[0])))
            mask |= 1ULL << pi;
        pi++;
    }
    return mask;
}

static void refIndex(RefCtx *c, const AST *n, int topLevel) {
    if (!n) return;
    if (n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) {
        for (int i = 0; i < n->child_count; i++) {
            const AST *d = n->children[i];
            if (d && d->type == AST_VAR_DECL && d->by_ref) {
                if (c->count == c->cap) {
                    int cap = c->cap ? c->cap * 2 : 32;
                    const AST **g = (const AST **)realloc((void *)c->fns, (size_t)cap * sizeof(*g));
                    unsigned long long *m = (unsigned long long *)realloc(
                        c->resizes, (size_t)cap * sizeof(*m));
                    if (g) c->fns = g;
                    if (m) c->resizes = m;
                    if (!g || !m) return;
                    c->cap = cap;
                }
                c->resizes[c->count] = refResizeMask(n);
                c->fns[c->count++] = n;
                break;
            }
        }
        topLevel = 0;
    }
    if (topLevel && n->type == AST_CONST_DECL && n->token && n->token->value) {
        if (c->nconst == c->constCap) {
            int cap = c->constCap ? c->constCap * 2 : 32;
            const char **g = (const char **)realloc((void *)c->consts, (size_t)cap * sizeof(*g));
            if (!g) return;
            c->consts = g;
            c->constCap = cap;
        }
        c->consts[c->nconst++] = n->token->value;
    }
    refIndex(c, n->left, topLevel);
    refIndex(c, n->right, topLevel);
    refIndex(c, n->extra, topLevel);
    for (int i = 0; i < n->child_count; i++) refIndex(c, n->children[i], topLevel);
}

static const AST *refCallee(const RefCtx *c, const AST *call, unsigned long long *resizes) {
    const char *name = (call && call->token) ? call->token->value : NULL;
    if (!name) return NULL;
    /* A body-carrying definition knows what it resizes; its prototype does not. */
    for (int i = c->count - 1; i >= 0; i--) {
        if (c->fns[i]->token && sameName(c->fns[i]->token->value, name) &&
            (c->fns[i]->right || c->fns[i]->extra)) {
            if (resizes) *resizes = c->resizes[i];
            return c->fns[i];
        }
    }
    return NULL;
}

static int refIsLvalue(const RefCtx *c, const AST *a) {
    if (!a) return 0;
    if (a->type == AST_ARRAY_ACCESS || a->type == AST_FIELD_ACCESS) return 1;
    if (a->type != AST_VARIABLE) return 0;
    for (int i = 0; i < c->nconst; i++)
        if (sameName(c->consts[i], varName(a))) return 0;
    return 1;
}

/* Hoists rvalue by-ref arguments of calls inside the expression parts of
 * `stmt` into `out` (VAR_DECLs to insert before it). Does not descend into
 * nested statement bodies; those are visited as statements of their own. */
static void refHoistIn(RefCtx *c, AST *n, AST **out, int *nout, int maxOut, int inLoopCond) {
    if (!n) return;
    switch (n->type) {
        case AST_COMPOUND: case AST_FUNCTION_DECL: case AST_PROCEDURE_DECL:
            return;
        case AST_IF:
            refHoistIn(c, n->left, out, nout, maxOut, inLoopCond);
            return;
        case AST_WHILE: case AST_REPEAT:
            refHoistIn(c, n->left, out, nout, maxOut, 1);
            return;
        default:
            break;
    }
    if (n->type == AST_PROCEDURE_CALL) {
        unsigned long long resizes = 0;
        const AST *fn = refCallee(c, n, &resizes);
        if (fn) {
            int pi = 0;
            for (int i = 0; i < fn->child_count; i++) {
                const AST *d = fn->children[i];
                if (!d || d->type != AST_VAR_DECL) continue;
                int normalise = pi < 64 && (resizes & (1ULL << pi)) != 0;
                if (pi < n->child_count && d->by_ref && normalise && refIsLvalue(c, n->children[pi]) &&
                    !inLoopCond && *nout < maxOut) {
                    out[(*nout)++] = buildArrayUnaliasStmt(n->children[pi], nodeLineOf(n->children[pi]));
                }
                if (pi < n->child_count && d->by_ref && !refIsLvalue(c, n->children[pi])) {
                    AST *arg = n->children[pi];
                    char idx[16];
                    snprintf(idx, sizeof(idx), "%d", pi);
                    if (inLoopCond || *nout >= maxOut) {
                        logRow("arr-ref", nodeLineOf(arg), fn->token->value, idx, NULL, "unhoistable");
                    } else {
                        char name[48];
                        int line = nodeLineOf(arg);
                        snprintf(name, sizeof(name), "__aether_ref_%d", c->nextTemp++);
                        AST *decl = newASTNode(AST_VAR_DECL, NULL);
                        AST *var = newASTNode(AST_VARIABLE, NULL);
                        var->token = newToken(TOKEN_IDENTIFIER, name, line, 0);
                        setTypeAST(var, d->var_type);
                        addChild(decl, var);
                        setLeft(decl, arg);
                        if (d->right) setRight(decl, copyAST(d->right));
                        setTypeAST(decl, d->var_type);
                        AST *ref = newASTNode(AST_VARIABLE, NULL);
                        ref->token = newToken(TOKEN_IDENTIFIER, name, line, 0);
                        setTypeAST(ref, d->var_type);
                        n->children[pi] = ref;
                        ref->parent = n;
                        out[(*nout)++] = decl;
                        if (normalise && *nout < maxOut) out[(*nout)++] = buildArrayUnaliasStmt(ref, line);
                        logRow("arr-ref", line, fn->token->value, idx, NULL, "hoisted");
                    }
                }
                pi++;
            }
        }
    }
    refHoistIn(c, n->left, out, nout, maxOut, inLoopCond);
    refHoistIn(c, n->right, out, nout, maxOut, inLoopCond);
    refHoistIn(c, n->extra, out, nout, maxOut, inLoopCond);
    for (int i = 0; i < n->child_count; i++) refHoistIn(c, n->children[i], out, nout, maxOut, inLoopCond);
}

static void refHoistBlocks(RefCtx *c, AST *n) {
    if (!n) return;
    if (n->type == AST_COMPOUND) {
        for (int i = 0; i < n->child_count; i++) {
            AST *decls[32];
            int nd = 0;
            refHoistIn(c, n->children[i], decls, &nd, 32, 0);
            for (int k = 0; k < nd; k++) {
                addChild(n, NULL); /* grow by one slot */
                for (int j = n->child_count - 1; j > i; j--) n->children[j] = n->children[j - 1];
                n->children[i] = decls[k];
                decls[k]->parent = n;
                i++;
            }
        }
    }
    refHoistBlocks(c, n->left);
    refHoistBlocks(c, n->right);
    refHoistBlocks(c, n->extra);
    for (int i = 0; i < n->child_count; i++) refHoistBlocks(c, n->children[i]);
}

/* A by-reference array parameter read as a whole value (`let c = xs;`,
 * `ret xs;`, `t.item0 = xs;`) yields the reference itself in the VM, and the
 * un-alias step after the copy then fails ("SetLength expects a string or
 * dynamic array target"). The ref arm gives those reads value semantics, as
 * by-ref languages do for `let`: an element-wise copy into a temporary. */
static AST *xVar(const char *name, VarType vt, int line) {
    AST *v = newASTNode(AST_VARIABLE, NULL);
    v->token = newToken(TOKEN_IDENTIFIER, name, line, 0);
    setTypeAST(v, vt);
    return v;
}

static AST *xInt(long n, int line) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", n);
    AST *v = newASTNode(AST_NUMBER, NULL);
    v->token = newToken(TOKEN_INTEGER_CONST, buf, line, 0);
    v->i_val = (int)n;
    setTypeAST(v, TYPE_INT64);
    return v;
}

static AST *xLength(const char *name, int line) {
    AST *c = newASTNode(AST_PROCEDURE_CALL, NULL);
    c->token = newToken(TOKEN_IDENTIFIER, "length", line, 0);
    addChild(c, xVar(name, TYPE_ARRAY, line));
    setTypeAST(c, TYPE_INTEGER);
    return c;
}

static AST *xBin(TokenType tt, const char *lex, AST *l, AST *r, VarType vt, int line) {
    AST *b = newASTNode(AST_BINARY_OP, NULL);
    b->token = newToken(tt, lex, line, 0);
    setLeft(b, l);
    setRight(b, r);
    setTypeAST(b, vt);
    return b;
}

static AST *xAssignStmt(AST *lhs, AST *rhs, int line) {
    AST *a = newASTNode(AST_ASSIGN, NULL);
    a->token = newToken(TOKEN_ASSIGN, "=", line, 0);
    setLeft(a, lhs);
    setRight(a, rhs);
    AST *st = newASTNode(AST_EXPR_STMT, NULL);
    st->token = newToken(TOKEN_ASSIGN, "=", line, 0);
    setLeft(st, a);
    return st;
}

static AST *xIntDecl(const char *name, AST *init, int line) {
    AST *t = newASTNode(AST_TYPE_IDENTIFIER, NULL);
    t->token = newToken(TOKEN_IDENTIFIER, "int", line, 0);
    setTypeAST(t, TYPE_INT64);
    AST *d = newASTNode(AST_VAR_DECL, NULL);
    addChild(d, xVar(name, TYPE_INT64, line));
    setLeft(d, init);
    setRight(d, t);
    setTypeAST(d, TYPE_INT64);
    return d;
}

/* Appends to `out`: `let tmp: T[] = []; setlength(tmp, length(src));
 * let i = 0; while i < length(src) { tmp[i] = src[i]; i = i + 1; }`. */
static int refBuildCopy(const char *tmp, const char *idx, const char *src, const AST *typeNode,
                        int line, AST **out, int max) {
    if (max < 4) return 0;
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, xVar(tmp, TYPE_ARRAY, line));
    setLeft(decl, newASTNode(AST_ARRAY_LITERAL, NULL));
    setTypeAST(decl->left, TYPE_ARRAY);
    if (typeNode) setRight(decl, copyAST((AST *)typeNode));
    setTypeAST(decl, TYPE_ARRAY);
    AST *sl = newASTNode(AST_PROCEDURE_CALL, NULL);
    sl->token = newToken(TOKEN_IDENTIFIER, "setlength", line, 0);
    addChild(sl, xVar(tmp, TYPE_ARRAY, line));
    addChild(sl, xLength(src, line));
    setTypeAST(sl, TYPE_VOID);
    AST *slStmt = newASTNode(AST_EXPR_STMT, NULL);
    slStmt->token = newToken(TOKEN_IDENTIFIER, "setlength", line, 0);
    setLeft(slStmt, sl);
    AST *dst = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(dst, xVar(tmp, TYPE_ARRAY, line));
    addChild(dst, xVar(idx, TYPE_INT64, line));
    AST *from = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(from, xVar(src, TYPE_ARRAY, line));
    addChild(from, xVar(idx, TYPE_INT64, line));
    AST *body = newASTNode(AST_COMPOUND, NULL);
    addChild(body, xAssignStmt(dst, from, line));
    addChild(body, xAssignStmt(xVar(idx, TYPE_INT64, line),
                               xBin(TOKEN_PLUS, "+", xVar(idx, TYPE_INT64, line), xInt(1, line),
                                    TYPE_INT64, line), line));
    AST *loop = newASTNode(AST_WHILE, NULL);
    setLeft(loop, xBin(TOKEN_LESS, "<", xVar(idx, TYPE_INT64, line), xLength(src, line),
                       TYPE_BOOLEAN, line));
    setRight(loop, body);
    out[0] = decl;
    out[1] = slStmt;
    out[2] = xIntDecl(idx, xInt(0, line), line);
    out[3] = loop;
    return 4;
}

typedef struct {
    const char *names[64];
    const AST *types[64];
    int n;
} RefParams;

static int refParamIndex(const RefParams *rp, const AST *v) {
    const char *nm = varName(v);
    for (int i = 0; nm && i < rp->n; i++)
        if (sameName(rp->names[i], nm)) return i;
    return -1;
}

/* The whole-value read slot of statement `s`, or NULL. */
static AST **refValueSlot(AST *s) {
    if (!s) return NULL;
    if (s->type == AST_VAR_DECL || s->type == AST_RETURN) return &s->left;
    if (s->type == AST_EXPR_STMT && s->left && s->left->type == AST_ASSIGN) s = s->left;
    if (s->type == AST_ASSIGN && s->token && s->token->type == TOKEN_ASSIGN) return &s->right;
    return NULL;
}

static void refCopyReads(RefCtx *c, AST *n, const RefParams *rp) {
    if (!n || n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) return;
    if (n->type == AST_COMPOUND) {
        for (int i = 0; i < n->child_count; i++) {
            AST **slot = refValueSlot(n->children[i]);
            int k = slot ? refParamIndex(rp, *slot) : -1;
            if (k < 0) continue;
            /* `p = p` / a param assigned to itself needs no copy. */
            AST *s = n->children[i];
            if (s->type == AST_EXPR_STMT) s = s->left;
            if (s->type == AST_ASSIGN && refParamIndex(rp, s->left) == k) continue;
            char tmp[48], idx[48];
            int line = nodeLineOf(*slot);
            snprintf(tmp, sizeof(tmp), "__aether_refcopy_%d", c->nextTemp);
            snprintf(idx, sizeof(idx), "__aether_refcopy_i_%d", c->nextTemp++);
            AST *stmts[4];
            int ns = refBuildCopy(tmp, idx, rp->names[k], rp->types[k], line, stmts, 4);
            freeAST(*slot);
            *slot = xVar(tmp, TYPE_ARRAY, line);
            (*slot)->parent = n->children[i];
            for (int q = 0; q < ns; q++) {
                addChild(n, NULL);
                for (int j = n->child_count - 1; j > i; j--) n->children[j] = n->children[j - 1];
                n->children[i] = stmts[q];
                stmts[q]->parent = n;
                i++;
            }
            logRow("arr-ref", line, rp->names[k], "-", NULL, "copied-read");
        }
    }
    refCopyReads(c, n->left, rp);
    refCopyReads(c, n->right, rp);
    refCopyReads(c, n->extra, rp);
    for (int i = 0; i < n->child_count; i++) refCopyReads(c, n->children[i], rp);
}

static void refWalkFunctions(RefCtx *c, AST *n) {
    if (!n) return;
    if ((n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) && (n->right || n->extra)) {
        RefParams rp;
        rp.n = 0;
        for (int i = 0; i < n->child_count && rp.n < 64; i++) {
            const AST *d = n->children[i];
            if (d && d->type == AST_VAR_DECL && d->by_ref && d->child_count > 0 && varName(d->children[0])) {
                rp.names[rp.n] = varName(d->children[0]);
                rp.types[rp.n] = d->right;
                rp.n++;
            }
        }
        if (rp.n > 0) refCopyReads(c, n->type == AST_FUNCTION_DECL ? n->extra : n->right, &rp);
    }
    refWalkFunctions(c, n->left);
    refWalkFunctions(c, n->right);
    refWalkFunctions(c, n->extra);
    for (int i = 0; i < n->child_count; i++) refWalkFunctions(c, n->children[i]);
}

void aetherRunExperimentsBeforeRea(AST *root) {
    if (!root || aetherExperiment()->arrays != AETHER_ARRAYS_REF) return;
    RefCtx c;
    memset(&c, 0, sizeof(c));
    refIndex(&c, root, 1);
    if (c.count > 0) {
        refHoistBlocks(&c, root);
        refWalkFunctions(&c, root);
    }
    free((void *)c.fns);
    free(c.resizes);
    free((void *)c.consts);
    if (g_log) fflush(g_log);
}

void aetherRunExperiments(AST *root) {
    const AetherExperiment *x = aetherExperiment();
    if (!root || (!x->any && !experimentLog())) return;
    if (x->arrays == AETHER_ARRAYS_VSTRICT || (x->arrays == AETHER_ARRAYS_VALUE && experimentLog())) {
        arrWalkFunctions(root, x->arrays);
        int modules = aetherGetLoadedModuleCount();
        for (int i = 0; i < modules; i++) {
            AST *m = aetherGetModuleAST(i);
            if (m && m != root) arrWalkFunctions(m, x->arrays);
        }
    }
    DivCtx dc;
    memset(&dc, 0, sizeof(dc));
    dc.rule = x->div;
    AetherTypedVisitor v;
    memset(&v, 0, sizeof(v));
    v.ctx = &dc;
    v.onExprPost = divPost;
    v.onDecl = divDecl;
    aetherTypedWalk(root, &v);
    free((void *)dc.rewritten);
    if (g_log) fflush(g_log);
}
