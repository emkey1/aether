/*
 * ast_stdlib.c -- the modern standard surface in the front end (D55-D61).
 *
 * Two jobs. The Pascal-era builtins the engine still has for its other front
 * ends (copy, pos, val, the File sequence, paramstr, inttostr, the unprefixed
 * mstream* names, ...) are not Aether: a call to one is a SCOPE-001 whose hint
 * names the modern form (class 2, tests/surface/registry.json). And
 * sorted_by / sorted_by_desc take their field name as a literal, checked here
 * against the element type's fields (FIELD-002) and lowered to the field's
 * record slot, because the engine's records carry no field names.
 *
 * The engine half of the helpers is pscal-core's
 * src/ext_builtins/strings/stdlib.c.
 */

#include "aether/ast_internal.h"

/* A removed spelling and the modern form its hint names. Matched
 * case-insensitively, as the engine resolves builtin names. */
typedef struct {
    const char *name;
    const char *hint;
} AetherRemovedBuiltin;

#define AETHER_FILE_HINT \
    "Aether has no file handles: read a whole file with `read_file(path)` (Text) or " \
    "`read_lines(path)` (Text[]); write with `write_file(path, text)` or " \
    "`append_file(path, text)`"

#define AETHER_STDIN_HINT \
    "read stdin with `read_line()` (one line, \"\" at the end) or `read_all_stdin()` " \
    "(all of it); a file with `read_lines(path)`"

static const AetherRemovedBuiltin kAetherRemovedBuiltins[] = {
    { "copy", "take a slice: `s[a..b]` (from a, up to but not including b), `s[a..]` or `s[..b]`" },
    { "pos", "write `index_of(s, needle)` (-1 when absent; the text comes first), or "
             "`contains(s, needle)`, `starts_with(s, p)`, `ends_with(s, p)`" },
    { "stringofchar", "write `repeat(s, n)`" },
    { "val", "write `let (n, ok) = try_parse_int(t);` (`ok` is false for \"\" or \"12x\")" },
    { "valreal", "write `let (x, ok) = try_parse_real(t);`" },
    { "inttostr", "write `int_to_text(n)`" },
    { "itoa", "write `int_to_text(n)`" },
    { "assign", AETHER_FILE_HINT },
    { "assignfile", AETHER_FILE_HINT },
    { "reset", AETHER_FILE_HINT },
    { "rewrite", AETHER_FILE_HINT },
    { "close", AETHER_FILE_HINT },
    { "closefile", AETHER_FILE_HINT },
    { "append", "add to an array with `xs = xs + [v];`; add to a file with "
                "`append_file(path, text)`" },
    { "eof", AETHER_STDIN_HINT },
    { "readln", AETHER_STDIN_HINT },
    { "read", AETHER_STDIN_HINT },
    { "erase", "write `delete_file(path)` (true when it was deleted)" },
    { "rename", "write `rename_file(from, to)` (true when it was renamed)" },
    { "paramcount", "write `length(args())`: `args()` is a Text[] of the arguments after the program" },
    { "paramstr", "write `args()[i]`: `args()` is a Text[] of the arguments after the program, "
                  "`args()[0]` the first" },
    { "gettime", "write `clock_ms()` (milliseconds since 1970) or `now_text()` "
                 "(\"YYYY-MM-DD HH:MM:SS\")" },
    { "getdate", "write `now_text()` (\"YYYY-MM-DD HH:MM:SS\") or `clock_ms()`" },
    { "setlength", "arrays are values: shrink with a slice `xs = xs[..n];`, grow with "
                   "`xs = xs + [v];`" },
    { "mstreamcreate", "write `mstream_new()`" },
    { "mstreamfromstring", "write `mstream_from_text(t)`" },
    { "mstreambuffer", "write `mstream_text(ms)`" },
    { "mstreamappendbyte", "write `mstream_append_byte(ms, b)`" },
    { "mstreamloadfromfile", "write `let ms = mstream_load(path);` (it returns a new MStream)" },
    { "mstreamsavetofile", "write `mstream_save(ms, path)`" },
    { "mstreamfree", "write `mstream_free(ms)`" },
};

const char *aetherRemovedBuiltinHint(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(kAetherRemovedBuiltins) / sizeof(kAetherRemovedBuiltins[0]); i++) {
        if (strcasecmp(name, kAetherRemovedBuiltins[i].name) == 0) {
            return kAetherRemovedBuiltins[i].hint;
        }
    }
    return NULL;
}

/* A call to a removed builtin, by the spelling the user wrote (before any
 * alias). A user's own `fn NAME` is not ours to judge. Reports SCOPE-001 and
 * returns true when the call is rejected. */
