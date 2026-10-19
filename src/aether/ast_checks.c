/*
 * ast_checks.c -- front-end checks over the parsed Aether AST.
 *
 * Undefined `Type.method` calls (aetherCheckMemberCalls, SCOPE-001), the
 * FLOW-001 fallthrough helpers, the PAR-001 shared-record guard, the
 * ARR-002 rank check and the TOON-001/MS-001 handle-index check. Split out
 * of ast_parser.c; see ast_internal.h.
 */

#include "aether/ast_internal.h"

/* ARR-002: a second index applied to a variable declared with only
 * one `[]`. `let dp: Int[] = []; dp[i][j]` is the DP-table shape
 * models write when they want a 2-D table -- they declare the rank
 * wrong rather than the syntax. It used to compile and die at
 * runtime with an uncoded "Expected a pointer to an array for
 * element access.", which names neither the variable nor the rank,
 * so the reader has nothing to act on. Aether does have real nested
 * arrays, so the fix is a declaration change, not a redesign.
 *
 * Only fires when the base is a plain variable whose declared type
 * name is known and has exactly one `[]`. A slice (`xs[a..b][i]`)
 * lowers to a temp variable before reaching here, and a field base
 * (`self.rows[i][j]`) is not AST_VARIABLE, so neither is judged. A
 * `Text[]` is exempt: its second index is character access.
 *
 * Called from parsePostfix with `node` the array access about to be indexed
 * again. Reports the error, sets p->hadError and returns true when the index
 * is rejected; the caller frees the operands. */
bool aetherCheckArrayRankIndex(AetherParser *p, AST *node, int openLine) {
    if (node && node->type == AST_ARRAY_ACCESS && node->left &&
        node->left->type == AST_VARIABLE && node->left->token &&
        node->left->token->value && p->bindings) {
        const char *baseName = node->left->token->value;
        const char *declared = bindingTableGet(p->bindings, baseName,
                                               strlen(baseName));
        /* `lines[i][j]` on a `Text[]` is a character of element i, not a
         * second dimension. */
        bool textElems = declared && strncasecmp(declared, "Text[", 5) == 0;
        if (declared && !textElems && aetherTypeNameRank(declared) == 1) {
            char detail[256];
            char hint[256];
            snprintf(detail, sizeof(detail),
                     "'%s' is declared '%s', a one-dimensional array, but is "
                     "indexed twice here.", baseName, declared);
            snprintf(hint, sizeof(hint),
                     "declare it as '%s[]' for a real 2-D array (rows are "
                     "themselves arrays: `row = row + [v];` then "
                     "`%s = %s + [row];`), or index it once with a computed "
                     "offset such as `%s[r * width + c]`.",
                     declared, baseName, baseName, baseName);
            reportAetherAstError(aetherSemanticGetSourcePath(), openLine,
                                 "array-rank", detail, hint);
            p->hadError = true;
            return true;
        }
    }
    return false;
}

/* TOON-001 / MS-001: `[]` on an opaque handle (`ToonDoc`, `ToonNode`,
 * `MStream`). A handle is an Int slot on the VM, so `items[i]` on a TOON array
 * node compiled and died at run time with an uncoded "Pointer does not point
 * to an array for element access."; the slice form was coded TYPE-001 but its
 * hint ("index one element with `[i]`") led straight into that crash. A TOON
 * array is read with `toon_at(node, i)` and `toon_len(node)`; a stream's bytes
 * with `mstream_text(ms)`, which is an indexable Text.
 *
 * `base` is the value about to be indexed; any expression whose type
 * aetherInferSliceBaseTypeName can name is judged (a handle local or
 * parameter, a `toon_key(...)` call, a handle field such as `h.node`). A
 * `ToonNode[]` is a real array and indexes normally. Reports, sets
 * p->hadError and returns true when the index is rejected; the caller frees
 * the operands. */
