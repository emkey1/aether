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
            "div=current|int|intplus|realplus|ctx.\n",
            item);
    exit(2);
}

static void loadExperiment(void) {
    static const char *const kDiv[] = {"current", "int", "intplus", "realplus", "ctx"};
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
        }
        if (v < 0) {
            *eq = '=';
            usageError(item);
        }
    }
    free(copy);
    g_experiment.any = g_experiment.div != AETHER_DIV_CURRENT;
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

void aetherRunExperiments(AST *root) {
    const AetherExperiment *x = aetherExperiment();
    if (!root || (!x->any && !experimentLog())) return;
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
