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
        { "YyjsonGetString", "Text" }, { "YyjsonGetStringOr", "Text" }, { "ai_chat", "Text" },
        { "openaichatcompletions", "Text" },
        { "aetherbuiltinsjson", "Text" }, { "aetherbuiltininfo", "Text" },
        { "inttostr", "Text" },
        /* Substring. Also the lowering target of the `s[a..b]` Text-slice form,
         * so an inferred `let sub = s[a..b];` resolves through here. */
        { "copy", "Text" }, { "trim", "Text" },
        /* Int-returning. */
        { "length", "Int" }, { "YyjsonGetInt", "Int" }, { "YyjsonGetLength", "Int" },
        { "YyjsonGetIntOr", "Int" },
        /* Real-returning. */
        { "YyjsonGetNumber", "Real" }, { "YyjsonGetNumberOr", "Real" },
        /* Math: pow/power -> Real. (int^int may be exact-Int at runtime, but Real is
         * the safe inferred type; matches pscal-core's builtin-return type list.) */
        { "pow", "Real" }, { "power", "Real" },
        /* The modern standard surface (D56-D61). */
        { "aether_upper", "Text" }, { "aether_lower", "Text" }, { "aether_replace", "Text" },
        { "aether_repeat", "Text" }, { "aether_join", "Text" }, { "aether_read_file", "Text" },
        { "aether_read_line", "Text" }, { "aether_read_all_stdin", "Text" }, { "aether_now_text", "Text" },
        { "aether_contains", "Bool" }, { "aether_starts_with", "Bool" }, { "aether_ends_with", "Bool" },
        { "aether_delete_file", "Bool" }, { "aether_rename_file", "Bool" }, { "fileexists", "Bool" },
        { "aether_index_of", "Int" }, { "aether_count", "Int" }, { "aether_clock_ms", "Int" },
        { "aether_mstream_length", "Int" }, { "aether_round_to", "Real" },
        { "split", "Text[]" }, { "aether_read_lines", "Text[]" }, { "aether_args", "Text[]" },
        { "aether_mstream_load", "MStream" },
        /* Bool-returning. */
        { "hasextbuiltin", "Bool" }, { "YyjsonGetBoolOr", "Bool" },
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
        /* Argument-typed helpers: sorted(xs) is an xs, sum(xs) an element. */
        if (!init->left && init->child_count >= 1 &&
            !aetherAstIsTopLevelUserFunction(init->token->value)) {
            const char *fn = init->token->value;
            bool sameAsArg = strcmp(fn, "aether_sorted") == 0 || strcmp(fn, "aether_sorted_desc") == 0 ||
                             strcmp(fn, "aether_sorted_by") == 0 || strcmp(fn, "aether_sorted_by_desc") == 0 ||
                             strcmp(fn, "aether_reversed") == 0;
            bool elemOfArg = strcmp(fn, "aether_sum") == 0 || strcmp(fn, "aether_min_of") == 0 ||
                             strcmp(fn, "aether_max_of") == 0;
            if (sameAsArg || elemOfArg) {
                char *at = inferLetTypeName(p, init->children[0]);
                if (at && elemOfArg) {
                    size_t n = strlen(at);
                    if (n >= 2 && strcmp(at + n - 2, "[]") == 0 && aetherTypeNameRank(at) == 1) {
                        at[n - 2] = '\0';
                    } else {
                        free(at);
                        at = NULL;
                    }
                }
                if (at) return at;
            }
        }
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

/* The declared Aether type name of field `field` of record type `recType`, from
 * the table parseTypeDecl fills ("Type.field" -> name), or NULL. With a NULL
 * `field`: non-NULL when the table holds any field of `recType` at all. The
 * field name compares exactly, as the backend's own field lookup does. */
static const char *aetherFieldTypeName(const AetherParser *p, const char *recType,
                                       const char *field) {
    if (!p || !p->fieldTypes || !recType) return NULL;
    size_t recLen = strlen(recType);
    for (size_t i = 0; i < p->fieldTypes->count; i++) {
        const char *key = p->fieldTypes->items[i].name;
        if (strncasecmp(key, recType, recLen) != 0 || key[recLen] != '.') continue;
        if (!field || strcmp(key + recLen + 1, field) == 0)
            return p->fieldTypes->items[i].typeName;
    }
    return NULL;
}

