#ifndef PSCAL_AETHER_TYPES_H
#define PSCAL_AETHER_TYPES_H

/*
 * The Aether type oracle (W7-24a): aetherTypeOf answers "what Aether type does
 * this expression have" from facts Aether owns -- literal kinds, declared and
 * inferred binding types (the VAR_DECL type nodes), user function and method
 * signatures (including loaded modules), record field lists, a builtin return
 * table, and the operator promotion rules. It never reads an expression node's
 * var_type: annotateTypes' builtin answers were rejected (CHANGELOG 2026-07-26-4:
 * min/max as REAL, sqr as VOID), and parse-time variable types are unknown.
 *
 * aetherTypedWalk visits a program in source order with a scope environment
 * and, for every expression, the use its value is put to (an Int sink, an
 * index, a `*` operand, a print argument, ...). Rules hosted on the pass are
 * visitors. The core ships with no rules; the only built-in visitor is the
 * dump behind AETHER_DUMP_TYPES.
 */

#include <stddef.h>

#include "ast/ast.h"

typedef enum {
    AETHER_T_UNKNOWN = 0,
    AETHER_T_VOID,
    AETHER_T_INT,
    AETHER_T_REAL,
    AETHER_T_TEXT,
    AETHER_T_CHAR,
    AETHER_T_BOOL,
    AETHER_T_NIL,
    AETHER_T_HANDLE, /* ToonDoc / ToonNode / MStream / File; lowered to Int or a stream */
    AETHER_T_RECORD, /* a `type`, a tuple (__AetherTupleN) */
    AETHER_T_ARRAY
} AetherTypeKind;

typedef struct {
    AetherTypeKind kind;
    AetherTypeKind elem; /* AETHER_T_ARRAY: element kind (UNKNOWN for `[]`) */
    int rank;            /* AETHER_T_ARRAY: number of [] */
    const char *name;    /* RECORD, HANDLE, or the element record of an array */
} AetherType;

/* How the value of an expression is used by its parent. */
typedef enum {
    AETHER_USE_NONE = 0,     /* statement position, or not classified        */
    AETHER_USE_SINK,         /* let/assign/ret/param of a known declared type */
    AETHER_USE_INFERRED,     /* initializer of an inferred `let` / `const`    */
    AETHER_USE_INDEX,        /* array or Text index                           */
    AETHER_USE_RANGE_BOUND,  /* upper bound of `loop i in a..b`               */
    AETHER_USE_MOD,          /* operand of `%` / `mod`                        */
    AETHER_USE_INT_DIV,      /* operand of `div`                              */
    AETHER_USE_MUL,          /* operand of `*`                                */
    AETHER_USE_DIV,          /* operand of `/`                                */
    AETHER_USE_COMPARE,      /* operand of a comparison                       */
    AETHER_USE_PRINT,        /* print/println argument                        */
    AETHER_USE_CONDITION,    /* if/while condition                            */
    AETHER_USE_BUILTIN_ARG,  /* argument of a builtin with no known parameter */
    AETHER_USE_CALL_ARG,     /* argument of a user call with unknown params   */
    AETHER_USE_OTHER
} AetherUseKind;

typedef struct {
    AetherUseKind kind;
    AetherType want;       /* AETHER_USE_SINK: the declared type            */
    const char *sinkLabel; /* "let", "assign", "ret", "arg", "field", ...    */
} AetherUse;

typedef struct AetherTypeEnv AetherTypeEnv;

AetherType aetherTypeOf(const AST *expr, const AetherTypeEnv *env);

/* The Aether type a declaration's type node spells (VAR_DECL->right, a
 * FUNCTION_DECL return type, a record field). */
AetherType aetherTypeFromTypeNode(const AST *typeNode, VarType fallback);

/* An expression's type name for a hint, with no environment (W6-06); NULL
 * when the oracle cannot name it. */
const char *aetherHintTypeName(const AST *e, char *buf, size_t n);

/* "Int", "Real[]", "Point", "?" ... into buf. */
const char *aetherTypeFormat(AetherType t, char *buf, size_t n);
int aetherOracleIsInt(AetherType t);
int aetherOracleIsReal(AetherType t);

/* Agreement of a value's oracle type with a sink's declared type. */
typedef enum {
    AETHER_AGREE_EXACT = 0,
    AETHER_AGREE_WIDEN,   /* Int value into a Real sink                       */
    AETHER_AGREE_NARROW,  /* Real value into an Int sink (D1 sink coercion)    */
    AETHER_AGREE_COMPAT,  /* Char/Text, nil/record, handle/Int, [] into T[]    */
    AETHER_AGREE_UNKNOWN, /* the oracle has no answer for the value            */
    AETHER_AGREE_MISMATCH
} AetherAgreement;

AetherAgreement aetherTypeAgreement(AetherType want, AetherType have);
const char *aetherAgreementName(AetherAgreement a);

typedef struct {
    void *ctx;
    /* Called for every expression node before its children, with its use. */
    void (*onExpr)(void *ctx, AST *expr, const AetherUse *use, const AetherTypeEnv *env);
    /* Called for every expression node after its children were visited (a
     * visitor that rewrites a node sees its operands' rewrites first). */
    void (*onExprPost)(void *ctx, AST *expr, const AetherUse *use, const AetherTypeEnv *env);
    /* Called once per sink (a typed let, assign, ret, call argument). */
    void (*onSink)(void *ctx, const AST *site, const AST *value, const AetherUse *use,
                   const AetherTypeEnv *env);
    /* Called for every AST_VAR_DECL after its initializer was visited. */
    void (*onDecl)(void *ctx, AST *decl, const AetherTypeEnv *env);
} AetherTypedVisitor;

/* Walk `root` (the program) and every loaded module. */
void aetherTypedWalk(AST *root, const AetherTypedVisitor *visitor);

/* Re-binds `name` in the innermost scope that holds it (a visitor that retypes
 * a declaration keeps later lookups consistent). */
void aetherTypeEnvRebind(const AetherTypeEnv *env, const char *name, AetherType t);

/* The declared return type of the function being walked (UNKNOWN at top level). */
AetherType aetherTypeEnvReturnType(const AetherTypeEnv *env);

/* The rules hosted on the oracle (L1: W8-15, W4-19, W4-32). Coded errors only;
 * called by aetherPerformSemanticAnalysis after the experiment arms. */
void aetherTypedRules(AST *root);

/* The W7-24a pass: runs only when AETHER_DUMP_TYPES is set, and reports no
 * diagnostics. Called at the end of aetherPerformSemanticAnalysis. */
void aetherTypedPass(AST *root);

#endif