bool aetherCheckRemovedBuiltin(AetherParser *p, const char *raw, int line) {
    if (!p || !raw || p->forwardScan || p->hadError) return false;
    const char *hint = aetherRemovedBuiltinHint(raw);
    if (!hint || aetherAstIsTopLevelUserFunction(raw)) return false;
    char detail[160];
    snprintf(detail, sizeof(detail), "identifier '%.64s' not in scope.", raw);
    reportAetherAstError(aetherSemanticGetSourcePath(), line, "scope", detail, hint);
    p->hadError = true;
    return true;
}

/* "Type.field#slot" -> slot, recorded by parseTypeDecl. */
void aetherRecordFieldSlot(AetherParser *p, const char *typeName, const char *field, int slot) {
    if (!p || !p->fieldTypes || !typeName || !field) return;
    char key[256];
    char val[16];
    snprintf(key, sizeof(key), "%s.%s#slot", typeName, field);
    snprintf(val, sizeof(val), "%d", slot);
    bindingTableSet(p->fieldTypes, key, val);
}

static bool aetherSortKeyTypeName(const char *t) {
    return t && (strcmp(t, "Int") == 0 || strcmp(t, "Real") == 0 || strcmp(t, "Text") == 0 ||
                 strcmp(t, "Float") == 0 || strcmp(t, "String") == 0);
}

static void reportSortedByField(AetherParser *p, int line, const char *detail, const char *hint) {
    const char *path = aetherSemanticGetSourcePath();
    if (path && *path) aetherDiagf("%s:%d: ", path, line > 0 ? line : 1);
    aetherDiagf("[FIELD-002] Aether field error: %s\n", detail);
    if (hint && *hint) aetherDiagf("hint: %s\n", hint);
    aetherReportGuideHelp("FIELD-002");
    p->hadError = true;
}

/* sorted_by(xs, "field") / sorted_by_desc(xs, "field"): `xs` is an array of a
 * record type and the second argument a string literal naming one of its
 * Int, Real or Text fields. The literal becomes that field's slot. Returns
 * false (reported) when the call is rejected. */
static bool aetherLowerSortedBy(AetherParser *p, AST *call, const char *fn, int line) {
    char detail[320];
    /* A wrong argument count is semantic.c's BUILT-002. */
    if (call->child_count != 2) return true;
    AST *fieldArg = call->children[1];
    if (!fieldArg || fieldArg->type != AST_STRING || !fieldArg->token || !fieldArg->token->value) {
        snprintf(detail, sizeof(detail), "%s needs the field name as a string literal.", fn);
        reportSortedByField(p, line, detail,
                            "write the field's name in quotes, e.g. `sorted_by(people, \"age\")`");
        return false;
    }
    const char *field = fieldArg->token->value;
    char *arrType = inferLetTypeName(p, call->children[0]);
    if (!arrType) {
        /* The element type is not known here (an imported value, say); the
         * engine still sorts by name where it can, and fails at run time with
         * the field named. Nothing to check. */
        return true;
    }
    size_t n = strlen(arrType);
    if (n < 3 || strcmp(arrType + n - 2, "[]") != 0 || aetherTypeNameRank(arrType) != 1) {
        snprintf(detail, sizeof(detail), "%s sorts an array of records; this is a %s.", fn, arrType);
        reportSortedByField(p, line, detail,
                            "for an Int[], Real[] or Text[] write `sorted(xs)` or `sorted_desc(xs)`");
        free(arrType);
        return false;
    }
    arrType[n - 2] = '\0';
    char key[256];
    snprintf(key, sizeof(key), "%s.%s", arrType, field);
    const char *ftype = p->fieldTypes ? bindingTableGet(p->fieldTypes, key, strlen(key)) : NULL;
    snprintf(key, sizeof(key), "%s.%s#slot", arrType, field);
    const char *slot = p->fieldTypes ? bindingTableGet(p->fieldTypes, key, strlen(key)) : NULL;
    VarType vt = TYPE_UNKNOWN;
    const char *reaName = NULL;
    bool scalarElem = mapAetherType(arrType, strlen(arrType), &reaName, &vt);
    if (scalarElem) {
        snprintf(detail, sizeof(detail), "%s sorts an array of records; this is a %s[].", fn, arrType);
        reportSortedByField(p, line, detail, "write `sorted(xs)` or `sorted_desc(xs)`");
        free(arrType);
        return false;
    }
    if (!ftype || !slot) {
        snprintf(detail, sizeof(detail), "Unknown field '%.64s' in type '%.64s' (in %s).", field,
                 arrType, fn);
        reportSortedByField(p, line, detail, "name one of the type's Int, Real or Text fields");
        free(arrType);
        return false;
    }
    if (!aetherSortKeyTypeName(ftype)) {
        snprintf(detail, sizeof(detail),
                 "field '%.64s' of '%.64s' is a %.64s; %s sorts by an Int, Real or Text field.",
                 field, arrType, ftype, fn);
        reportSortedByField(p, line, detail, "name one of the type's Int, Real or Text fields");
        free(arrType);
        return false;
    }
    free(arrType);
    AST *slotNode = aetherBuildIntConst(strtol(slot, NULL, 10), line);
    if (!slotNode) return false;
    call->children[1] = slotNode;
    slotNode->parent = call;
    freeAST(fieldArg);
    return true;
}

