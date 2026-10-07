/*
 * ast_types.c -- Aether type-name inference and type promotion helpers.
 *
 * inferLetTypeName and inferBuiltinReturnTypeName (the Aether type name of an
 * initializer), the VarType <-> type-name helpers, and the binary/conditional
 * type promotion rules mirrored from rea. Split out of ast_parser.c; see
 * ast_internal.h.
 */

#include "aether/ast_internal.h"

/* ------------------------------------------------------------------ */
/* Type promotion helpers (verbatim from rea parser.c)                 */
/* ------------------------------------------------------------------ */

VarType promoteRealBinaryType(VarType a, VarType b) {
    if (a == TYPE_LONG_DOUBLE || b == TYPE_LONG_DOUBLE) return TYPE_LONG_DOUBLE;
    if (a == TYPE_DOUBLE || b == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (a == TYPE_FLOAT || b == TYPE_FLOAT) return TYPE_FLOAT;
    return TYPE_DOUBLE;
}

VarType promoteIntegralBinaryType(VarType a, VarType b) {
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

VarType inferStringLiteralType(const char *text, size_t len) {
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

VarType resolveConditionalType(AST *thenExpr, AST *elseExpr) {
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

/* Map a VarType back to the Aether builtin type *name*, so an inferred binding
 * can be recorded the way the rewriter records it. Mirrors the inverse of
 * mapAetherType for the scalar builtins; returns NULL for non-builtins. */
const char *aetherTypeNameForVarType(VarType vt) {
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
        case TYPE_FILE:
            return "File";
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
        /* Substring. Also the lowering target of the `s[a..b]` Text-slice form,
         * so an inferred `let sub = s[a..b];` resolves through here. */
        { "copy", "Text" }, { "trim", "Text" },
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
char *inferLetTypeName(AetherParser *p, AST *init) {
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

/* Is `name` (an Aether type name from the binding table, e.g. "Int[]") an
 * array type -- i.e. does it end in the "[]" suffix parseTypeWithArraySuffix /
 * buildTypeNodeFromName use? */
bool aetherTypeNameIsArray(const char *name) {
    if (!name) return false;
    size_t n = strlen(name);
    return n >= 2 && name[n - 2] == '[' && name[n - 1] == ']';
}

/* Trailing `[]` pairs on an Aether type name: "Int" 0, "Int[]" 1, "Int[][]" 2.
 * Used by ARR-002 to tell a genuinely 1-D declaration from a nested one. */
int aetherTypeNameRank(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    int rank = 0;
    while (n >= 2 && name[n - 2] == '[' && name[n - 1] == ']') {
        rank++;
        n -= 2;
    }
    return rank;
}