/* The Aether type name of a slice's base (`base[lo..hi]`), or NULL.
 *
 * inferLetTypeName names a local, a parameter, a global and a call result, and
 * it is asked first, so those bases keep the type they always had. What it
 * cannot name is a base reached through a record: `o.f`, `o.inner.f`, `self.f`
 * in a method, `items[i].f`, `o.inner.m()`. Those resolve here, from the
 * receiver's type and the field's (or method's) declared type. Without a name
 * the slice could not pick its lowering (a Text is `copy`, an array a hoisted
 * temp), and the temp it declared had no type node, which rea's resolver reads
 * as "no declaration": every use of the temp was "[SCOPE-001] identifier
 * '__aether_slice_N' not in scope".
 *
 * Kept apart from inferLetTypeName on purpose: an inferred `let x = o.f;`, the
 * `.length` property and the array un-alias steps all ask that function, and
 * teaching it fields would change what they do. */
char *aetherInferSliceBaseTypeName(AetherParser *p, AST *base) {
    if (!p || !base) return NULL;
    char *known = inferLetTypeName(p, base);
    if (known) return known;
    switch (base->type) {
        case AST_VARIABLE:
            /* `self` inside a `type` body has no binding of its own. */
            if (base->token && base->token->value && p->currentClassName &&
                (strcasecmp(base->token->value, "myself") == 0 ||
                 strcasecmp(base->token->value, "my") == 0))
                return strdup(p->currentClassName);
            /* A `@post` clause's `result` has the function's return type (it is
             * never in the binding table; see contractOperandTypeName). */
            if (base->token && base->token->value && p->detachedText &&
                p->currentReturnTypeName && strcmp(base->token->value, "result") == 0)
                return strdup(p->currentReturnTypeName);
            return NULL;
        case AST_FIELD_ACCESS: {
            if (!base->left || !base->token || !base->token->value) return NULL;
            char *recv = aetherInferSliceBaseTypeName(p, base->left);
            const char *ft = aetherFieldTypeName(p, recv, base->token->value);
            free(recv);
            return ft ? strdup(ft) : NULL;
        }
        case AST_ARRAY_ACCESS: {
            /* An element of an array reached through a record: `o.rows[i]`. */
            if (!base->left) return NULL;
            char *arr = aetherInferSliceBaseTypeName(p, base->left);
            if (arr && aetherTypeNameIsArray(arr)) {
                arr[strlen(arr) - 2] = '\0';
                return arr;
            }
            free(arr);
            return NULL;
        }
        case AST_PROCEDURE_CALL: {
            /* A method call on such a receiver (`o.inner.m()`, `items[i].m()`).
             * parsePostfix mangles the callee to `Type.m` only for a plain
             * variable receiver; here the receiver's type gives the same key. */
            if (!base->left || !base->token || !base->token->value || !p->funcReturns ||
                strchr(base->token->value, '.'))
                return NULL;
            char *recv = aetherInferSliceBaseTypeName(p, base->left);
            const char *rt = NULL;
            if (recv) {
                size_t keyLen = strlen(recv) + 1 + strlen(base->token->value) + 1;
                char *key = (char *)malloc(keyLen);
                if (key) {
                    snprintf(key, keyLen, "%s.%s", recv, base->token->value);
                    rt = bindingTableGet(p->funcReturns, key, strlen(key));
                    free(key);
                }
            }
            free(recv);
            return rt ? strdup(rt) : NULL;
        }
        default:
            return NULL;
    }
}

/* For the slice diagnostic: when `base` is `recv.field`, `recv` is a record
 * type this program declares and the type has no such field, that type's name
 * (malloc'd). NULL otherwise. */
char *aetherSliceBaseMissingFieldOwner(AetherParser *p, AST *base) {
    if (!p || !base || base->type != AST_FIELD_ACCESS || !base->left || !base->token ||
        !base->token->value)
        return NULL;
    char *recv = aetherInferSliceBaseTypeName(p, base->left);
    if (recv && aetherFieldTypeName(p, recv, NULL) &&
        !aetherFieldTypeName(p, recv, base->token->value))
        return recv;
    free(recv);
    return NULL;
}

/* For the slice diagnostic: is `name` a field of the type whose method is
 * being parsed (so a bare `name[a..b]` was meant as `self.name[a..b]`)? */
bool aetherIsEnclosingTypeField(AetherParser *p, const char *name) {
    return p && name && p->currentClassName &&
           aetherFieldTypeName(p, p->currentClassName, name) != NULL;
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
