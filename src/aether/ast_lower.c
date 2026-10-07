/*
 * ast_lower.c -- Aether AST desugarings (lowering).
 *
 * Array append/concat/slice, tuple returns and `let (a, b) = f();`
 * destructuring, record-literal initialisation, `ret` with @post or tuple
 * shapes, foreach and range loops, and `par` blocks: each builds the shared
 * pscal AST shape the rewriter used to emit. Split out of ast_parser.c; see
 * ast_internal.h.
 */

#include "aether/ast_internal.h"

static void buildArrayAppendSteps(const AST *target, AST *item, int line,
                                  AST **outSetlenStmt, AST **outIdxAssign);
static bool buildArrayConcatSteps(AetherParser *p, AST *dest, AST *target, AST *other,
                                  char *otherTypeName, int line);

/* Expand a typed object-literal initializer `let x: T = T { f: v, ... };` into
 * the rea shape the rewriter produces: an AST_VAR_DECL with init = AST_NEW(T)
 * (no record-init block) followed by one AST_ASSIGN(x.f = v) per field. The
 * caller passes the already-parsed AST_NEW `lit` (built from the bare `T { }`
 * form). Returns an AST_COMPOUND[ var-decl, x.f=v ... ], or NULL on mismatch. */
AST *buildObjectInitDecl(Token *nameTok, AST *typeNode, VarType vtype,
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
bool aetherIsLValueChain(const AST *node) {
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

bool aetherLValueEqual(const AST *a, const AST *b) {
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

/* Does `expr` read the lvalue `target` anywhere inside it?
 *
 * Matters for self-referencing concat where the destination is on the RIGHT:
 * `ys = [0] + ys;`. That lowers to `ys = [0];` followed by steps that copy
 * `ys` in -- but the copy has already clobbered it, so the result was `0 0`
 * instead of `0 1 2`. Silently wrong data, no diagnostic. (`xs = xs + [v];`,
 * the documented append idiom, is safe only because its copy `xs = xs` is a
 * no-op.) Callers use this to hoist `other` into a temp BEFORE the copy.
 *
 * Conservative on purpose: a false positive costs one redundant temp, a false
 * negative silently corrupts data. */
bool aetherExprReadsLValue(const AST *expr, const AST *target) {
    if (!expr || !target) return false;
    if (aetherLValueEqual(expr, target)) return true;
    /* A bare-name match anywhere is enough for the shapes that reach here;
     * comparing whole lvalue chains would miss `ys[i]` reading `ys`. */
    if (expr->type == AST_VARIABLE && target->type == AST_VARIABLE &&
        expr->token && target->token && expr->token->value && target->token->value &&
        strcmp(expr->token->value, target->token->value) == 0) {
        return true;
    }
    if (aetherExprReadsLValue(expr->left, target)) return true;
    if (aetherExprReadsLValue(expr->right, target)) return true;
    if (aetherExprReadsLValue(expr->extra, target)) return true;
    for (int i = 0; i < expr->child_count; i++) {
        if (aetherExprReadsLValue(expr->children[i], target)) return true;
    }
    return false;
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

/* Build `setlength(target, length(target));` as a statement -- the un-aliasing
 * step spliced after any array store whose source may share the VM-level
 * ArrayObj with another live variable. SetLength always allocates a fresh
 * ArrayObj and preserves contents (see pscal-core builtin.c, "Always allocate
 * a fresh ArrayObj wrapper rather than mutating `old` in place"), so this
 * no-op resize is exactly a contents-preserving deep copy of the top level.
 * Required because the VM's dynamic arrays are reference types by design
 * (valueEnsureUnique's is_dynamic exemption -- Free/Delphi semantics that
 * Pascal/Rea rely on), while Aether's contract is that arrays are VALUE
 * types on assignment and at the call boundary (the ARR-001 model). Literal
 * arrays are static ArrayObjs and already copy-on-write; without this step,
 * concat-/setlength-built arrays silently alias instead. */
AST *buildArrayUnaliasStmt(const AST *target, int line) {
    Token *slTok = newToken(TOKEN_IDENTIFIER, "setlength", line, 0);
    AST *setlen = newASTNode(AST_PROCEDURE_CALL, slTok);
    addChild(setlen, copyAST((AST *)target));
    addChild(setlen, buildLengthCall(target, line));
    setTypeAST(setlen, TYPE_VOID);
    AST *stmt = newASTNode(AST_EXPR_STMT, setlen->token);
    setLeft(stmt, setlen);
    return stmt;
}

/* Can an array-typed initializer/RHS expression yield a value that SHARES its
 * ArrayObj with another live variable? Variables, field reads, array-element
 * reads, and calls (a fn may `ret` a global or a record field's array) all
 * can; array literals, slices, and the append/concat expansions build fresh
 * storage. Internal `__aether_*` temps are rewriter-built fresh values whose
 * names user code can never touch again, so copying them would only double
 * the work the rewriter already did. */
bool aetherArrayInitMayAlias(const AST *init) {
    if (!init) return false;
    switch (init->type) {
        case AST_VARIABLE:
            if (!init->token || !init->token->value) return false;
            return strncmp(init->token->value, "__aether_", 9) != 0;
        case AST_FIELD_ACCESS:
        case AST_ARRAY_ACCESS:
        case AST_PROCEDURE_CALL:
            return true;
        default:
            return false;
    }
}

/* Expand `target = src + [items...]` into the copy step plus a
 * setlength+indexed-assign pair per item, the rewriter's self-reassignment
 * case (translate.c translateArrayAppendLine) generalized two ways: to any
 * `src` (not just `src == target`), and to an array literal of any length
 * (0, 1, or more), not just exactly one element:
 *     target = src;                       -- only when src != target
 *     setlength(target, length(target) + 1);
 *     target[length(target) - 1] = items[0];
 *     setlength(target, length(target) + 1);
 *     target[length(target) - 1] = items[1];
 *     ...
 * When `src` is NULL or structurally equal to `target` (the original,
 * narrower self-reassignment shape this function used to require), the copy
 * step is skipped -- `target` already holds the array to extend, exactly as
 * before. This is what lets `let b: Int[] = a + [3];` and `b = a + [3];`
 * (`b` a different, already-declared array than `a`) lower the same way
 * `xs = xs + [v]` always has, instead of falling through to a raw
 * `ARRAY + ARRAY_LITERAL` binary op the VM has no operator for (previously:
 * "Runtime Error: Operands must be numbers for arithmetic operation '+'...
 * Got ARRAY and ARRAY"). `itemCount == 0` (an empty literal, `src + []`) still
 * performs the copy (when needed) with no append steps, rather than crashing.
 * Returns an AST_COMPOUND splice (i_val==1) so parseBlock flattens it; the caller
 * has already verified the shape. `assign` is consumed (its target + the items
 * are reused/freed). `items` itself (the array of pointers, not its elements)
 * is only read here -- ownership stays with the caller. */
AST *buildArrayAppend(AST *assign, AST *target, AST **items, int itemCount, int line, AST *src) {
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

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */
    if (copyStmt) addChild(outer, copyStmt);
    for (int i = 0; i < itemCount; i++) {
        AST *setlenStmt = NULL, *idxAssign = NULL;
        buildArrayAppendSteps(target, items[i], line, &setlenStmt, &idxAssign);
        addChild(outer, setlenStmt);
        addChild(outer, idxAssign);
    }

    /* The original assign node's target was copied; release it along with the
     * now-detached `+` expression (items were moved out, so detach them first). */
    freeAST(assign);
    return outer;
}

/* Lower `base[lo..hi]` (slice sugar -- see the parsePostfix call site) into a
 * hoisted temp-array declaration plus a presize-and-copy loop, spliced into
 * the enclosing statement the same way buildObjectInitDecl/buildArrayAppend
 * splice theirs (an i_val==1 AST_COMPOUND, flushed by the parseStatement
 * wrapper):
 *
 *   let __aether_slice_N: T[] = [];
 *   setlength(__aether_slice_N, HI - LO);
 *   let __aether_slice_idx_N: Int = 0;
 *   while (__aether_slice_idx_N < (HI - LO)) {
 *       __aether_slice_N[__aether_slice_idx_N] = base[LO + __aether_slice_idx_N];
 *       __aether_slice_idx_N = __aether_slice_idx_N + 1;
 *   }
 *
 * The presize-then-index-assign shape mirrors buildArrayAppendSteps rather
 * than repeated `+ [item]` appends, for the same reason: it's the idiom the
 * rest of this file already uses for building arrays element-by-element.
 *
 * `base`, `lo`, `hi` are consumed (freed or moved into the result). Returns
 * an AST_VARIABLE referencing the temp slice (so the caller can keep
 * chaining postfix operations on it, exactly like the bare-object-literal
 * case in parsePrimary), or NULL on a hard failure (frees its inputs first).
 */
AST *buildArraySlice(AetherParser *p, AST *base, AST *lo, AST *hi, int line) {
    if (!base || !lo || !hi) {
        if (base) freeAST(base);
        if (lo) freeAST(lo);
        if (hi) freeAST(hi);
        return NULL;
    }

    char tempName[40];
    char idxName[40];
    snprintf(tempName, sizeof(tempName), "__aether_slice_%d", p->nextObjLitId);
    snprintf(idxName, sizeof(idxName), "__aether_slice_idx_%d", p->nextObjLitId);
    p->nextObjLitId++;

    /* Infer the slice's declared type from the base array's own type name
     * (e.g. base is "Int[]" -> the slice is also "Int[]"), reusing the same
     * inference `let x = e;` uses. Falls back to an untyped decl (var_type
     * TYPE_ARRAY, no type node -- same as an inferred `let x = [];` would
     * produce) if inference fails; the backend still handles a dynamically
     * typed empty array literal, just without the extra static-type info. */
    VarType sliceVtype = TYPE_ARRAY;
    AST *sliceTypeNode = NULL;
    /* inferLetTypeName only reads its argument (it never takes ownership), so
     * pass `base` directly -- the copy this used to make was leaked outright. */
    char *inferredName = inferLetTypeName(p, base);

    /* `s[a..b]` where `s` is a Text -> `copy(s, a, b - a)`.
     *
     * Slicing is the most natural way to reach for a substring, and now that
     * Text is 0-based this means exactly what `arr[a..b]` means: half-open,
     * from `a` up to but not including `b`. Since `copy`'s `start` is also
     * 0-based, the translation is direct and needs no index fixups.
     *
     * Unlike the array case below, nothing is hoisted: `copy` is an ordinary
     * expression, so the slice stays an expression and works anywhere a Text
     * does -- including the `ret s[a..b];` position that has no statement slot.
     * (Before Text was 0-based this form was rejected outright with a coded
     * TYPE-001 pointing at copy(); the rejection is what this replaces.) */
    if (inferredName && (strcmp(inferredName, "Text") == 0 ||
                         strcmp(inferredName, "str") == 0)) {
        free(inferredName);
        /* count = hi - lo, built before `lo` is handed to the call. */
        Token *minusTok = newToken(TOKEN_MINUS, "-", line, 0);
        AST *count = newASTNode(AST_BINARY_OP, minusTok);
        setLeft(count, hi);
        setRight(count, copyAST(lo));
        setTypeAST(count, TYPE_INTEGER);

        Token *copyTok = newToken(TOKEN_IDENTIFIER, "copy", line, 0);
        AST *call = newASTNode(AST_PROCEDURE_CALL, copyTok);
        addChild(call, base);
        addChild(call, lo);
        addChild(call, count);
        setTypeAST(call, TYPE_STRING);
        return call;
    }

    if (inferredName) {
        sliceTypeNode = buildTypeNodeFromName(inferredName, strlen(inferredName), line, &sliceVtype);
        if (!sliceTypeNode) {
            sliceVtype = TYPE_ARRAY;
        }
    }

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */

    /* let __aether_slice_N: T[] = []; */
    Token *sliceNameTok = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
    AST *sliceVar = newASTNode(AST_VARIABLE, sliceNameTok);
    setTypeAST(sliceVar, sliceVtype);
    AST *emptyLit = newASTNode(AST_ARRAY_LITERAL, NULL);
    setTypeAST(emptyLit, TYPE_ARRAY);
    AST *sliceDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(sliceDecl, sliceVar);
    setLeft(sliceDecl, emptyLit);
    setRight(sliceDecl, sliceTypeNode);
    setTypeAST(sliceDecl, sliceVtype);
    addChild(outer, sliceDecl);
    if (inferredName) {
        bindingTableSet(p->bindings, tempName, inferredName);
    }
    free(inferredName);

    /* HI - LO, built once and reused for both setlength's argument and the
     * while condition (copied each use; AST nodes aren't shared). */
    Token *widthMinusTok = newToken(TOKEN_MINUS, "-", line, 0);
    AST *widthExpr = newASTNode(AST_BINARY_OP, widthMinusTok);
    setLeft(widthExpr, copyAST(hi));
    setRight(widthExpr, copyAST(lo));
    setTypeAST(widthExpr, TYPE_INTEGER);

    /* setlength(__aether_slice_N, HI - LO); */
    Token *sliceRefTok1 = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
    AST *sliceRef1 = newASTNode(AST_VARIABLE, sliceRefTok1);
    setTypeAST(sliceRef1, sliceVtype);
    Token *slTok = newToken(TOKEN_IDENTIFIER, "setlength", line, 0);
    AST *setlen = newASTNode(AST_PROCEDURE_CALL, slTok);
    addChild(setlen, sliceRef1);
    addChild(setlen, widthExpr);
    setTypeAST(setlen, TYPE_VOID);
    AST *setlenStmt = newASTNode(AST_EXPR_STMT, setlen->token);
    setLeft(setlenStmt, setlen);
    addChild(outer, setlenStmt);

    /* let __aether_slice_idx_N: Int = 0; */
    Token *idxNameTok = newToken(TOKEN_IDENTIFIER, idxName, line, 0);
    AST *idxVar = newASTNode(AST_VARIABLE, idxNameTok);
    setTypeAST(idxVar, TYPE_INT64);
    Token *intTypeTok = newToken(TOKEN_IDENTIFIER, "int", line, 0);
    AST *intTypeNode = newASTNode(AST_TYPE_IDENTIFIER, intTypeTok);
    setTypeAST(intTypeNode, TYPE_INT64);
    AST *idxDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(idxDecl, idxVar);
    setLeft(idxDecl, buildIntLiteral(0, line));
    setRight(idxDecl, intTypeNode);
    setTypeAST(idxDecl, TYPE_INT64);
    addChild(outer, idxDecl);

    /* while (__aether_slice_idx_N < (HI - LO)) { ... } */
    Token *condIdxTok = newToken(TOKEN_IDENTIFIER, idxName, line, 0);
    AST *condIdx = newASTNode(AST_VARIABLE, condIdxTok);
    setTypeAST(condIdx, TYPE_INT64);
    Token *ltTok = newToken(TOKEN_LESS, "<", line, 0);
    AST *cond = newASTNode(AST_BINARY_OP, ltTok);
    setLeft(cond, condIdx);
    setRight(cond, copyAST(widthExpr));
    setTypeAST(cond, TYPE_BOOLEAN);

    /* __aether_slice_N[__aether_slice_idx_N] = base[LO + __aether_slice_idx_N]; */
    Token *sliceRefTok2 = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
    AST *sliceRef2 = newASTNode(AST_VARIABLE, sliceRefTok2);
    setTypeAST(sliceRef2, sliceVtype);
    Token *lhsIdxTok = newToken(TOKEN_IDENTIFIER, idxName, line, 0);
    AST *lhsIdx = newASTNode(AST_VARIABLE, lhsIdxTok);
    setTypeAST(lhsIdx, TYPE_INT64);
    AST *lhsAccess = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(lhsAccess, sliceRef2);
    addChild(lhsAccess, lhsIdx);
    setTypeAST(lhsAccess, TYPE_UNKNOWN);

    Token *srcIdxBaseTok = newToken(TOKEN_IDENTIFIER, idxName, line, 0);
    AST *srcIdxBase = newASTNode(AST_VARIABLE, srcIdxBaseTok);
    setTypeAST(srcIdxBase, TYPE_INT64);
    Token *srcPlusTok = newToken(TOKEN_PLUS, "+", line, 0);
    AST *srcIdxExpr = newASTNode(AST_BINARY_OP, srcPlusTok);
    setLeft(srcIdxExpr, copyAST(lo));
    setRight(srcIdxExpr, srcIdxBase);
    setTypeAST(srcIdxExpr, TYPE_INTEGER);
    AST *rhsAccess = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(rhsAccess, copyAST(base));
    addChild(rhsAccess, srcIdxExpr);
    setTypeAST(rhsAccess, TYPE_UNKNOWN);

    Token *copyAssignTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *copyAssign = newASTNode(AST_ASSIGN, copyAssignTok);
    setLeft(copyAssign, lhsAccess);
    setRight(copyAssign, rhsAccess);
    setTypeAST(copyAssign, sliceVtype);
    AST *copyStmt = newASTNode(AST_EXPR_STMT, copyAssign->token);
    setLeft(copyStmt, copyAssign);

    /* __aether_slice_idx_N = __aether_slice_idx_N + 1; */
    Token *postLhsTok = newToken(TOKEN_IDENTIFIER, idxName, line, 0);
    AST *postLhs = newASTNode(AST_VARIABLE, postLhsTok);
    setTypeAST(postLhs, TYPE_INT64);
    Token *postAddLhsTok = newToken(TOKEN_IDENTIFIER, idxName, line, 0);
    AST *postAddLhs = newASTNode(AST_VARIABLE, postAddLhsTok);
    setTypeAST(postAddLhs, TYPE_INT64);
    Token *postPlusTok = newToken(TOKEN_PLUS, "+", line, 0);
    AST *postAddExpr = newASTNode(AST_BINARY_OP, postPlusTok);
    setLeft(postAddExpr, postAddLhs);
    setRight(postAddExpr, buildIntLiteral(1, line));
    setTypeAST(postAddExpr, TYPE_INT64);
    Token *postAssignTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *postAssign = newASTNode(AST_ASSIGN, postAssignTok);
    setLeft(postAssign, postLhs);
    setRight(postAssign, postAddExpr);
    setTypeAST(postAssign, TYPE_INT64);
    AST *postStmt = newASTNode(AST_EXPR_STMT, postAssign->token);
    setLeft(postStmt, postAssign);

    AST *whileBody = newASTNode(AST_COMPOUND, NULL);
    addChild(whileBody, copyStmt);
    addChild(whileBody, postStmt);
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);
    addChild(outer, whileNode);

    /* `base`, `lo`, `hi` are fully consumed now (copied where reused above,
     * originals no longer needed). */
    freeAST(base);
    freeAST(lo);
    freeAST(hi);

    pushPendingObjLit(p, outer);

    /* Reference to the temp slice for the caller to keep chaining postfix
     * operations on, exactly like the bare-object-literal case. */
    Token *refTok = newToken(TOKEN_IDENTIFIER, tempName, line, 0);
    AST *ref = newASTNode(AST_VARIABLE, refTok);
    setTypeAST(ref, sliceVtype);
    return ref;
}

/* Build a fresh reference to local `name` (a distinct AST_VARIABLE instance --
 * AST nodes form a tree, so every use site needs its own node). */
AST *buildVarRef(const char *name, VarType vt, int line) {
    Token *tok = newToken(TOKEN_IDENTIFIER, name, line, 0);
    AST *var = newASTNode(AST_VARIABLE, tok);
    setTypeAST(var, vt);
    return var;
}

/* Build `length(<name>)` as an AST_PROCEDURE_CALL (INTEGER), by variable name
 * rather than by AST (see buildVarRef -- avoids constructing then discarding
 * an extra node just to hand it to buildLengthCall). */
static AST *buildLengthCallByName(const char *name, VarType vt, int line) {
    Token *lenTok = newToken(TOKEN_IDENTIFIER, "length", line, 0);
    AST *call = newASTNode(AST_PROCEDURE_CALL, lenTok);
    addChild(call, buildVarRef(name, vt, line));
    setTypeAST(call, TYPE_INTEGER);
    return call;
}

/* Build `int <name> = low; while (<name> < high) { body; <name> = <name> + 1; }`,
 * the same lowering parseLoopRange emits for the surface `loop NAME in LOW..HIGH
 * { BODY }`, but from already-built AST nodes (`low`/`high`/`body`) rather than
 * parsed tokens -- used to build a synthetic copy loop for array concatenation.
 * Ownership of low/high/body transfers in. Returns a plain AST_COMPOUND (no
 * i_val splice marker), usable directly as one statement, exactly like
 * parseLoopRange's return value. */
static AST *buildIndexLoop(const char *name, AST *low, AST *high, AST *body, int line) {
    AST *initVar = buildVarRef(name, TYPE_INT64, line);
    Token *intTypeTok = newToken(TOKEN_IDENTIFIER, "int", line, 0);
    AST *intTypeNode = newASTNode(AST_TYPE_IDENTIFIER, intTypeTok);
    setTypeAST(intTypeNode, TYPE_INT64);
    AST *initDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(initDecl, initVar);
    setLeft(initDecl, low);
    setRight(initDecl, intTypeNode);
    setTypeAST(initDecl, TYPE_INT64);

    Token *ltTok = newToken(TOKEN_LESS, "<", line, 0);
    AST *cond = newASTNode(AST_BINARY_OP, ltTok);
    setLeft(cond, buildVarRef(name, TYPE_UNKNOWN, line));
    setRight(cond, high);
    setTypeAST(cond, TYPE_BOOLEAN);

    Token *plusTok = newToken(TOKEN_PLUS, "+", line, 0);
    AST *addExpr = newASTNode(AST_BINARY_OP, plusTok);
    setLeft(addExpr, buildVarRef(name, TYPE_UNKNOWN, line));
    setRight(addExpr, buildIntLiteral(1, line));
    setTypeAST(addExpr, promoteIntegralBinaryType(TYPE_UNKNOWN, TYPE_INT64));
    Token *assignTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *postAssign = newASTNode(AST_ASSIGN, assignTok);
    setLeft(postAssign, buildVarRef(name, TYPE_UNKNOWN, line));
    setRight(postAssign, addExpr);
    setTypeAST(postAssign, TYPE_UNKNOWN);
    AST *postStmt = newASTNode(AST_EXPR_STMT, postAssign->token);
    setLeft(postStmt, postAssign);

    AST *whileBody = newASTNode(AST_COMPOUND, NULL);
    addChild(whileBody, body);
    addChild(whileBody, postStmt);
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    addChild(outer, initDecl);
    addChild(outer, whileNode);
    return outer;
}

/* Expand `other` (an array-*valued expression*, not a literal -- the shape
 * buildArrayAppend handles) into a real concatenation onto `target`, appending
 * the resulting statements as children of `dest` in order -- genuine array
 * concatenation, e.g. `ys = ys + two;` / `let zs: T[] = ys + two;`. Previously
 * this fell through to a raw VM `ARRAY + ARRAY` binary op with no defined
 * meaning ("Runtime Error: Operands must be numbers for arithmetic operation
 * '+'... Got ARRAY and ARRAY"). Rather than a new VM opcode -- `ArrayObj`
 * ownership (refcounted dynamic arrays vs. deep-copied static arrays, the
 * `dynamic_array_refcount_mutex`) is the highest-risk type family in
 * pscal-core's own in-flight rewrite plan, and no existing runtime helper
 * builds a new array by concatenating two already-built ones -- this lowers
 * entirely to already-hardened primitives, matching the append idiom's own
 * approach:
 *     let __aether_concat_other_<line>: <OtherType> = other;  -- evaluate once
 *     let __aether_concat_base_<line>: Int = length(target);
 *     setlength(target, __aether_concat_base_<line> + length(__aether_concat_other_<line>));
 *     int __aether_concat_i_<line> = 0;
 *     while (__aether_concat_i_<line> < length(__aether_concat_other_<line>)) {
 *         target[__aether_concat_base_<line> + __aether_concat_i_<line>] =
 *             __aether_concat_other_<line>[__aether_concat_i_<line>];
 *         __aether_concat_i_<line> = __aether_concat_i_<line> + 1;
 *     }
 * `other` is hoisted into a temp local first so it's evaluated exactly once
 * even if it's a call or other expression with side effects. `otherTypeName`
 * (e.g. "Int[]", ownership transferred in) is `other`'s inferred Aether type
 * name -- inference can't run on a synthesized AST node, only on real parsed
 * source, so the caller must supply it (from inferLetTypeName on the original
 * parsed `other`). `target` is only read (copied); `dest` must already exist
 * (an AST_COMPOUND the caller splices into its own result) and the four
 * statements are appended to it in order. Returns false (and sets
 * `p->hadError`, freeing `other`) only on a malloc-only type-node build
 * failure; the caller must then discard whatever it was assembling. `other`
 * must already be detached from its original parent. */
static bool buildArrayConcatSteps(AetherParser *p, AST *dest, AST *target, AST *other,
                                  char *otherTypeName, int line) {
    VarType otherVt = TYPE_UNKNOWN;
    AST *otherTypeNode = buildTypeNodeFromName(otherTypeName, strlen(otherTypeName), line, &otherVt);
    free(otherTypeName);
    if (!otherTypeNode) {
        p->hadError = true;
        freeAST(other);
        return false;
    }

    /* Temp names carry a monotonic serial as well as the line. The line alone
     * was unique while only ONE concat could be lowered per statement, but a
     * chain puts several on the same line -- `mk() + mk() + mk()` emitted two
     * `__aether_concat_other_<line>` declarations and failed with "duplicate
     * variable ... in this scope". The line is kept in the name because it is
     * what makes these readable in a disassembly or a scope dump. */
    static int concatSerial = 0;
    int serial = ++concatSerial;
    char otherName[64], baseName[64], idxName[64];
    snprintf(otherName, sizeof(otherName), "__aether_concat_other_%d_%d", line, serial);
    snprintf(baseName, sizeof(baseName), "__aether_concat_base_%d_%d", line, serial);
    snprintf(idxName, sizeof(idxName), "__aether_concat_i_%d_%d", line, serial);

    /* let __aether_concat_other_<line>: <OtherType> = other; */
    AST *otherDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(otherDecl, buildVarRef(otherName, otherVt, line));
    setLeft(otherDecl, other);
    setRight(otherDecl, otherTypeNode);
    setTypeAST(otherDecl, otherVt);

    /* let __aether_concat_base_<line>: Int = length(target); */
    Token *intTypeTok = newToken(TOKEN_IDENTIFIER, "int", line, 0);
    AST *intTypeNode = newASTNode(AST_TYPE_IDENTIFIER, intTypeTok);
    setTypeAST(intTypeNode, TYPE_INT64);
    AST *baseDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(baseDecl, buildVarRef(baseName, TYPE_INT64, line));
    setLeft(baseDecl, buildLengthCall(target, line));
    setRight(baseDecl, intTypeNode);
    setTypeAST(baseDecl, TYPE_INT64);

    /* setlength(target, base + length(other)); */
    Token *slTok = newToken(TOKEN_IDENTIFIER, "setlength", line, 0);
    AST *setlen = newASTNode(AST_PROCEDURE_CALL, slTok);
    addChild(setlen, copyAST(target));
    Token *sumTok = newToken(TOKEN_PLUS, "+", line, 0);
    AST *sumExpr = newASTNode(AST_BINARY_OP, sumTok);
    setLeft(sumExpr, buildVarRef(baseName, TYPE_INT64, line));
    setRight(sumExpr, buildLengthCallByName(otherName, otherVt, line));
    setTypeAST(sumExpr, TYPE_INTEGER);
    addChild(setlen, sumExpr);
    setTypeAST(setlen, TYPE_VOID);
    AST *setlenStmt = newASTNode(AST_EXPR_STMT, setlen->token);
    setLeft(setlenStmt, setlen);

    /* target[base + i] = other[i]; -- the loop body. */
    Token *idxSumTok = newToken(TOKEN_PLUS, "+", line, 0);
    AST *idxSum = newASTNode(AST_BINARY_OP, idxSumTok);
    setLeft(idxSum, buildVarRef(baseName, TYPE_INT64, line));
    setRight(idxSum, buildVarRef(idxName, TYPE_INT64, line));
    setTypeAST(idxSum, TYPE_INTEGER);
    AST *targetIdx = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(targetIdx, copyAST(target));
    addChild(targetIdx, idxSum);
    setTypeAST(targetIdx, TYPE_UNKNOWN);
    AST *otherIdx = newASTNode(AST_ARRAY_ACCESS, NULL);
    setLeft(otherIdx, buildVarRef(otherName, otherVt, line));
    addChild(otherIdx, buildVarRef(idxName, TYPE_INT64, line));
    setTypeAST(otherIdx, TYPE_UNKNOWN);
    Token *copyTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *elemAssign = newASTNode(AST_ASSIGN, copyTok);
    setLeft(elemAssign, targetIdx);
    setRight(elemAssign, otherIdx);
    setTypeAST(elemAssign, TYPE_UNKNOWN);
    AST *elemStmt = newASTNode(AST_EXPR_STMT, elemAssign->token);
    setLeft(elemStmt, elemAssign);
    AST *loopBody = newASTNode(AST_COMPOUND, NULL);
    addChild(loopBody, elemStmt);

    AST *loop = buildIndexLoop(idxName, buildIntLiteral(0, line),
                                buildLengthCallByName(otherName, otherVt, line),
                                loopBody, line);

    addChild(dest, otherDecl);
    addChild(dest, baseDecl);
    addChild(dest, setlenStmt);
    addChild(dest, loop);
    return true;
}

/* Statement-level wrapper for buildArrayConcatSteps: `target = src + other;`,
 * `other` an array-valued expression (not a literal). Splices in the
 * self-reassignment copy (`target = src;`, only when `src != target`, exactly
 * like buildArrayAppend) ahead of the four concatenation steps. Returns an
 * AST_COMPOUND splice (i_val==1) so parseBlock flattens it, or NULL on
 * (malloc-only) failure inside buildArrayConcatSteps. `assign` (and its
 * still-attached `src` at rhs->left) is consumed; `other` must already be
 * detached from `assign`. */
AST *buildArrayConcat(AetherParser *p, AST *assign, AST *target, AST *other,
                      char *otherTypeName, int line, AST *src) {
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

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */

    /* `ys = [0] + ys;` -- the destination appears in the RIGHT operand. The copy
     * below (`ys = [0]`) clobbers it before buildArrayConcatSteps hoists it into
     * its own temp, so the steps then copy in the already-overwritten value:
     * `0 0` instead of `0 1 2`, silently, with no diagnostic. Hoist `other`
     * ahead of the copy so it is captured intact.
     *
     * The mirror shape `xs = xs + [v]` (destination on the LEFT) never had this
     * problem, because there `src` IS `target` and no copy is emitted at all --
     * which is why the documented append idiom always worked and this one did
     * not. */
    if (copyStmt && aetherExprReadsLValue(other, target)) {
        VarType preVt = TYPE_UNKNOWN;
        AST *preTypeNode = buildTypeNodeFromName(otherTypeName, strlen(otherTypeName),
                                                 line, &preVt);
        if (preTypeNode) {
            static int preSerial = 0;
            char preName[64];
            snprintf(preName, sizeof(preName), "__aether_concat_pre_%d_%d", line, ++preSerial);
            AST *preDecl = newASTNode(AST_VAR_DECL, NULL);
            addChild(preDecl, buildVarRef(preName, preVt, line));
            setLeft(preDecl, other);
            setRight(preDecl, preTypeNode);
            setTypeAST(preDecl, preVt);
            addChild(outer, preDecl);
            other = buildVarRef(preName, preVt, line);
        }
        /* If the type node could not be built we fall through unhoisted rather
         * than fail the parse: the pre-existing behavior, not a regression. */
    }

    if (copyStmt) addChild(outer, copyStmt);
    if (!buildArrayConcatSteps(p, outer, target, other, otherTypeName, line)) {
        freeAST(assign);
        freeAST(outer);
        return NULL;
    }

    /* The original assign node's target was copied; `other` was already
     * detached by the caller. Release everything else (target, src, the now-
     * empty `+` shell). */
    freeAST(assign);
    return outer;
}

/* Peel a left-nested chain of array `+` into its operand list.
 *
 * `a + b + c` parses as `((a + b) + c)`, so the chain hangs off the LEFT spine.
 * Every lowering here rewrites `x = <chain>` into "initialize x from the chain's
 * innermost left operand, then apply one setlength+copy step per remaining
 * operand" -- which is correct for any length, because each step appends to `x`
 * in place. Before this, the decl and `ret` paths peeled exactly ONE operand and
 * left whatever remained as the initializer; for a two-term chain that remainder
 * is a plain array and everything worked, but for three or more it was still an
 * array `+`, which reached the VM and died as "Operands must be numbers for
 * arithmetic operation '+' ... Got ARRAY and ARRAY". That made `a + b + c` fail
 * while `a + b` succeeded -- and it bit the textbook shape
 * `quicksort(less) + [pivot] + quicksort(greater)`.
 *
 * On success `*initInOut` is replaced by the innermost left operand (the
 * initializer) and `*outOps` receives `*outCount` operands already in APPLY
 * order. A chain of length 1 (no array `+` at all) yields count 0 and leaves
 * `*initInOut` untouched, so callers can treat "not a chain" uniformly.
 *
 * Ownership: each operand's `items`/`other`/`otherTypeName` are detached from
 * the consumed `+` nodes and owned by the caller, which must either hand them to
 * aetherEmitConcatOperand (which consumes them) or release them with
 * aetherFreeConcatOperands. The emptied `+` shells are freed here. */
/* How many array-`+` operands hang off this expression's left spine, WITHOUT
 * consuming it. aetherCollectConcatChain destroys the `+` shells as it peels,
 * so callers that must fall through to another lowering on a short chain need
 * to know the length before committing. */
int aetherConcatChainLength(AetherParser *p, const AST *expr) {
    int n = 0;
    const AST *cur = expr;
    while (cur && cur->type == AST_BINARY_OP && cur->token &&
           cur->token->type == TOKEN_PLUS && cur->left && cur->right) {
        if (cur->right->type != AST_ARRAY_LITERAL) {
            char *tn = inferLetTypeName((AetherParser *)p, (AST *)cur->right);
            bool isArr = tn && aetherTypeNameIsArray(tn);
            free(tn);
            if (!isArr) break;
        }
        n++;
        cur = cur->left;
    }
    return n;
}

bool aetherCollectConcatChain(AetherParser *p, AST **initInOut,
                              AetherConcatOperand **outOps, int *outCount) {
    *outOps = NULL;
    *outCount = 0;
    AST *init = initInOut ? *initInOut : NULL;

    AetherConcatOperand *rev = NULL; /* collected right-to-left */
    int count = 0, cap = 0;

    while (init && init->type == AST_BINARY_OP && init->token &&
           init->token->type == TOKEN_PLUS && init->left && init->right) {
        bool isAppend = (init->right->type == AST_ARRAY_LITERAL);
        char *otherTypeName = NULL;
        if (!isAppend) {
            /* Only an array-*valued* right operand is a concat; this is what
             * keeps ordinary Text/Int/Real `+` out of the rewrite entirely. */
            otherTypeName = inferLetTypeName(p, init->right);
            if (!otherTypeName || !aetherTypeNameIsArray(otherTypeName)) {
                free(otherTypeName);
                break;
            }
        }
        if (count == cap) {
            int ncap = cap ? cap * 2 : 4;
            AetherConcatOperand *grown =
                (AetherConcatOperand *)realloc(rev, sizeof(AetherConcatOperand) * (size_t)ncap);
            if (!grown) { free(otherTypeName); aetherFreeConcatOperands(rev, count, true); return false; }
            rev = grown;
            cap = ncap;
        }

        AetherConcatOperand *op = &rev[count++];
        memset(op, 0, sizeof(*op));
        op->line = init->token->line;
        if (isAppend) {
            op->itemCount = init->right->child_count;
            if (op->itemCount > 0) {
                op->items = (AST **)malloc(sizeof(AST *) * (size_t)op->itemCount);
                if (!op->items) { aetherFreeConcatOperands(rev, count, true); return false; }
                for (int i = 0; i < op->itemCount; i++) {
                    op->items[i] = init->right->children[i];
                    init->right->children[i] = NULL;
                    if (op->items[i]) op->items[i]->parent = NULL;
                }
                init->right->child_count = 0;
            }
        } else {
            op->other = init->right;
            op->otherTypeName = otherTypeName;
            init->right = NULL;
            if (op->other) op->other->parent = NULL;
        }

        AST *left = init->left;
        init->left = NULL;
        freeAST(init); /* the emptied `+` shell (and, for appends, the literal) */
        init = left;
        if (init) init->parent = NULL;
    }

    if (count > 0) {
        /* Reverse into apply order: peeled c,b -> apply b,c. */
        for (int i = 0, j = count - 1; i < j; i++, j--) {
            AetherConcatOperand tmp = rev[i];
            rev[i] = rev[j];
            rev[j] = tmp;
        }
        *initInOut = init;
    }
    *outOps = rev;
    *outCount = count;
    return true;
}

void aetherFreeConcatOperands(AetherConcatOperand *ops, int count, bool freeOwned) {
    if (!ops) return;
    if (freeOwned) {
        for (int i = 0; i < count; i++) {
            for (int k = 0; k < ops[i].itemCount; k++) freeAST(ops[i].items[k]);
            free(ops[i].items);
            freeAST(ops[i].other);
            free(ops[i].otherTypeName);
        }
    }
    free(ops);
}

/* Emit one operand's statements into `dest`, appending to `var`. Consumes the
 * operand's owned nodes either way. */
bool aetherEmitConcatOperand(AetherParser *p, AST *dest, AST *var,
                             AetherConcatOperand *op) {
    if (op->other) {
        bool ok = buildArrayConcatSteps(p, dest, var, op->other, op->otherTypeName, op->line);
        op->other = NULL;          /* consumed by buildArrayConcatSteps */
        op->otherTypeName = NULL;  /* likewise (it takes ownership) */
        return ok;
    }
    for (int i = 0; i < op->itemCount; i++) {
        AST *setlenStmt = NULL, *idxAssign = NULL;
        buildArrayAppendSteps(var, op->items[i], op->line, &setlenStmt, &idxAssign);
        addChild(dest, setlenStmt);
        addChild(dest, idxAssign);
        op->items[i] = NULL; /* consumed */
    }
    free(op->items);
    op->items = NULL;
    op->itemCount = 0;
    return true;
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
AST *parseLetTupleDestructure(AetherParser *p, int kwLine) {
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
        /* FromName, not buildTypeNode: itemTypes carry the `[]` suffix
         * convention (an `(Int, Int[])` signature stores "Int[]"), and
         * buildTypeNode would lookupType("Int[]") and fail. */
        AST *typeNode = buildTypeNodeFromName(sig->itemTypes[i], strlen(sig->itemTypes[i]), kwLine, &vt);

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
 * name (a naming convention kept from the retired rewriter). */
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

/* `ret src + other;` / `ret src + [items...];` -> bind the concatenation to a
 * temp, then return the temp:
 *
 *     let __aether_ret_concat_<line>: T[] = src;
 *     <setlength + indexed-copy steps for `other`/`items`>
 *     return __aether_ret_concat_<line>;
 *
 * Array `+` is not a VM operation: the front end lowers it into setlength plus
 * an indexed element-copy, and those are *statements*, so they need a statement
 * position to be spliced into. A `let` declaration provides one (see the
 * hasArrayConcatInit / hasArrayAppendInit branches in parseLet); a bare `ret`
 * is an expression position, so before this the raw `+` survived to the VM and
 * died as "Operands must be numbers for arithmetic operation '+' ... Got ARRAY
 * and ARRAY". Introducing the temp turns `ret` back into a statement position,
 * which is exactly the `let c = a + b; ret c;` workaround, done by the parser.
 *
 * `value` (the parsed `+` expression) is consumed. Returns an AST_COMPOUND
 * splice (i_val==1) so parseBlock flattens it, or NULL on failure (having set
 * p->hadError). Callers detect the shape with returnArrayConcatNeedsLowering().
 *
 * `stageResultName` selects the tail: NULL emits the `return <temp>;` above,
 * while a name (`"result"`) emits `<name> = <temp>;` instead and leaves the
 * compound open for the caller to append a @post guard and its own return --
 * that path stages `result` itself, so it needs the concat lowered *before* the
 * guard runs rather than a finished return. */
static AST *buildReturnArrayConcat(AetherParser *p, AST *value, int line,
                                   const char *stageResultName) {
    int concatLine = value->token->line;

    /* Peel the whole chain, not just one operand: `ret a + b + c;` is as valid
     * a shape as `ret a + b;`, and `quicksort(less) + [pivot] + quicksort(gt)`
     * is the textbook one. `base` becomes the temp's initializer and each
     * remaining operand appends to the temp in place. Previously this peeled a
     * single operand and left the rest as the initializer, so a three-term
     * chain declared the temp from a raw array `+` that reached the VM as
     * "Operands must be numbers ... Got ARRAY and ARRAY". */
    AST *base = value;
    AetherConcatOperand *ops = NULL;
    int opCount = 0;
    if (!aetherCollectConcatChain(p, &base, &ops, &opCount) || opCount == 0) {
        aetherFreeConcatOperands(ops, opCount, true);
        p->hadError = true;
        freeAST(base);
        return NULL;
    }

    /* The temp's declared type. Prefer the base's own type; fall back to the
     * first array-valued operand, since an append operand is a bare literal
     * that infers no element type of its own. */
    char *tmpTypeName = inferLetTypeName(p, base);
    if (!tmpTypeName || !aetherTypeNameIsArray(tmpTypeName)) {
        free(tmpTypeName);
        tmpTypeName = NULL;
        for (int i = 0; i < opCount && !tmpTypeName; i++) {
            if (ops[i].otherTypeName) tmpTypeName = strdup(ops[i].otherTypeName);
        }
    }
    if (!tmpTypeName) {
        /* Wording deliberately starts with "cannot infer the type of" so the
         * detail-based fallback in aetherInferDiagnosticCode tags it TYPE-001
         * rather than leaving it uncoded. */
        reportAetherAstError(aetherSemanticGetSourcePath(), concatLine, "return",
                "cannot infer the type of this array concatenation.",
                "bind it first: `let c: T[] = a + b; ret c;`.");
        p->hadError = true;
        aetherFreeConcatOperands(ops, opCount, true);
        freeAST(base);
        return NULL;
    }

    VarType tmpVt = TYPE_UNKNOWN;
    AST *tmpTypeNode = buildTypeNodeFromName(tmpTypeName, strlen(tmpTypeName),
                                             concatLine, &tmpVt);
    if (!tmpTypeNode) {
        p->hadError = true;
        free(tmpTypeName);
        aetherFreeConcatOperands(ops, opCount, true);
        freeAST(base);
        return NULL;
    }

    char tmpName[64];
    snprintf(tmpName, sizeof(tmpName), "__aether_ret_concat_%d", concatLine);
    bindingTableSet(p->bindings, tmpName, tmpTypeName);
    free(tmpTypeName);

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */

    /* let __aether_ret_concat_<line>: T[] = <chain base>; */
    AST *tmpVar = buildVarRef(tmpName, tmpVt, concatLine);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, tmpVar);
    setLeft(decl, base);
    setRight(decl, tmpTypeNode);
    setTypeAST(decl, tmpVt);
    addChild(outer, decl);

    /* Any step past the first setlength un-aliases the temp from its source for
     * free; a chain of nothing but empty literals (`ret src + [];`) emits none,
     * so it still needs the explicit un-alias. */
    bool emitsAnyStep = false;
    for (int i = 0; i < opCount; i++) {
        if (ops[i].other || ops[i].itemCount > 0) { emitsAnyStep = true; break; }
    }
    if (!emitsAnyStep && aetherArrayInitMayAlias(base)) {
        addChild(outer, buildArrayUnaliasStmt(tmpVar, concatLine));
    }

    /* `tmpVar` is only read (copied) by the step builders, exactly as in the
     * parseLet branch; ownership of each operand's nodes transfers in. */
    for (int i = 0; i < opCount; i++) {
        if (!aetherEmitConcatOperand(p, outer, tmpVar, &ops[i])) {
            aetherFreeConcatOperands(ops, opCount, true);
            freeAST(outer);
            return NULL;
        }
    }
    aetherFreeConcatOperands(ops, opCount, false);

    if (stageResultName) {
        /* result = __aether_ret_concat_<line>;  (caller appends guard + return) */
        addChild(outer, buildSimpleAssign(stageResultName,
                                          buildVarRef(tmpName, tmpVt, concatLine),
                                          line));
    } else {
        /* return __aether_ret_concat_<line>; */
        Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
        AST *ret = newASTNode(AST_RETURN, retTok);
        setLeft(ret, buildVarRef(tmpName, tmpVt, concatLine));
        setTypeAST(ret, tmpVt);
        addChild(outer, ret);
    }
    return outer;
}

/* Does `value` need the buildReturnArrayConcat lowering -- i.e. is it an array
 * `+` in return position? Mirrors the two known-good detection conditions in
 * parseLet: an array *literal* right operand is the append shape (unconditional
 * -- a literal can only be appended to an array), any other right operand is
 * the concat shape and must infer to a manifestly array ("[]"-suffixed) type so
 * ordinary Text/Int/Real `+` (string concat, arithmetic) is left untouched. */
static bool returnArrayConcatNeedsLowering(AetherParser *p, const AST *value) {
    if (!value || value->type != AST_BINARY_OP || !value->token ||
        value->token->type != TOKEN_PLUS || !value->right || !value->left) {
        return false;
    }
    if (value->right->type == AST_ARRAY_LITERAL) return true;
    char *otherTypeName = inferLetTypeName(p, value->right);
    bool isArray = otherTypeName && aetherTypeNameIsArray(otherTypeName);
    free(otherTypeName);
    return isArray;
}

/* ret [expr] ;  ->  AST_RETURN (mirrors rea parseReturn).
 *
 * Three contract/tuple-aware shapes (MILESTONE 3), matching translate.c:
 *   - tuple-return fn: `ret (a,b);`  -> per-slot writes + [post] + `return;`
 *   - @post on a value fn: `ret e;`  -> `result = e; <post guard>; return result;`
 *   - otherwise: a plain AST_RETURN. */
AST *parseRet(AetherParser *p) {
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
        /* `ret x + ;` used to return nil from a `-> Int` function, exit 0. */
        if (!value) aetherReportMissingExpr(p, "after 'ret'");
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

    /* `ret src + other;` -- array concat/append in return position needs a
     * statement position to splice its lowering into (see
     * buildReturnArrayConcat). Under a @post guard the same lowering runs, but
     * staged into `result` ahead of the guard, in the branch just below. */
    if (value && !p->currentPostExpr && returnArrayConcatNeedsLowering(p, value)) {
        return buildReturnArrayConcat(p, value, line, NULL);
    }

    /* @post on a value-returning function: stage `result`, check, then return it,
     * exactly as the rewriter's translateReturnWithPost. */
    if (p->currentPostExpr && value) {
        /* Captured up front: the concat lowering below consumes (frees) `value`,
         * and the `return result;` tail still needs the returned type. */
        VarType resultVt = value->var_type;
        AST *outer;
        if (returnArrayConcatNeedsLowering(p, value)) {
            /* `@post ... ret a + b;` -- lower the concat into a temp first, then
             * stage that into `result`, so the guard sees the finished array
             * rather than a raw `+` the VM has no operator for. */
            outer = buildReturnArrayConcat(p, value, line, "result");
            if (!outer) return NULL;
        } else {
            outer = newASTNode(AST_COMPOUND, NULL);
            outer->i_val = 1; /* splice into the surrounding block */
            addChild(outer, buildSimpleAssign("result", value, line));
        }
        AST *guard = buildContractGuard(p, p->currentPostExpr, "post",
                                        p->currentFunctionName, line);
        if (!guard) { freeAST(outer); return NULL; }
        addChild(outer, guard);
        Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
        AST *ret = newASTNode(AST_RETURN, retTok);
        Token *resTok = newToken(TOKEN_IDENTIFIER, "result", line, 0);
        AST *resVar = newASTNode(AST_VARIABLE, resTok);
        setTypeAST(resVar, resultVt);
        setLeft(ret, resVar);
        setTypeAST(ret, resultVt);
        addChild(outer, ret);
        return outer;
    }

    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *node = newASTNode(AST_RETURN, retTok);
    setLeft(node, value);
    setTypeAST(node, value ? value->var_type : TYPE_VOID);
    return node;
}

/* Inject the loop post-step before every `continue` in a range-loop body.
 * `loop i in a..b` lowers to a while whose post-increment is the last body
 * statement, so a bare `continue` would jump straight to the condition, skip the
 * increment, and spin forever. Rewrite each `continue` to `{ post; continue; }`.
 *
 * Stop at nested loop and routine nodes: a `continue` inside them belongs to
 * that inner loop, not to this one. This is exact, not heuristic -- an inner
 * range/foreach loop has already been lowered to AST_WHILE (with its own
 * post-step spliced in) by the time the outer body is rewritten, so descending
 * into it would re-wrap the inner `continue` and advance every enclosing
 * counter too (`loop i in 0..3 { loop j in 0..3 { if j == 1 { continue; } ... } }`
 * used to bump `i` on every inner `continue`). Descend through everything else,
 * including if/match/case, which do not own `continue`. */
static AST *aetherRewriteContinueWithPost(AST *node, AST *postStmt) {
    if (!node) return NULL;
    switch (node->type) {
        case AST_WHILE:
        case AST_REPEAT:
        case AST_FOR_TO:
        case AST_FOR_DOWNTO:
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            return node;
        default:
            break;
    }
    if (node->type == AST_CONTINUE) {
        AST *comp = newASTNode(AST_COMPOUND, NULL);
        addChild(comp, copyAST(postStmt));
        addChild(comp, newASTNode(AST_CONTINUE, NULL));
        return comp;
    }
    /* Reattach parent links on every splice: when a CONTINUE child is replaced
     * by the new COMPOUND above, plain slot assignment would leave the
     * compound's parent NULL, orphaning the copied post-statement from the
     * scope chain -- rea semantic's upward walk then can't see the loop
     * variable's decl and reports a bogus SCOPE-001 whenever the same name is
     * bound anywhere else in the program. */
    node->left  = aetherRewriteContinueWithPost(node->left, postStmt);
    if (node->left) node->left->parent = node;
    node->right = aetherRewriteContinueWithPost(node->right, postStmt);
    if (node->right) node->right->parent = node;
    node->extra = aetherRewriteContinueWithPost(node->extra, postStmt);
    if (node->extra) node->extra->parent = node;
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) {
            node->children[i] = aetherRewriteContinueWithPost(node->children[i], postStmt);
            if (node->children[i]) node->children[i]->parent = node;
        }
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
 * We reproduce that exact structure so output matches rea's own for loop.
 *
 * The range operator is now a real AE_TOKEN_DOTDOT token (the aetherAdvance()
 * tokenizer reconstructs it despite the shared Rea lexer folding the dots), so
 * both bounds are parsed straight from the token stream as expressions -- no
 * raw-source-span workaround. Handles numeric and identifier bounds uniformly. */
/* Compile-time integer literal test: `3`, or `-3` as unary minus over a
 * literal. Used to pick the `step` comparison direction statically. */
static bool aetherConstIntValue(const AST *node, long *out) {
    if (!node) return false;
    if (node->type == AST_NUMBER && node->token && node->token->value &&
        !isRealType(node->var_type)) {
        char *end = NULL;
        long v = strtol(node->token->value, &end, 0);
        if (end && end != node->token->value && *end == '\0') { *out = v; return true; }
        return false;
    }
    if (node->type == AST_UNARY_OP && node->token && node->token->type == TOKEN_MINUS && node->left) {
        long inner;
        if (aetherConstIntValue(node->left, &inner)) { *out = -inner; return true; }
    }
    return false;
}

static AST *buildBinOp(TokenType tt, const char *lex, AST *l, AST *r, VarType vt, int line) {
    Token *tok = newToken(tt, lex, line, 0);
    AST *bin = newASTNode(AST_BINARY_OP, tok);
    setLeft(bin, l);
    setRight(bin, r);
    setTypeAST(bin, vt);
    return bin;
}

/* `int NAME = init;` -- the loop counter shape rea parseFor emits. */
static AST *buildIntCounterDecl(const char *name, AST *init, int line) {
    AST *initVar = buildVarRef(name, TYPE_INT64, line);
    Token *intTypeTok = newToken(TOKEN_IDENTIFIER, "int", line, 0);
    AST *intTypeNode = newASTNode(AST_TYPE_IDENTIFIER, intTypeTok);
    setTypeAST(intTypeNode, TYPE_INT64);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, initVar);
    setLeft(decl, init);
    setRight(decl, intTypeNode);
    setTypeAST(decl, TYPE_INT64);
    return decl;
}

/* `NAME = NAME + delta;` as an expression statement; `delta` ownership transfers. */
static AST *buildCounterPost(const char *name, AST *delta, int line) {
    AST *addExpr = buildBinOp(TOKEN_PLUS, "+", buildVarRef(name, TYPE_UNKNOWN, line), delta,
                              promoteIntegralBinaryType(TYPE_UNKNOWN, TYPE_INT64), line);
    Token *assignTok = newToken(TOKEN_ASSIGN, "=", line, 0);
    AST *postAssign = newASTNode(AST_ASSIGN, assignTok);
    setLeft(postAssign, buildVarRef(name, TYPE_UNKNOWN, line));
    setRight(postAssign, addExpr);
    setTypeAST(postAssign, TYPE_UNKNOWN);
    AST *postStmt = newASTNode(AST_EXPR_STMT, postAssign->token);
    setLeft(postStmt, postAssign);
    return postStmt;
}

/* Typed `let NAME: TypeName = init;` (the explicitly typed binding shape
 * parseLetDeclAfterKeyword emits). `init` ownership transfers. */
static AST *buildTypedLetDecl(const char *name, const char *typeName, AST *init, int line) {
    VarType vt = TYPE_UNKNOWN;
    AST *typeNode = buildTypeNodeFromName(typeName, strlen(typeName), line, &vt);
    if (!typeNode) { if (init) freeAST(init); return NULL; }
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, buildVarRef(name, vt, line));
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vt);
    aetherAstRegisterExplicitTypedDecl(decl);
    return decl;
}

/* `loop NAME in COLLECTION { BODY }` -- iterate an array (`T[]`), a `Text`
 * (one character at a time) or a TOON array node (`ToonNode`). Lowers to the
 * same index-loop shape the range form uses, with the element bound at the
 * top of every iteration:
 *
 *     [let __aether_seq_N: T[] = COLLECTION;]   -- only when COLLECTION is not
 *                                                  a plain variable/field/index
 *     int __aether_i_N = 0;
 *     while (__aether_i_N < length(seq)) {       -- toon_len for a ToonNode
 *         let NAME: T = seq[__aether_i_N];       -- toon_at for a ToonNode
 *         BODY
 *         __aether_i_N = __aether_i_N + 1;
 *     }
 *
 * The element type comes from the collection's inferred Aether type, which is
 * why the collection must be something the parser can type: a typed binding,
 * a field, a call with a declared return type, or a literal. `continue` still
 * advances the index (the post-statement is spliced ahead of it, as in the
 * range loop). A row of a nested array binds through the same un-alias step a
 * `let row: Int[] = table[i];` gets, so writing to the loop variable never
 * writes through to the collection. The variable's type is registered before
 * the body is parsed so `NAME.field` and `let x = NAME;` infer inside it. */
static AST *parseForeach(AetherParser *p, const char *name, AST *coll, int line) {
    char *collType = inferLetTypeName(p, coll);
    char elemBuf[128];
    const char *elemType = NULL;
    bool isToon = false;
    if (collType) {
        size_t n = strlen(collType);
        if (aetherTypeNameIsArray(collType)) {
            if (n > 2 && n - 2 < sizeof(elemBuf)) {
                memcpy(elemBuf, collType, n - 2);
                elemBuf[n - 2] = '\0';
                elemType = elemBuf;
            }
        } else if (strcmp(collType, "Text") == 0 || strcmp(collType, "String") == 0) {
            elemType = "Text";
        } else if (strcmp(collType, "ToonNode") == 0) {
            elemType = "ToonNode";
            isToon = true;
        }
    }
    if (!elemType) {
        char detail[256];
        if (collType) {
            snprintf(detail, sizeof(detail),
                     "cannot iterate over a value of type %s with `loop %s in ...`; the "
                     "collection must be an array (`T[]`), a `Text`, or a `ToonNode` array.",
                     collType, name);
            reportAetherAstError(aetherSemanticGetSourcePath(), line, "parser", detail, NULL);
        } else {
            snprintf(detail, sizeof(detail),
                     "cannot infer the type of the collection in `loop %s in ...`.", name);
            reportAetherAstError(aetherSemanticGetSourcePath(), line, "declaration", detail,
                                 "bind it to a typed `let` first, for example "
                                 "`let items: Int[] = ...;`, then loop over `items`.");
        }
        p->hadError = true;
        free(collType);
        freeAST(coll);
        return NULL;
    }

    int id = p->nextLoopId++;
    char idxName[48], seqName[48];
    snprintf(idxName, sizeof(idxName), "__aether_i_%d", id);
    AST *outer = newASTNode(AST_COMPOUND, NULL);
    AST *seqProto;                 /* the collection reference, copied at each use */
    bool hoisted = !aetherIsLValueChain(coll);
    if (hoisted) {
        snprintf(seqName, sizeof(seqName), "__aether_seq_%d", id);
        AST *seqDecl = buildTypedLetDecl(seqName, collType, coll, line);
        if (!seqDecl) { p->hadError = true; free(collType); freeAST(outer); return NULL; }
        addChild(outer, seqDecl);
        if (p->bindings) bindingTableSet(p->bindings, seqName, collType);
        seqProto = buildVarRef(seqName, TYPE_UNKNOWN, line);
    } else {
        seqProto = coll;
    }
    if (p->bindings) bindingTableSet(p->bindings, name, elemType);

    AST *body = parseBlock(p);
    if (!body) {
        if (!p->hadError) {
            reportAetherAstError(aetherSemanticGetSourcePath(), line, "parser",
                                 "expected '{' to open loop body.", NULL);
        }
        p->hadError = true;
        free(collType);
        freeAST(outer);
        freeAST(seqProto);
        return NULL;
    }

    AST *lenCall;
    AST *elem;
    if (isToon) {
        Token *lenTok = newToken(TOKEN_IDENTIFIER, "YyjsonGetLength", line, 0);
        lenCall = newASTNode(AST_PROCEDURE_CALL, lenTok);
        addChild(lenCall, copyAST(seqProto));
        setTypeAST(lenCall, TYPE_INTEGER);
        aetherAstRegisterCallSurfaceName(lenCall, "toon_len");
        Token *atTok = newToken(TOKEN_IDENTIFIER, "YyjsonGetIndex", line, 0);
        elem = newASTNode(AST_PROCEDURE_CALL, atTok);
        addChild(elem, copyAST(seqProto));
        addChild(elem, buildVarRef(idxName, TYPE_UNKNOWN, line));
        setTypeAST(elem, TYPE_INT64);
        aetherAstRegisterCallSurfaceName(elem, "toon_at");
    } else {
        lenCall = buildLengthCall(seqProto, line);
        elem = newASTNode(AST_ARRAY_ACCESS, NULL);
        setLeft(elem, copyAST(seqProto));
        addChild(elem, buildVarRef(idxName, TYPE_UNKNOWN, line));
        setTypeAST(elem, TYPE_UNKNOWN);
    }
    AST *elemDecl = buildTypedLetDecl(name, elemType, elem, line);
    if (!elemDecl) {
        p->hadError = true;
        free(collType);
        freeAST(outer);
        freeAST(seqProto);
        freeAST(lenCall);
        freeAST(body);
        return NULL;
    }

    AST *innerBody = newASTNode(AST_COMPOUND, NULL);
    addChild(innerBody, elemDecl);
    if (aetherTypeNameIsArray(elemType)) {
        AST *elemRef = buildVarRef(name, TYPE_UNKNOWN, line);
        addChild(innerBody, buildArrayUnaliasStmt(elemRef, line));
        freeAST(elemRef);
    }
    addChild(innerBody, body);

    AST *postStmt = buildCounterPost(idxName, buildIntLiteral(1, line), line);
    innerBody = aetherRewriteContinueWithPost(innerBody, postStmt);
    AST *whileBody = newASTNode(AST_COMPOUND, NULL);
    addChild(whileBody, innerBody);
    addChild(whileBody, postStmt);
    AST *cond = buildBinOp(TOKEN_LESS, "<", buildVarRef(idxName, TYPE_UNKNOWN, line), lenCall,
                           TYPE_BOOLEAN, line);
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);
    addChild(outer, buildIntCounterDecl(idxName, buildIntLiteral(0, line), line));
    addChild(outer, whileNode);

    freeAST(seqProto); /* every use above was a copy */
    free(collType);
    return outer;
}