bool aetherCheckHandleIndex(AetherParser *p, AST *base, int openLine, bool slice) {
    if (!base) return false;
    char *tn = aetherInferSliceBaseTypeName(p, base);
    if (!tn) return false;
    bool isDoc = strcasecmp(tn, "ToonDoc") == 0;
    bool isNode = strcasecmp(tn, "ToonNode") == 0;
    bool isStream = strcasecmp(tn, "MStream") == 0;
    free(tn);
    if (!isDoc && !isNode && !isStream) return false;

    const char *typeName = isDoc ? "ToonDoc" : isNode ? "ToonNode" : "MStream";
    const char *article = isStream ? "an" : "a";
    const char *family = isStream ? "MStream" : "TOON";
    const char *verb = slice ? "slice" : "index";
    const char *form = slice ? "[a..b]" : "[]";
    const char *baseName = (base->type == AST_VARIABLE && base->token && base->token->value)
                               ? base->token->value : NULL;
    const char *shown = baseName ? baseName : (isDoc ? "doc" : isNode ? "node" : "ms");
    char detail[256];
    char hint[512];
    if (baseName) {
        snprintf(detail, sizeof(detail),
                 "cannot %s '%.64s' with `%s`: it is %s %s, an opaque %s handle, "
                 "not an array.", verb, baseName, form, article, typeName, family);
    } else {
        snprintf(detail, sizeof(detail),
                 "cannot %s %s %s with `%s`: it is an opaque %s handle, not an array.",
                 verb, article, typeName, form, family);
    }
    if (isNode) {
        snprintf(hint, sizeof(hint),
                 "read element i of a TOON array with `toon_at(%.64s, i)` and its length "
                 "with `toon_len(%.64s)`: `loop i in 0..toon_len(%.64s) { let el: ToonNode = "
                 "toon_at(%.64s, i); ... }`.",
                 shown, shown, shown, shown);
    } else if (isDoc) {
        snprintf(hint, sizeof(hint),
                 "take the root node first: `let root: ToonNode = toon_root(%.64s);`, then "
                 "`toon_at(root, i)` for an array element (`toon_len(root)` for the length) "
                 "or `toon_key(root, \"name\")` for a field.",
                 shown);
    } else {
        snprintf(hint, sizeof(hint),
                 "copy the contents out first: `let text: Text = mstream_text(%.64s);`, then "
                 "index or slice `text`.",
                 shown);
    }
    reportAetherAstError(aetherSemanticGetSourcePath(), openLine, "type", detail, hint);
    p->hadError = true;
    return true;
}

/* FMT-001 (W6-04, D48): a placeholder in the first argument of a print call
 * that has more arguments. Aether's print/println concatenate their arguments,
 * so `println("{} items", n)` printed `{} items5` with exit 0 -- a silent wrong
 * answer that the stdout diff alone did not get models to repair. Placeholders
 * are `{}`, `{0}`, `{:spec}`, `{name}` / `{name:spec}` with `name` a binding in
 * scope, `${`, and a printf conversion `%[-+0#]*\d*(\.\d+)?[dsfixeg]`. A call
 * with only the literal (`println("{}")`) and JSON-ish `{"` text never fire.
 * Writes the placeholder found into `out` and returns true. */
