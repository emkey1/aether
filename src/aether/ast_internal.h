/*
 * ast_internal.h -- state and helpers shared by the Aether AST front end.
 *
 * The recursive-descent parser is split across four translation units that
 * share the AetherParser state defined here:
 *
 *   ast_parser.c  the recursive-descent parse, token layer and the semantic
 *                 side-registries (parseAetherAst is the entry point)
 *   ast_lower.c   desugarings: array append/concat/slice, tuple returns and
 *                 destructuring, record-init, foreach, range loops, par blocks
 *   ast_types.c   type-name inference (inferLetTypeName and friends)
 *   ast_checks.c  front-end checks over the parsed AST (member calls, FLOW-001,
 *                 PAR-001, ARR-002)
 *
 * Internal to src/aether; not part of the front end's public interface
 * (that is parser.h).
 */

#ifndef AETHER_AST_INTERNAL_H
#define AETHER_AST_INTERNAL_H

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

/* Provided by core/utils.c */
Token *newToken(TokenType type, const char *value, int line, int column);
/* Compile-time constant folding, declared in compiler/compiler.h. Aether links
 * rea's parser which already pulls these in; declaring them here avoids adding a
 * compiler header dependency to the front end. evaluateCompileTimeValue is
 * aliased to rea_evaluateCompileTimeValue under FRONTEND_REA (see
 * common/frontend_symbol_aliases.h, pulled in transitively). */
void addCompilerConstant(const char *name_original_case, const Value *value, int line);
Value evaluateCompileTimeValue(AST *node);

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
    int nextLoopId;            /* monotonic counter for foreach / step temp names */
    /* Missing-operand diagnostics (aetherReportMissingExpr). A binary or unary
     * operator records its text and the start of the token right after it, so
     * an operand that is not there reads "expected an expression after '+'".
     * stmtStartAt is the first token of an expression statement: a token that
     * cannot start a statement keeps parseBlock's "expected a statement".
     * detachedText marks a parseExprFromText sub-parser, whose caller reports. */
    const char *exprAfterOpText;   /* operator lexeme (not NUL-terminated) */
    int exprAfterOpLen;
    const char *exprAfterOpAt;
    int exprAfterOpLine;
    const char *stmtStartAt;
    bool detachedText;
    /* The token before `current` (line, text), for the D39 juxtaposition
     * arm (AETHER_EXPERIMENT=tail=reject). Set by aetherAdvance. */
    int prevLine;
    const char *prevStart;
    int prevLength;
    /* DIV-002 (W8-14): whether the last token the lexer produced ends an
     * expression, and the furthest `//` already judged (a speculative parse
     * that rewinds the lexer must not report the same comment twice). */
    bool rawPrevIsTail;
    const char *slashJudgedAt;
} AetherParser;

/* One right-hand operand of a left-nested array-`+` chain, in APPLY order.
 * Exactly one of the two shapes is populated: `items` for an array literal
 * (`+ [1, 2]`), `other` for an array-valued expression (`+ ys`, `+ f()`). */
typedef struct AetherConcatOperand {
    AST **items;          /* append shape: literal elements, owned */
    int itemCount;
    AST *other;           /* concat shape: array-valued expression, owned */
    char *otherTypeName;  /* concat shape: inferred type name, owned */
    int line;
} AetherConcatOperand;

/* One record-typed argument variable already passed to a `par` branch, for
 * the PAR-001 shared-record guard (aetherCheckParSharedRecords). */
typedef struct {
    const char *name;
    int branch;
} AetherParSharedRec;

/* ---- Defined in ast_parser.c ---------------------------------------- */

int aetherDiagf(const char *fmt, ...);
void aetherReportMissingExpr(AetherParser *p, const char *context);
void aetherNoteOperator(AetherParser *p, const ReaToken *op);
void reportAetherAstError(const char *path, int line, const char *kind,
                          const char *detail, const char *hint);
void bindingTableSet(AetherBindingTable *t, const char *name, const char *typeName);
const char *bindingTableGet(const AetherBindingTable *t, const char *name, size_t len);
const AetherTupleSig *tupleTableGet(const AetherTupleTable *t, const char *name, size_t len);
void aetherTupleSyntheticTypeName(char *buf, size_t bufSize, int typeId);
void aetherAdvance(AetherParser *p);
void pushPendingObjLit(AetherParser *p, AST *hoisted);
bool isAetherKeyword(const ReaToken *t, const char *kw);
bool aetherTokenIsIdentifierLike(const ReaToken *t);
bool mapAetherType(const char *name, size_t len,
                   const char **outReaName, VarType *outType);