/* `loop NAME in LOW..HIGH [step DELTA] { BODY }` (also spelled `for`), or the
 * foreach form `loop NAME in COLLECTION { BODY }` (parseForeach). The range
 * lowers to the C-for shape rea parseFor emits:
 *
 *     int NAME = LOW;
 *     [int __aether_step_N = DELTA;]   -- a non-literal step is evaluated once
 *     while (NAME < HIGH) { BODY; NAME = NAME + DELTA; }
 *
 * A negative literal step flips the comparison to `NAME > HIGH`; a step whose
 * sign is only known at run time tests both directions
 * (`(s > 0 && NAME < HIGH) || (s < 0 && NAME > HIGH)`), so the loop never
 * runs away in the wrong direction. A literal `step 0` is rejected. */
AST *parseLoopRange(AetherParser *p) {
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
    /* No bound at all (`loop i in ..5`, `loop i in {`): the loop-header message
     * says more than parsePrimary's generic missing-expression one. */
    AST *low = (p->current.type == AE_TOKEN_DOTDOT || p->current.type == REA_TOKEN_LEFT_BRACE)
                   ? NULL : parseAdd(p);
    if (!low) {
        if (!p->hadError) {
            reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                    "expected '<low>..<high>' or a collection after 'in' in the loop header.", NULL);
        }
        p->hadError = true;
        free(nameBuf);
        return NULL;
    }
    /* `loop NAME in COLLECTION {` -- no range operator, straight into the body. */
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        AST *loop = parseForeach(p, nameBuf, low, idLine);
        free(nameBuf);
        return loop;
    }
    if (p->current.type != AE_TOKEN_DOTDOT) {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "expected '<low>..<high>' in loop range.", NULL);
        p->hadError = true;
        freeAST(low);
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
    ReaToken dots = p->current;
    aetherAdvance(p); /* consume '..' */
    aetherNoteOperator(p, &dots);
    AST *high = parseAdd(p);
    if (!high) {
        if (!p->hadError) {
            reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                    "could not parse loop range bounds.", NULL);
        }
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

    /* Optional `step DELTA`. */
    AST *step = NULL;
    long stepLit = 1;
    int stepSign = 1;   /* +1 / -1 for a literal step, 0 when only known at run time */
    if (isAetherKeyword(&p->current, "step")) {
        int stepLine = p->current.line;
        aetherAdvance(p); /* consume 'step' */
        step = (p->current.type == REA_TOKEN_LEFT_BRACE) ? NULL : parseAdd(p);
        if (!step || step->var_type == TYPE_BOOLEAN) {
            if (step || !p->hadError) {
                reportAetherAstError(aetherSemanticGetSourcePath(), stepLine, "parser",
                        "expected an Int step after 'step' (for example `step 2` or `step -1`).", NULL);
            }
            p->hadError = true;
            if (step) freeAST(step);
            freeAST(low);
            freeAST(high);
            free(nameBuf);
            return NULL;
        }
        if (aetherConstIntValue(step, &stepLit)) {
            if (stepLit == 0) {
                reportAetherAstError(aetherSemanticGetSourcePath(), stepLine, "parser",
                        "loop step must not be zero.", NULL);
                p->hadError = true;
                freeAST(step);
                freeAST(low);
                freeAST(high);
                free(nameBuf);
                return NULL;
            }
            stepSign = stepLit > 0 ? 1 : -1;
        } else {
            stepSign = 0;
        }
    }

    /* The loop variable is an Int for the rest of the function (the inference
     * table is flat per function, like `let`), so `let s = i;` infers. */
    if (p->bindings) bindingTableSet(p->bindings, nameBuf, "Int");

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        reportAetherAstError(aetherSemanticGetSourcePath(), idLine, "parser",
                "expected '{' to open loop body.", NULL);
        p->hadError = true;
        if (step) freeAST(step);
        freeAST(low);
        freeAST(high);
        free(nameBuf);
        return NULL;
    }

    AST *outer = newASTNode(AST_COMPOUND, NULL);
    addChild(outer, buildIntCounterDecl(nameBuf, low, idLine));

    /* A literal step is inlined; any other step expression is evaluated once,
     * before the loop, into a hidden counter. */
    char stepName[48];
    bool stepHoisted = false;
    if (step && stepSign == 0) {
        snprintf(stepName, sizeof(stepName), "__aether_step_%d", p->nextLoopId++);
        addChild(outer, buildIntCounterDecl(stepName, step, idLine));
        stepHoisted = true;
    } else if (step) {
        freeAST(step); /* the literal value is carried in stepLit */
    }
    step = NULL;