static bool aetherFindPlaceholder(AetherParser *p, const char *s, char *out, size_t outLen) {
    for (const char *c = s; c && *c; c++) {
        const char *end = NULL;
        if (c[0] == '$' && c[1] == '{') {
            const char *close = strchr(c + 2, '}');
            end = (close && close - c <= 24) ? close + 1 : c + 2;
        } else if (c[0] == '{') {
            const char *q = c + 1;
            if (*q == '}') {
                end = q + 1;
            } else if (*q >= '0' && *q <= '9') {
                while (*q >= '0' && *q <= '9') q++;
                if (*q == '}' || *q == ':') end = strchr(q, '}');
                if (end) end++;
            } else if (*q == ':') {
                const char *close = strchr(q, '}');
                if (close && close - q <= 12) end = close + 1;
            } else if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '_') {
                const char *n0 = q;
                while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                       (*q >= '0' && *q <= '9') || *q == '_')
                    q++;
                if ((*q == '}' || *q == ':') && p && p->bindings &&
                    bindingTableGet(p->bindings, n0, (size_t)(q - n0))) {
                    const char *close = strchr(q, '}');
                    if (close) end = close + 1;
                }
            }
        } else if (c[0] == '%') {
            const char *q = c + 1;
            while (*q == '-' || *q == '+' || *q == '0' || *q == '#') q++;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == '.') {
                q++;
                if (!(*q >= '0' && *q <= '9')) continue;
                while (*q >= '0' && *q <= '9') q++;
            }
            if (*q && strchr("dsfixeg", *q)) end = q + 1;
        }
        if (end) {
            size_t n = (size_t)(end - c);
            if (n >= outLen) n = outLen - 1;
            memcpy(out, c, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

bool aetherCheckPrintPlaceholders(AetherParser *p, AST *call, const char *surface, int line) {
    if (!call || (call->type != AST_WRITE && call->type != AST_WRITELN)) return false;
    if (call->child_count < 2) return false;
    AST *first = call->children[0];
    if (!first || first->type != AST_STRING || !first->token || !first->token->value) return false;
    char found[32];
    if (!aetherFindPlaceholder(p, first->token->value, found, sizeof(found))) return false;
    const char *name = surface && *surface ? surface
                       : (call->type == AST_WRITELN ? "println" : "print");
    char detail[256];
    snprintf(detail, sizeof(detail),
             "%s has no placeholders: \"%s\" prints literally and the values are appended.",
             name, found);
    reportAetherAstError(aetherSemanticGetSourcePath(), line, "format", detail,
                         "pass values as separate arguments: `println(\"n = \", n)`; "
                         "decimals: `formatfloat(x, 2)`");
    p->hadError = true;
    return true;
}

/* W6-05 (D7): a method call or property on a builtin-typed receiver. Aether's
 * builtin types have no methods, and models arrive with JS/Java/Python
 * spellings: `xs.push(4)` was a file-less "L3: Compiler error: Unknown field
 * Int[].push", `xs.size()` / `s.contains(t)` / `n.toString()` passed --strict
 * and died at run time ("Undefined global variable"), `s.toUpper()` printed
 * the first letter and `t.pos(",")` silently swapped its arguments through
 * UFCS. The receiver's type must be positively known (a binding, a call's
 * return type); a user record never reaches here. */

/* Int, Real, Text, Bool (and their accepted spellings) or any array. */
bool aetherIsBuiltinValueTypeName(const char *t) {
    if (!t || !*t) return false;
    if (aetherTypeNameIsArray(t)) return true;
    return strcmp(t, "Int") == 0 || strcmp(t, "Real") == 0 || strcmp(t, "Text") == 0 ||
           strcmp(t, "Bool") == 0 || strcmp(t, "Float") == 0 || strcmp(t, "String") == 0;
}

typedef struct {
    const char *names; /* space-separated, matched case-insensitively */
    const char *hint;  /* %s: the receiver type */
} AetherForeignMethod;

static const char *foreignMethodHint(const char *recvType, const char *m, char *buf, size_t n) {
    bool isArray = aetherTypeNameIsArray(recvType);
    bool isText = strcmp(recvType, "Text") == 0 || strcmp(recvType, "String") == 0;
    bool isInt = strcmp(recvType, "Int") == 0;
    bool isReal = strcmp(recvType, "Real") == 0 || strcmp(recvType, "Float") == 0;
    /* Array-only advice first; a scalar receiver falls through to the rest. The
     * modern surface (D56, D57) has a function for most of these: the hint
     * names it, since a method spelling stays SCOPE-001 (W6-05). */
    static const AetherForeignMethod arrayTable[] = {
        {" push append add push_back pushback extend ", "append with `xs = xs + [v];`"},
        {" pop remove clear insert splice shift unshift removeat delete ",
         "build the new array with a loop, or take a slice `xs[a..b]`"},
        {" sort ", "write `sorted(xs)` (a new array, ascending) or `sorted_desc(xs)`; "
                   "records: `sorted_by(xs, \"field\")`"},
        {" sorted ", "write `sorted(xs)` (a new array, ascending) or `sorted_desc(xs)`"},
        {" reverse reversed ", "write `reversed(xs)` (a new array)"},
        {" join ", "write `join(xs, sep)`"},
        {" sum ", "write `sum(xs)`"},
        {" min ", "write `min_of(xs)`"},
        {" max ", "write `max_of(xs)`"},
        {NULL, NULL}};
    static const AetherForeignMethod table[] = {
        {" map filter foreach each reduce any all ",
         "there are no closures: write a `loop x in xs { ... }`"},
        {NULL, NULL}};
    char key[72];
    snprintf(key, sizeof(key), " %s ", m);
    for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
    if (isText || isArray) {
        const char *what = isText ? "s" : "xs";
        const char *arg = isText ? "x" : "v";
        if (strstr(" contains includes has ", key)) {
            snprintf(buf, n, "write `contains(%s, %s)`", what, arg);
            return buf;
        }
        if (strstr(" indexof index find findindex pos ", key)) {
            snprintf(buf, n, "write `index_of(%s, %s)`: -1 when absent", what, arg);
            return buf;
        }
        if (strstr(" count ", key)) {
            snprintf(buf, n, "write `count(%s, %s)` for how many times %s occurs, `length(%s)` "
                     "for the size", what, arg, arg, what);
            return buf;
        }
    }
    if (isText) {
        if (strstr(" startswith starts_with hasprefix ", key)) return "write `starts_with(s, p)`";
        if (strstr(" endswith ends_with hassuffix ", key)) return "write `ends_with(s, p)`";
        if (strstr(" lower tolower tolowercase lowercase downcase ", key)) return "write `lower(s)`";
        if (strstr(" upper toupper touppercase uppercase upcase ", key)) return "write `upper(s)`";
        if (strstr(" capitalize ", key)) return "write `upper(s[..1]) + s[1..]`";
        /* D67: the character classes are functions, as upper(s) is. */
        if (strstr(" isdigit is_digit isnumeric is_numeric isdecimal ", key))
            return "write `is_digit(s)`";
        if (strstr(" isalpha is_alpha isletter is_letter ", key)) return "write `is_alpha(s)`";
        if (strstr(" isalnum is_alnum ", key)) return "write `is_alnum(s)`";
        if (strstr(" isspace is_space iswhitespace is_whitespace ", key))
            return "write `is_space(s)`";
        if (strstr(" isupper is_upper ", key)) return "write `is_upper(s)`";
        if (strstr(" islower is_lower ", key)) return "write `is_lower(s)`";
        if (strstr(" replace replaceall replace_all ", key)) return "write `replace(s, old, new)`";
        /* `s.trim()` and `s.split(sep)` stay UFCS calls of the builtins
         * (fixtures diag/ufcs_builtin_pass); only the foreign spelling is a
         * hint. */
        if (strstr(" strip ", key)) return "write `trim(s)`";
        if (strstr(" repeat ", key)) return "write `repeat(s, n)`";
        if (strstr(" reverse reversed ", key)) return "write `reversed(s)`";
        if (strstr(" join ", key)) return "write `join(xs, sep)`: the Text[] comes first";
        if (strstr(" substring substr slice ", key)) return "take a slice: `s[a..b]`";
    }
    if (strstr(" tostring to_string str tostr totext to_text ", key)) {
        if (isInt) return "write `int_to_text(n)`";
        if (isReal) return "write `formatfloat(x, 2)`";
        return "write `int_to_text(n)` for an Int, `formatfloat(x, 2)` for a Real";
    }
    if (strstr(" toint to_int parseint tointeger ", key)) {
        return isText ? "write `parse_int(t)`, or `try_parse_int(t)` to check it"
                      : "write `trunc(x)` or `round(x)`";
    }
    if (strstr(" tofloat toreal parsefloat todouble ", key)) return "write `parse_float(t)`";
    if (strstr(" size ", key) && (isArray || isText))
        return isText ? "write `length(s)`" : "write `length(xs)`";
    if (isArray) {
        for (int i = 0; arrayTable[i].names; i++) {
            if (strstr(arrayTable[i].names, key)) return arrayTable[i].hint;
        }
    }
    for (int i = 0; table[i].names; i++) {
        if (strstr(table[i].names, key)) return table[i].hint;
    }
    return NULL;
}

/* `callFormHint` is set when `method` is a builtin known only by its alias
 * (round_to, index_of, ...): the UFCS call could not reach it, so it is
 * SCOPE-001 with that hint unless the foreign-method table has a better one. */
bool aetherCheckBuiltinMethod(AetherParser *p, const char *recvType, const char *method,
                              bool isCall, bool knownCallable, const char *callFormHint,
                              int line) {
    if (!aetherIsBuiltinValueTypeName(recvType) || !method) return false;
    char buf[160];
    const char *hint = foreignMethodHint(recvType, method, buf, sizeof(buf));
    if (!hint && isCall && callFormHint) hint = callFormHint;
    if (!hint && isCall && knownCallable) return false; /* UFCS: f(recv, args) */
    if (!hint && isCall)
        hint = "Aether types have no methods: call a function with the value as an argument";
    if (!hint && !isCall) return false; /* a property: left to the field-access path */
    const char *path = aetherSemanticGetSourcePath();
    if (path && *path) aetherDiagf("%s:%d: ", path, line > 0 ? line : 1);
    aetherDiagf("[SCOPE-001] Aether method error: %s has no %s '%s'.\n", recvType,
                isCall ? "method" : "field", method);
    aetherDiagf("hint: %s.\n", hint);
    aetherReportGuideHelp("SCOPE-001");
    p->hadError = true;
    return true;
}

/* PAR-001: reject a record shared across par branches before it becomes a
 * concurrent double-free at runtime. Scan this call's argument variables
 * (children[0] is the receiver for a method call); a bare identifier whose
 * declared type is a user aggregate (record/array -- pointer-backed, so it
 * aliases across threads) is flagged if a *different* branch already passed
 * the same name. Scalars (Int/Real/Text/Bool/...) are value-copied per
 * thread, so they are never flagged.
 *
 * Called from parseParBlock once per branch call; `handle` is the branch
 * number. Reports the error, sets p->hadError and returns true when the call
 * is rejected; the caller frees the par block. */
bool aetherCheckParSharedRecords(AetherParser *p, AST *call, int handle, int callLine,
                                 AetherParSharedRec *sharedRecs, int *sharedRecCount,
                                 int sharedRecCap) {
    for (int ci = 0; ci < call->child_count; ci++) {
        AST *arg = call->children[ci];
        if (!arg || arg->type != AST_VARIABLE || !arg->token || !arg->token->value) continue;
        const char *an = arg->token->value;
        const char *aty = bindingTableGet(p->bindings, an, strlen(an));
        if (!aty) continue;
        VarType avt = TYPE_UNKNOWN; const char *arn = NULL;
        if (mapAetherType(aty, strlen(aty), &arn, &avt)) continue; /* scalar -> safe */
        int prior = -1;
        for (int si = 0; si < *sharedRecCount; si++) {
            if (sharedRecs[si].name && strcmp(sharedRecs[si].name, an) == 0) {
                prior = sharedRecs[si].branch;
                break;
            }
        }
        if (prior >= 0 && prior != handle) {
            const char *ppath = aetherSemanticGetSourcePath();
            if (ppath && *ppath) aetherDiagf("%s:%d: ", ppath, callLine);
            aetherDiagf("[PAR-001] Aether par error: record '%s' is shared by more than one par "
                        "branch; the spawned threads write it concurrently, which races (heap "
                        "corruption).\n", an);
            aetherDiagf("hint: give each par branch its own record, then combine the results "
                        "after the `par { ... }` block.\n");
            aetherReportGuideHelp("PAR-001");
            p->hadError = true;
            return true;
        }
        if (prior < 0 && *sharedRecCount < sharedRecCap) {
            sharedRecs[*sharedRecCount].name = an;
            sharedRecs[*sharedRecCount].branch = handle;
            (*sharedRecCount)++;
        }
    }
    return false;
}

/* True if the subtree contains a value-bearing `ret <expr>` (AST_RETURN with a
 * non-NULL value in ->left). Does not descend into nested function/procedure
 * declarations, whose returns belong to them. Mirrors the rewriter's
 * "sawValueReturn" used for the FLOW-001 fallthrough check. */
bool astHasValueReturn(const AST *node) {
    if (!node) return false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) return false;
    if (node->type == AST_RETURN && node->left) return true;
    if (astHasValueReturn(node->left)) return true;
    if (astHasValueReturn(node->right)) return true;
    if (astHasValueReturn(node->extra)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (astHasValueReturn(node->children[i])) return true;
    }
    return false;
}

/* True if the block has a top-level statement that "falls through": an
 * expression/assignment/call, not a declaration, control-flow construct, or
 * return. Mirrors the rewriter's sawFallthroughTopLevelStmt (translate.c ~8966,
 * which excludes if/else/loop/for/while/let/const/fn), so an all-declarations
 * body (e.g. a malformed unclosed `fn`) is not mistaken for a fallthrough. */
bool astBlockHasFallthroughStmt(const AST *block) {
    if (!block) return false;
    for (int i = 0; i < block->child_count; i++) {
        const AST *c = block->children[i];
        if (!c) continue;
        switch (c->type) {
            case AST_VAR_DECL:
            case AST_CONST_DECL:
            case AST_FUNCTION_DECL:
            case AST_PROCEDURE_DECL:
            case AST_IF:
            case AST_WHILE:
            case AST_RETURN:
                break; /* declaration / control-flow / return: not a fallthrough */
            default:
                return true;
        }
    }
    return false;
}

/* True if `node`'s subtree declares a function/procedure whose (mangled) name
 * matches `mangled`, case-insensitively (mirroring the compiler's lookup, which
 * lowercases). Used to decide whether a `Type.method` call resolves to a real
 * method. */
static bool aetherProcDefined(AST *node, const char *mangled) {
    if (!node || !mangled) return false;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) &&
        node->token && node->token->value &&
        strcasecmp(node->token->value, mangled) == 0) {
        return true;
    }
    AST *c0 = node->child_count > 0 ? node->children[0] : NULL;
    if (node->left && node->left != c0 && aetherProcDefined(node->left, mangled)) return true;
    if (node->right && aetherProcDefined(node->right, mangled)) return true;
    if (node->extra && aetherProcDefined(node->extra, mangled)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (aetherProcDefined(node->children[i], mangled)) return true;
    }
    return false;
}