/* Front-end checks and lowerings of a just-parsed call to a standard-library
 * helper, by its canonical name. Returns false (reported) to reject it. */
bool aetherLowerStdlibCall(AetherParser *p, AST *call, int line) {
    if (!p || !call || call->type != AST_PROCEDURE_CALL || !call->token || !call->token->value) {
        return true;
    }
    const char *fn = call->token->value;
    if (call->left || aetherAstIsTopLevelUserFunction(fn)) return true;
    if (strcmp(fn, "aether_sorted_by") == 0 || strcmp(fn, "aether_sorted_by_desc") == 0) {
        return aetherLowerSortedBy(p, call, fn + strlen("aether_"), line);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* try_parse_int / try_parse_real (D59)                                */
/* ------------------------------------------------------------------ */

/* Checked parsing returns a narrow tuple, `(value, ok)`, and tuples exist only
 * as the return type of a declared fn (D53). So the two helpers are declared
 * fns: one line of Aether each, over four hidden pure builtins, parsed from
 * their own text into the program's declarations when the source calls them
 * and does not define them itself (parseAetherAst). The user's source is never
 * edited, so no line moves and script-mode programs are unaffected. */
static const struct {
    const char *name;
    const char *decl;
} kAetherPreludeFns[] = {
    { "try_parse_int",
      "@pure\nfn try_parse_int(t: Text) -> (Int, Bool) { ret (aetherparseintvalue(t), "
      "aetherparseintok(t)); }" },
    { "try_parse_real",
      "@pure\nfn try_parse_real(t: Text) -> (Real, Bool) { ret (aetherparserealvalue(t), "
      "aetherparserealok(t)); }" },
};

/* Does `source` call `name(` (outside strings and comments), and does it
 * declare `fn name` itself? */
static void aetherScanPreludeUse(const char *source, const char *name, bool *called,
                                 bool *declared) {
    size_t nlen = strlen(name);
    const char *c = source;
    const char *prevWord = NULL;
    size_t prevLen = 0;
    *called = false;
    *declared = false;
    while (*c) {
        if (c[0] == '/' && c[1] == '/') {
            while (*c && *c != '\n') c++;
            continue;
        }
        if (c[0] == '/' && c[1] == '*') {
            c += 2;
            while (*c && !(c[0] == '*' && c[1] == '/')) c++;
            if (*c) c += 2;
            continue;
        }
        if (*c == '"' || *c == '\'') {
            char q = *c++;
            while (*c && *c != q) {
                if (*c == '\\' && c[1]) c++;
                c++;
            }
            if (*c) c++;
            continue;
        }
        if (isalpha((unsigned char)*c) || *c == '_') {
            const char *w = c;
            while (isalnum((unsigned char)*c) || *c == '_') c++;
            size_t wl = (size_t)(c - w);
            if (wl == nlen && strncmp(w, name, nlen) == 0) {
                bool afterFn = prevWord && prevLen == 2 && strncmp(prevWord, "fn", 2) == 0;
                const char *a = c;
                while (*a == ' ' || *a == '\t') a++;
                if (afterFn) *declared = true;
                else if (*a == '(') *called = true;
            }
            prevWord = w;
            prevLen = wl;
            continue;
        }
        if (!isspace((unsigned char)*c)) prevWord = NULL;
        c++;
    }
}

char *aetherStdlibPrelude(const char *source) {
    if (!source) return NULL;
    char prelude[512];
    size_t used = 0;
    prelude[0] = '\0';
    for (size_t i = 0; i < sizeof(kAetherPreludeFns) / sizeof(kAetherPreludeFns[0]); i++) {
        bool called = false, declared = false;
        aetherScanPreludeUse(source, kAetherPreludeFns[i].name, &called, &declared);
        if (!called || declared) continue;
        int n = snprintf(prelude + used, sizeof(prelude) - used, "%s\n", kAetherPreludeFns[i].decl);
        if (n < 0 || (size_t)n >= sizeof(prelude) - used) break;
        used += (size_t)n;
    }
    return used ? strdup(prelude) : NULL;
}