#define AETHER_STEP_REF() (stepHoisted ? buildVarRef(stepName, TYPE_UNKNOWN, idLine) \
                                       : buildIntLiteral(stepLit, idLine))

    AST *cond;
    if (stepSign > 0) {
        cond = buildBinOp(TOKEN_LESS, "<", buildVarRef(nameBuf, TYPE_UNKNOWN, idLine), high,
                          TYPE_BOOLEAN, idLine);
    } else if (stepSign < 0) {
        cond = buildBinOp(TOKEN_GREATER, ">", buildVarRef(nameBuf, TYPE_UNKNOWN, idLine), high,
                          TYPE_BOOLEAN, idLine);
    } else {
        AST *up = buildBinOp(TOKEN_AND, "&&",
                buildBinOp(TOKEN_GREATER, ">", AETHER_STEP_REF(), buildIntLiteral(0, idLine),
                           TYPE_BOOLEAN, idLine),
                buildBinOp(TOKEN_LESS, "<", buildVarRef(nameBuf, TYPE_UNKNOWN, idLine), high,
                           TYPE_BOOLEAN, idLine),
                TYPE_BOOLEAN, idLine);
        AST *down = buildBinOp(TOKEN_AND, "&&",
                buildBinOp(TOKEN_LESS, "<", AETHER_STEP_REF(), buildIntLiteral(0, idLine),
                           TYPE_BOOLEAN, idLine),
                buildBinOp(TOKEN_GREATER, ">", buildVarRef(nameBuf, TYPE_UNKNOWN, idLine),
                           copyAST(high), TYPE_BOOLEAN, idLine),
                TYPE_BOOLEAN, idLine);
        cond = buildBinOp(TOKEN_OR, "||", up, down, TYPE_BOOLEAN, idLine);
    }

    /* Post: NAME = NAME + DELTA. Rewrite `continue` in the body to run it first
     * (rea parseFor does this via rewriteContinueWithPost) -- otherwise
     * `continue` jumps to the condition, skips the increment, and the loop
     * spins forever. */
    AST *postStmt = buildCounterPost(nameBuf, AETHER_STEP_REF(), idLine);