void releaseTransientTypeNode(AST *resolved);
AST *buildTypeNode(const char *name, size_t len, int line, VarType *outType);
AST *buildTypeNodeFromName(const char *name, size_t len, int line, VarType *outType);
Token *copyNameToken(AetherParser *p);
AST *parseRecordInitBlock(AetherParser *p);
AST *parseAdd(AetherParser *p);
AST *parseExpr(AetherParser *p);
AST *buildContractGuard(AetherParser *p, const char *exprText,
                       const char *kind, const char *fnName, int line);
AST *parseStatement(AetherParser *p);
/* AETHER_EXPERIMENT=tail=reject (D39 census): true, after reporting SYN-001,
 * when `current` starts another expression on the line of the token before
 * it with no `;` between them (`let t: Int = price qty;`). False otherwise,
 * and always false with the flag off. */
bool aetherTailRejectJuxtaposed(AetherParser *p);
AST *parseBlock(AetherParser *p);

/* ---- Defined in ast_lower.c ----------------------------------------- */

AST *buildObjectInitDecl(Token *nameTok, AST *typeNode, VarType vtype,
                         const char *typeName, AST *lit, int line);
bool aetherIsLValueChain(const AST *node);
bool aetherLValueEqual(const AST *a, const AST *b);
bool aetherExprReadsLValue(const AST *expr, const AST *target);
AST *buildArrayUnaliasStmt(const AST *target, int line);
int aetherDeclLine(const AST *decl);
void aetherAlignSpliceLines(AST *outer, AST *anchor);
bool aetherArrayInitMayAlias(const AST *init);
AST *buildArrayAppend(AST *assign, AST *target, AST **items, int itemCount, int line, AST *src);
AST *buildArraySlice(AetherParser *p, AST *base, AST *lo, AST *hi, int line);
AST *buildVarRef(const char *name, VarType vt, int line);
AST *buildArrayConcat(AetherParser *p, AST *assign, AST *target, AST *other,
                      char *otherTypeName, int line, AST *src);
int aetherConcatChainLength(AetherParser *p, const AST *expr);
bool aetherCollectConcatChain(AetherParser *p, AST **initInOut,
                              AetherConcatOperand **outOps, int *outCount);
void aetherFreeConcatOperands(AetherConcatOperand *ops, int count, bool freeOwned);
bool aetherEmitConcatOperand(AetherParser *p, AST *dest, AST *var,
                             AetherConcatOperand *op);
AST *parseLetTupleDestructure(AetherParser *p, int kwLine);
AST *parseRet(AetherParser *p);
AST *parseLoopRange(AetherParser *p);
AST *buildWhile(AST *cond, AST *body);
AST *parseParBlock(AetherParser *p);

/* ---- Defined in ast_types.c ----------------------------------------- */

VarType promoteRealBinaryType(VarType a, VarType b);
VarType promoteIntegralBinaryType(VarType a, VarType b);
VarType inferStringLiteralType(const char *text, size_t len);
VarType resolveConditionalType(AST *thenExpr, AST *elseExpr);
const char *aetherTypeNameForVarType(VarType vt);
char *inferLetTypeName(AetherParser *p, AST *init);
bool aetherTypeNameIsArray(const char *name);
int aetherTypeNameRank(const char *name);

/* ---- Defined in ast_checks.c ---------------------------------------- */

bool aetherCheckArrayRankIndex(AetherParser *p, AST *node, int openLine);
bool aetherCheckParSharedRecords(AetherParser *p, AST *call, int handle, int callLine,
                                 AetherParSharedRec *sharedRecs, int *sharedRecCount,
                                 int sharedRecCap);
bool astHasValueReturn(const AST *node);
bool astBlockHasFallthroughStmt(const AST *block);
int aetherCheckMemberCalls(AST *node, AST *decls);

/* ---- Tuple annotations (ast_parser.c, W4-25) ------------------------- */

bool aetherParseTupleTypeList(const char *start, const char *end, char ***outItems,
                              size_t *outCount);
bool aetherTupleItemsMatch(char **ann, size_t annCount, char **sig, size_t sigCount);
const char *aetherFormatTupleItems(char **items, size_t count, char *buf, size_t n);
void aetherFreeTupleItems(char **items, size_t count);
bool aetherCheckPrintPlaceholders(AetherParser *p, AST *call, const char *surface, int line);
bool aetherIsBuiltinValueTypeName(const char *t);
bool aetherCheckBuiltinMethod(AetherParser *p, const char *recvType, const char *method,
                              bool isCall, bool knownCallable, int line);

#endif /* AETHER_AST_INTERNAL_H */