/* True if the AST_RECORD_TYPE `recordAst` declares a field named `field`
 * (case-insensitive). Only immediate fields -- callers that reach here have already
 * excluded types with a parent, so inherited fields cannot matter. */
static bool aetherRecordHasField(AST *recordAst, const char *field) {
    if (!recordAst || !field) return false;
    for (int i = 0; i < recordAst->child_count; i++) {
        AST *decl = recordAst->children[i];
        if (!decl || decl->type != AST_VAR_DECL) continue;
        for (int j = 0; j < decl->child_count; j++) {
            AST *v = decl->children[j];
            if (v && v->token && v->token->value &&
                strcasecmp(v->token->value, field) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* Walk the whole program; for every `Type.member(...)` call where the parser
 * mangled a record-receiver method, verify the method exists. Emits a compile-time
 * [SCOPE-001] for each undefined method and returns the count. Conservative by
 * construction so it never rejects a valid benchmark-corpus program:
 *   - the pre-dot segment must name a real user record type (lookupType), never a
 *     module/unit qualifier or a builtin;
 *   - the record must have NO parent type -- inheritance can define the method on
 *     an ancestor under a different mangling, so skip to stay safe;
 *   - a matching field is left alone (not our concern here);
 *   - only single-dot names (the `Type.method` mangling) are considered. */
int aetherCheckMemberCalls(AST *node, AST *decls) {
    if (!node) return 0;
    int errs = 0;
    if (node->type == AST_PROCEDURE_CALL && node->token && node->token->value) {
        const char *name = node->token->value;
        const char *dot = strchr(name, '.');
        if (dot && dot != name && dot[1] != '\0' && strchr(dot + 1, '.') == NULL) {
            size_t tlen = (size_t)(dot - name);
            char typeName[128];
            if (tlen > 0 && tlen < sizeof(typeName)) {
                memcpy(typeName, name, tlen);
                typeName[tlen] = '\0';
                const char *member = dot + 1;
                AST *rec = lookupType(typeName);
                /* Resolved if EITHER a true method `Type.member` exists, OR a plain
                 * top-level `fn member(self: Type, ...)` extension method exists --
                 * those stay un-mangled in the AST (registered as `Type.member` only
                 * in funcReturns), and a `recv.member()` call site is still valid via
                 * UFCS. Checking the un-mangled `member` decl covers that without a
                 * false positive; missing an unrelated same-named fn only *under*-
                 * reports, which is the safe direction. */
                if (rec && rec->type == AST_RECORD_TYPE && rec->extra == NULL &&
                    !aetherProcDefined(decls, name) &&
                    !aetherProcDefined(decls, member) &&
                    !aetherRecordHasField(rec, member)) {
                    const char *path = aetherSemanticGetSourcePath();
                    int line = node->token->line > 0 ? node->token->line : 1;
                    if (path && *path) aetherDiagf("%s:%d: ", path, line);
                    aetherDiagf("[SCOPE-001] Aether method error: method '%s' is not defined on "
                                "type '%s'.\n", member, typeName);
                    aetherDiagf("hint: define `fn %s(...)` inside `type %s { ... }`, or fix the "
                                "method name (methods do not capture outer locals -- METH-001).\n",
                                member, typeName);
                    aetherReportGuideHelp("SCOPE-001");
                    errs++;
                }
                /* A builtin type name here fabricates a transient node (it
                 * fails the AST_RECORD_TYPE test above, so it never produces a
                 * false SCOPE-001) -- release it rather than leaking it once
                 * per `Type.member` call site. */
                releaseTransientTypeNode(rec);
            }
        }
    }
    AST *c0 = node->child_count > 0 ? node->children[0] : NULL;
    if (node->left && node->left != c0) errs += aetherCheckMemberCalls(node->left, decls);
    if (node->right) errs += aetherCheckMemberCalls(node->right, decls);
    if (node->extra) errs += aetherCheckMemberCalls(node->extra, decls);
    for (int i = 0; i < node->child_count; i++) {
        errs += aetherCheckMemberCalls(node->children[i], decls);
    }
    return errs;
}