#undef AETHER_STEP_REF
    body = aetherRewriteContinueWithPost(body, postStmt);

    /* while body = COMPOUND[ body, postStmt ]  (rea parseFor with post). */
    AST *whileBody = newASTNode(AST_COMPOUND, NULL);
    addChild(whileBody, body);
    addChild(whileBody, postStmt);
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);
    addChild(outer, whileNode);

    free(nameBuf);
    return outer;
}

/* Build a while loop from an already-parsed condition + body, mirroring rea
 * parseWhile: AST_WHILE(left=condition, right=body). */
AST *buildWhile(AST *cond, AST *body) {
    AST *node = newASTNode(AST_WHILE, NULL);
    setLeft(node, cond);
    setRight(node, body);
    return node;
}

/* `par { call1(); call2(); ... }` -> the spawn/join block the rewriter emits
 * (translate.c translateParallelCallLine): a nested `{ }` scope holding one
 * `int __aether_par_<N> = spawn callN();` per body call followed by a
 * `join __aether_par_<N>;` for each, in order. The handle counter restarts at 1
 * per par block (clearParBlockState resets nextHandle). Returns an AST_COMPOUND
 * (a block statement). */
AST *parseParBlock(AetherParser *p) {
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
    AetherParSharedRec sharedRecs[64];
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
         * concurrent double-free at runtime (aetherCheckParSharedRecords,
         * ast_checks.c). */
        if (aetherCheckParSharedRecords(p, call, handle, callLine, sharedRecs, &sharedRecCount,
                                        (int)(sizeof(sharedRecs) / sizeof(sharedRecs[0])))) {
            freeAST(call);
            freeAST(block);
            freeAST(joins);
            return NULL;
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
