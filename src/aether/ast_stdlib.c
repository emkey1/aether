/*
 * ast_stdlib.c -- the modern standard surface in the front end (D55-D61).
 *
 * Two jobs. The Pascal-era builtins the engine still has for its other front
 * ends (copy, pos, val, the File sequence, paramstr, inttostr, the unprefixed
 * mstream* names, ...) and the append verbs D66 renamed are not Aether: a call
 * to one is a SCOPE-001 whose hint names the modern form (class 2,
 * tests/surface/registry.json). And
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
    /* D72: run-together names, renamed to snake_case. */
    { "fileexists", "write `file_exists(...)`: builtin names are snake_case (D72)" },
    { "formatfloat", "write `format_float(...)`: builtin names are snake_case (D72)" },
    { "getcurrentdir", "write `get_current_dir(...)`: builtin names are snake_case (D72)" },
    { "getenv", "write `get_env(...)`: builtin names are snake_case (D72)" },
    { "getenvint", "write `get_env_int(...)`: builtin names are snake_case (D72)" },
    { "realtostr", "write `real_to_text(...)`: builtin names are snake_case (D72)" },
    { "dnslookup", "write `dns_lookup(...)`: builtin names are snake_case (D72)" },
    { "httpsession", "write `http_session(...)`: builtin names are snake_case (D72)" },
    { "httpclose", "write `http_close(...)`: builtin names are snake_case (D72)" },
    { "httpsetheader", "write `http_set_header(...)`: builtin names are snake_case (D72)" },
    { "httprequest", "write `http_request(...)`: builtin names are snake_case (D72)" },
    { "httpawait", "write `http_await(...)`: builtin names are snake_case (D72)" },
    { "httpcancel", "write `http_cancel(...)`: builtin names are snake_case (D72)" },
    { "httpclearheaders", "write `http_clear_headers(...)`: builtin names are snake_case (D72)" },
    { "httperrorcode", "write `http_error_code(...)`: builtin names are snake_case (D72)" },
    { "httpgetasyncprogress", "write `http_get_async_progress(...)`: builtin names are snake_case (D72)" },
    { "httpgetasynctotal", "write `http_get_async_total(...)`: builtin names are snake_case (D72)" },
    { "httpgetheader", "write `http_get_header(...)`: builtin names are snake_case (D72)" },
    { "httpgetlastheaders", "write `http_get_last_headers(...)`: builtin names are snake_case (D72)" },
    { "httpisdone", "write `http_is_done(...)`: builtin names are snake_case (D72)" },
    { "httplasterror", "write `http_last_error(...)`: builtin names are snake_case (D72)" },
    { "httprequestasync", "write `http_request_async(...)`: builtin names are snake_case (D72)" },
    { "httprequestasynctofile", "write `http_request_async_to_file(...)`: builtin names are snake_case (D72)" },
    { "httprequesttofile", "write `http_request_to_file(...)`: builtin names are snake_case (D72)" },
    { "httpsetoption", "write `http_set_option(...)`: builtin names are snake_case (D72)" },
    { "httptryawait", "write `http_try_await(...)`: builtin names are snake_case (D72)" },
    { "socketcreate", "write `socket_create(...)`: builtin names are snake_case (D72)" },
    { "socketconnect", "write `socket_connect(...)`: builtin names are snake_case (D72)" },
    { "socketbind", "write `socket_bind(...)`: builtin names are snake_case (D72)" },
    { "socketbindaddr", "write `socket_bind_addr(...)`: builtin names are snake_case (D72)" },
    { "socketlisten", "write `socket_listen(...)`: builtin names are snake_case (D72)" },
    { "socketaccept", "write `socket_accept(...)`: builtin names are snake_case (D72)" },
    { "socketsend", "write `socket_send(...)`: builtin names are snake_case (D72)" },
    { "socketreceive", "write `socket_receive(...)`: builtin names are snake_case (D72)" },
    { "socketclose", "write `socket_close(...)`: builtin names are snake_case (D72)" },
    { "socketpeeraddr", "write `socket_peer_addr(...)`: builtin names are snake_case (D72)" },
    { "socketlasterror", "write `socket_last_error(...)`: builtin names are snake_case (D72)" },
    { "socketsetblocking", "write `socket_set_blocking(...)`: builtin names are snake_case (D72)" },
    { "socketpoll", "write `socket_poll(...)`: builtin names are snake_case (D72)" },
    { "sqliteopen", "write `sqlite_open(...)`: builtin names are snake_case (D72)" },
    { "sqliteclose", "write `sqlite_close(...)`: builtin names are snake_case (D72)" },
    { "sqliteexec", "write `sqlite_exec(...)`: builtin names are snake_case (D72)" },
    { "sqliteprepare", "write `sqlite_prepare(...)`: builtin names are snake_case (D72)" },
    { "sqlitestep", "write `sqlite_step(...)`: builtin names are snake_case (D72)" },
    { "sqlitefinalize", "write `sqlite_finalize(...)`: builtin names are snake_case (D72)" },
    { "sqlitereset", "write `sqlite_reset(...)`: builtin names are snake_case (D72)" },
    { "sqliteclearbindings", "write `sqlite_clear_bindings(...)`: builtin names are snake_case (D72)" },
    { "sqlitebindint", "write `sqlite_bind_int(...)`: builtin names are snake_case (D72)" },
    { "sqlitebinddouble", "write `sqlite_bind_double(...)`: builtin names are snake_case (D72)" },
    { "sqlitebindtext", "write `sqlite_bind_text(...)`: builtin names are snake_case (D72)" },
    { "sqlitebindnull", "write `sqlite_bind_null(...)`: builtin names are snake_case (D72)" },
    { "sqlitecolumncount", "write `sqlite_column_count(...)`: builtin names are snake_case (D72)" },
    { "sqlitecolumntype", "write `sqlite_column_type(...)`: builtin names are snake_case (D72)" },
    { "sqlitecolumnname", "write `sqlite_column_name(...)`: builtin names are snake_case (D72)" },
    { "sqlitecolumnint", "write `sqlite_column_int(...)`: builtin names are snake_case (D72)" },
    { "sqlitecolumndouble", "write `sqlite_column_double(...)`: builtin names are snake_case (D72)" },
    { "sqlitecolumntext", "write `sqlite_column_text(...)`: builtin names are snake_case (D72)" },
    { "sqlitechanges", "write `sqlite_changes(...)`: builtin names are snake_case (D72)" },
    { "sqlitelastinsertrowid", "write `sqlite_last_insert_rowid(...)`: builtin names are snake_case (D72)" },
    { "sqliteerrmsg", "write `sqlite_errmsg(...)`: builtin names are snake_case (D72)" },
    { "mstreamcreate", "write `mstream_new()`" },
    { "mstreamfromstring", "write `mstream_from_text(t)`" },
    { "mstreambuffer", "write `mstream_text(ms)`" },
    { "mstreamappendbyte", "write `mstream_write_byte(ms, b)`" },
    { "mstreamloadfromfile", "write `let ms = mstream_load(path);` (it returns a new MStream)" },
    { "mstreamsavetofile", "write `mstream_save(ms, path)`" },
    { "mstreamfree", "write `mstream_free(ms)`" },
    /* D66: the 2026-10-17 append verbs, renamed to write (one name per
     * operation, D55). */
    { "mstream_append_text", "write `mstream_write_text(ms, t)` (it adds t's bytes at the end)" },
    { "mstream_append_byte", "write `mstream_write_byte(ms, b)` (it adds one byte at the end)" },
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

bool aetherBuiltinNameRemoved(const char *name) {
    return aetherRemovedBuiltinHint(name) != NULL;
}

/* A call to a removed builtin, by the spelling the user wrote (before any
 * alias). A user's own `fn NAME` is not ours to judge. Reports SCOPE-001 and
 * returns true when the call is rejected. */
bool aetherCheckRemovedBuiltin(AetherParser *p, const char *raw, int line) {
    if (!p || !raw || p->forwardScan || p->hadError) return false;
    const char *hint = aetherRemovedBuiltinHint(raw);
    if (!hint || aetherAstIsTopLevelUserFunction(raw) || aetherIsNestedFnName(p, raw)) return false;
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
    /* D68: `length(m)` on a map. */
    if (strcasecmp(fn, "length") == 0 && call->child_count == 1) {
        char *mt = aetherMapTypeNameOfExpr(p, call->children[0]);
        if (mt) {
            free(mt);
            free(call->token->value);
            call->token->value = strdup("aether_map_length");
            call->token->length = strlen(call->token->value);
            setTypeAST(call, TYPE_INT64);
        }
        return true;
    }
    /* D68: the map functions' result types, for the backend. */
    if (strncmp(fn, "aether_map_", 11) == 0) {
        if (strcmp(fn, "aether_map_has") == 0) setTypeAST(call, TYPE_BOOLEAN);
        else if (strcmp(fn, "aether_map_keys") == 0 || strcmp(fn, "aether_map_values") == 0)
            setTypeAST(call, TYPE_ARRAY);
        else if (strcmp(fn, "aether_map_get_or") == 0 && call->child_count >= 1) {
            char *mt = aetherMapTypeNameOfExpr(p, call->children[0]);
            char *vt = aetherMapValueTypeName(mt);
            setTypeAST(call, aetherVarTypeForTypeName(vt));
            if (call->child_count == 3) {
                AST *d = aetherMapHoistComposite(p, call->children[2], vt, line);
                if (d && d != call->children[2]) {
                    call->children[2] = d;
                    d->parent = call;
                }
            }
            free(mt);
            free(vt);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Maps (D68): `Map[Text, T]`                                          */
/* ------------------------------------------------------------------ */

/* The front end spells a map type "Map[Text, T]" in its binding tables (T is
 * itself a type name, so "Map[Text, Int[]]" and "Map[Text, Map[Text, Int]]"
 * nest). Every map operation lowers to an Aether-private engine builtin
 * (pscal-core ext_builtins/strings/stdlib.c): `m[k]` reads through
 * aether_map_get(m, k, "m"), `m[k] = v` is the statement aether_map_set(m, k,
 * v), and a write into a stored value (`m[k][i] = v`, `m[a][b] = v`,
 * `m[k].f = v`) moves the value into a temp with aether_map_take, writes the
 * temp as any variable is written, and moves it back with aether_map_put. */

bool aetherTypeNameIsMap(const char *name) {
    if (!name) return false;
    size_t n = strlen(name);
    return n > 5 && strncmp(name, "Map[", 4) == 0 && name[n - 1] == ']' &&
           !aetherTypeNameIsArray(name);
}

/* "Map[Text, Int[]]" -> "Int[]" (malloc'd), or NULL. */
char *aetherMapValueTypeName(const char *mapName) {
    if (!aetherTypeNameIsMap(mapName)) return NULL;
    const char *inner = mapName + 4;
    size_t n = strlen(mapName);
    const char *end = mapName + n - 1; /* the closing ']' */
    int depth = 0;
    const char *comma = NULL;
    for (const char *c = inner; c < end; c++) {
        if (*c == '[') depth++;
        else if (*c == ']') depth--;
        else if (*c == ',' && depth == 0) { comma = c; break; }
    }
    if (!comma) return NULL;
    const char *v = comma + 1;
    while (v < end && *v == ' ') v++;
    size_t len = (size_t)(end - v);
    while (len > 0 && v[len - 1] == ' ') len--;
    if (len == 0) return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, v, len);
    out[len] = '\0';
    return out;
}

char *aetherMapTypeNameOf(const char *valueName) {
    if (!valueName) return NULL;
    size_t n = strlen("Map[Text, ") + strlen(valueName) + 2;
    char *out = (char *)malloc(n);
    if (out) snprintf(out, n, "Map[Text, %s]", valueName);
    return out;
}

/* The engine VarType a value of the named Aether type has. */
VarType aetherVarTypeForTypeName(const char *name) {
    if (!name) return TYPE_UNKNOWN;
    if (aetherTypeNameIsArray(name)) return TYPE_ARRAY;
    if (aetherTypeNameIsMap(name)) return TYPE_MAP;
    const char *reaName = NULL;
    VarType vt = TYPE_UNKNOWN;
    if (mapAetherType(name, strlen(name), &reaName, &vt)) return vt;
    AST *resolved = lookupType(name);
    bool isRecord = resolved != NULL;
    releaseTransientTypeNode(resolved);
    return isRecord ? TYPE_POINTER : TYPE_UNKNOWN;
}

/* The type node of a map: AST_TYPE_IDENTIFIER named by the full Aether type
 * name, TYPE_MAP, with the value type's node on `right` (makeValueForType
 * reads the value type from it). Takes ownership of `valueNode`. */
AST *aetherBuildMapTypeNode(const char *mapName, AST *valueNode, int line) {
    Token *tok = newToken(TOKEN_IDENTIFIER, mapName, line, 0);
    AST *node = newASTNode(AST_TYPE_IDENTIFIER, tok);
    freeToken(tok);
    setTypeAST(node, TYPE_MAP);
    if (valueNode) setRight(node, valueNode);
    return node;
}

/* A map's type node from its name ("Map[Text, Int]"), or NULL. */
AST *aetherBuildMapTypeNodeFromName(const char *name, size_t len, int line, VarType *outType) {
    char *full = (char *)malloc(len + 1);
    if (!full) return NULL;
    memcpy(full, name, len);
    full[len] = '\0';
    char *valueName = aetherMapValueTypeName(full);
    if (!valueName) { free(full); return NULL; }
    VarType vvt = TYPE_UNKNOWN;
    AST *valueNode = buildTypeNodeFromName(valueName, strlen(valueName), line, &vvt);
    free(valueName);
    if (!valueNode) { free(full); return NULL; }
    AST *node = aetherBuildMapTypeNode(full, valueNode, line);
    free(full);
    if (outType) *outType = TYPE_MAP;
    return node;
}

/* The value-type code (an engine VarType) a map literal passes to
 * aether_map_of as its first argument. */
void aetherSetMapLiteralCode(AST *lit, VarType vt) {
    if (!lit || lit->child_count < 1 || !lit->children[0]) return;
    AST *code = lit->children[0];
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)vt);
    if (code->token) {
        free(code->token->value);
        code->token->value = strdup(buf);
        code->token->length = strlen(buf);
    }
    code->i_val = (int)vt;
}

bool aetherIsMapLiteral(const AST *n) {
    return n && n->type == AST_PROCEDURE_CALL && !n->left && n->token && n->token->value &&
           strcmp(n->token->value, "aether_map_of") == 0 && n->child_count >= 1;
}

bool aetherIsMapGet(const AST *n) {
    return n && n->type == AST_PROCEDURE_CALL && !n->left && n->token && n->token->value &&
           strcmp(n->token->value, "aether_map_get") == 0 && n->child_count >= 2;
}

static bool aetherIsMapCallNamed(const AST *n, const char *name) {
    return n && n->type == AST_PROCEDURE_CALL && !n->left && n->token && n->token->value &&
           strcmp(n->token->value, name) == 0;
}

/* A composite literal (`[a, b]`, `[]`, `new P { x: 1 }`) is built by a
 * declaration's initializer, not by a builtin's argument: bind it to a temp
 * hoisted ahead of the statement and pass the temp. `typeName` is the type
 * the map stores (NULL: infer it from the literal). */
AST *aetherMapHoistComposite(AetherParser *p, AST *value, const char *typeName, int line) {
    if (!p || !value || p->forwardScan) return value;
    bool composite = (value->type == AST_NEW && value->extra) || value->type == AST_ARRAY_LITERAL;
    if (!composite) return value;
    char *tn = typeName ? strdup(typeName) : inferLetTypeName(p, value);
    if (!tn) return value;
    static int litSerial = 0;
    char name[48];
    snprintf(name, sizeof(name), "__aether_mlit_%d", ++litSerial);
    AST *decl = buildTypedLetDecl(name, tn, value, line);
    if (!decl) { free(tn); return NULL; }
    if (p->bindings) bindingTableSet(p->bindings, name, tn);
    VarType vt = decl->var_type;
    AST *hoist = newASTNode(AST_COMPOUND, NULL);
    hoist->i_val = 1;
    addChild(hoist, decl);
    AST *ref = buildVarRef(name, vt, line);
    aetherEmitLiteralUnalias(p, hoist, ref, decl->left,
                             aetherTypeNameIsArray(tn) ? aetherTypeNameRank(tn) : -1, line);
    free(tn);
    pushPendingObjLit(p, hoist);
    return ref;
}

/* A map read used as a record receiver (`m[k].x`, `m[k].area()`): the
 * backend types a field access only through a variable, so the read is bound
 * to a temp hoisted ahead of the statement (records are pointers, so the temp
 * is the stored record itself). */
AST *aetherMapHoistRead(AetherParser *p, AST *get, int line) {
    if (!p || !aetherIsMapGet(get) || p->forwardScan) return get;
    char *tn = inferLetTypeName(p, get);
    if (!tn) return get;
    static int readSerial = 0;
    char name[48];
    snprintf(name, sizeof(name), "__aether_mr_%d", ++readSerial);
    AST *decl = buildTypedLetDecl(name, tn, get, line);
    if (!decl) { free(tn); return NULL; }
    if (p->bindings) bindingTableSet(p->bindings, name, tn);
    VarType vt = decl->var_type;
    free(tn);
    AST *hoist = newASTNode(AST_COMPOUND, NULL);
    hoist->i_val = 1;
    addChild(hoist, decl);
    pushPendingObjLit(p, hoist);
    return buildVarRef(name, vt, line);
}

/* Every `<map read>.field` in a just-parsed postfix chain that is not a
 * store target. */
AST *aetherMapHoistFieldReceivers(AetherParser *p, AST *node) {
    if (!node) return node;
    if (node->type == AST_FIELD_ACCESS || node->type == AST_ARRAY_ACCESS) {
        AST *l = aetherMapHoistFieldReceivers(p, node->left);
        if (node->type == AST_FIELD_ACCESS && aetherIsMapGet(l)) {
            int line = node->token ? node->token->line : 0;
            l = aetherMapHoistRead(p, l, line);
        }
        if (l != node->left) {
            node->left = NULL;
            setLeft(node, l);
        }
        return node;
    }
    if (aetherIsMapGet(node) && node->child_count >= 1) {
        AST *b = aetherMapHoistFieldReceivers(p, node->children[0]);
        if (b != node->children[0]) {
            node->children[0] = b;
            if (b) b->parent = node;
        }
    }
    return node;
}

bool aetherChainHasMapGet(const AST *lv) {
    while (lv) {
        if (aetherIsMapGet(lv)) return true;
        if (lv->type == AST_ARRAY_ACCESS || lv->type == AST_FIELD_ACCESS) lv = lv->left;
        else return false;
    }
    return false;
}

/* The source spelling of a map expression for the missing-key message:
 * `counts`, `self.counts`, `groups[...]`, else "m". */
static void aetherMapExprText(const AST *e, char *buf, size_t n) {
    if (!buf || n == 0) return;
    buf[0] = '\0';
    if (!e) { snprintf(buf, n, "m"); return; }
    if (e->type == AST_VARIABLE && e->token && e->token->value) {
        const char *v = e->token->value;
        snprintf(buf, n, "%s", (strcasecmp(v, "myself") == 0 || strcasecmp(v, "my") == 0) ? "self" : v);
        return;
    }
    if (e->type == AST_FIELD_ACCESS && e->token && e->token->value) {
        char left[96];
        aetherMapExprText(e->left, left, sizeof(left));
        snprintf(buf, n, "%s.%s", left, e->token->value);
        return;
    }
    if (e->type == AST_ARRAY_ACCESS || aetherIsMapGet(e)) {
        char left[96];
        aetherMapExprText(aetherIsMapGet(e) ? e->children[0] : e->left, left, sizeof(left));
        snprintf(buf, n, "%s[...]", left);
        return;
    }
    snprintf(buf, n, "m");
}

static AST *aetherBuildStringConst(const char *text, int line) {
    Token *tok = newToken(TOKEN_STRING_CONST, text ? text : "", line, 0);
    AST *node = newASTNode(AST_STRING, tok);
    freeToken(tok);
    node->i_val = (int)strlen(text ? text : "");
    setTypeAST(node, inferStringLiteralType(text ? text : "", strlen(text ? text : "")));
    return node;
}

static AST *aetherBuildCall(const char *name, int line) {
    Token *tok = newToken(TOKEN_IDENTIFIER, name, line, 0);
    AST *call = newASTNode(AST_PROCEDURE_CALL, tok);
    freeToken(tok);
    setTypeAST(call, TYPE_UNKNOWN);
    return call;
}

static AST *aetherBuildCallStmt(AST *call) {
    AST *stmt = newASTNode(AST_EXPR_STMT, call->token);
    setLeft(stmt, call);
    return stmt;
}

/* The map type name of `base`, or NULL. */
char *aetherMapTypeNameOfExpr(AetherParser *p, AST *base) {
    char *t = aetherInferSliceBaseTypeName(p, base);
    if (t && aetherTypeNameIsMap(t)) return t;
    free(t);
    return NULL;
}

/* `base[key]` on a map: aether_map_get(base, key, "<base>"). Takes ownership
 * of both. A non-Text key is TYPE-001. */
AST *aetherBuildMapGet(AetherParser *p, AST *base, AST *key, const char *mapType, int line) {
    char text[128];
    aetherMapExprText(base, text, sizeof(text));
    AST *call = aetherBuildCall("aether_map_get", line);
    addChild(call, base);
    addChild(call, key);
    addChild(call, aetherBuildStringConst(text, line));
    char *valueName = aetherMapValueTypeName(mapType);
    setTypeAST(call, aetherVarTypeForTypeName(valueName));
    free(valueName);
    return call;
}

static bool aetherSimpleEqual(const AST *a, const AST *b) {
    if (!a || !b || a->type != b->type || !a->token || !b->token || !a->token->value ||
        !b->token->value)
        return false;
    if (a->type != AST_STRING && a->type != AST_VARIABLE && a->type != AST_NUMBER) return false;
    return strcmp(a->token->value, b->token->value) == 0;
}

/* Two lvalue chains that name the same place: variables, fields, simple
 * indexes and simple map keys. */
static bool aetherChainEqual(const AST *a, const AST *b) {
    if (!a || !b) return false;
    if (aetherIsMapGet(a) || aetherIsMapGet(b)) {
        return aetherIsMapGet(a) && aetherIsMapGet(b) &&
               aetherSimpleEqual(a->children[1], b->children[1]) &&
               aetherChainEqual(a->children[0], b->children[0]);
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case AST_VARIABLE:
            return aetherSimpleEqual(a, b);
        case AST_FIELD_ACCESS:
            return a->token && b->token && a->token->value && b->token->value &&
                   strcmp(a->token->value, b->token->value) == 0 &&
                   aetherChainEqual(a->left, b->left);
        case AST_ARRAY_ACCESS:
            if (a->child_count != b->child_count) return false;
            for (int i = 0; i < a->child_count; i++)
                if (!aetherSimpleEqual(a->children[i], b->children[i])) return false;
            return aetherChainEqual(a->left, b->left);
        default:
            return false;
    }
}

static bool aetherIsSimpleOperand(const AST *e) {
    return e && (e->type == AST_NUMBER || e->type == AST_STRING || e->type == AST_BOOLEAN ||
                 e->type == AST_NIL ||
                 (e->type == AST_VARIABLE && e->token && e->token->value &&
                  strncmp(e->token->value, "__aether_", 9) != 0));
}

/* `let NAME: TYPE = init;` registered in the binding table. */
static AST *aetherMapTempDecl(AetherParser *p, const char *name, const char *typeName, AST *init,
                              int line) {
    AST *decl = buildTypedLetDecl(name, typeName, init, line);
    if (decl && p->bindings) bindingTableSet(p->bindings, name, typeName);
    return decl;
}

/* The deepest (closest to the root) map read in an lvalue chain, and the
 * slot that holds it. */
static AST *aetherDeepestMapGet(AST **slot, AST ***outSlot) {
    AST *found = NULL;
    AST **foundSlot = NULL;
    while (slot && *slot) {
        AST *cur = *slot;
        if (aetherIsMapGet(cur)) {
            found = cur;
            foundSlot = slot;
            slot = &cur->children[0];
        } else if (cur->type == AST_ARRAY_ACCESS || cur->type == AST_FIELD_ACCESS) {
            slot = &cur->left;
        } else {
            break;
        }
    }
    *outSlot = foundSlot;
    return found;
}

static void aetherReplaceSlot(AST **slot, AST *parent, AST *with) {
    *slot = with;
    if (with) with->parent = parent;
}

/* Bracket `inner` (a statement over `__aether_mt_N`, built by buildInner from
 * the chain with the map read replaced by the temp) with take/put on the map
 * read `g`. Takes ownership of `g`. */
typedef AST *(*AetherMapInnerFn)(AetherParser *p, AST *tempRef, void *ctx);

static AST *aetherMapTakePut(AetherParser *p, AST *g, AetherMapInnerFn buildInner, void *ctx,
                             AST *outer, int line) {
    AST *base = g->children[0];
    AST *key = g->children[1];
    AST *nameLit = g->child_count >= 3 ? g->children[2] : NULL;
    if (!aetherIsLValueChain(base)) {
        reportAetherAstErrorWithCode(line, "TYPE-001",
                                     "only a map held in a variable, field or element can be "
                                     "written; this map is the result of an expression.",
                                     "bind it first: `let m2 = <expr>; m2[k] = v;`.");
        p->hadError = true;
        return NULL;
    }
    char *mapType = aetherMapTypeNameOfExpr(p, base);
    char *valueType = aetherMapValueTypeName(mapType);
    free(mapType);
    if (!valueType) {
        reportAetherAstErrorWithCode(line, "TYPE-001",
                                     "cannot name the type of this map to write into it.",
                                     "declare the map with its type: `let m: Map[Text, T] = {};`.");
        p->hadError = true;
        return NULL;
    }
    static int takeSerial = 0;
    int serial = ++takeSerial;
    /* A computed key is evaluated once, before the take. */
    if (!aetherIsSimpleOperand(key)) {
        char kname[48];
        snprintf(kname, sizeof(kname), "__aether_mk_%d", serial);
        g->children[1] = NULL;
        addChild(outer, aetherMapTempDecl(p, kname, "Text", key, line));
        key = buildVarRef(kname, TYPE_UNICODE_STRING, line);
        g->children[1] = key;
        key->parent = g;
    }
    char tname[48];
    snprintf(tname, sizeof(tname), "__aether_mt_%d", serial);
    AST *tdecl = aetherMapTempDecl(p, tname, valueType, NULL, line);
    VarType tvt = aetherVarTypeForTypeName(valueType);
    free(valueType);
    if (!tdecl) return NULL;
    addChild(outer, tdecl);

    AST *take = aetherBuildCall("aether_map_take", line);
    addChild(take, copyAST(base));
    addChild(take, copyAST(key));
    addChild(take, buildVarRef(tname, tvt, line));
    if (nameLit) addChild(take, copyAST(nameLit));
    setTypeAST(take, TYPE_VOID);
    addChild(outer, aetherBuildCallStmt(take));

    AST *inner = buildInner(p, buildVarRef(tname, tvt, line), ctx);
    if (!inner) return NULL;
    addChild(outer, inner);

    AST *put = aetherBuildCall("aether_map_put", line);
    addChild(put, copyAST(base));
    addChild(put, copyAST(key));
    addChild(put, buildVarRef(tname, tvt, line));
    setTypeAST(put, TYPE_VOID);
    addChild(outer, aetherBuildCallStmt(put));
    freeAST(g);
    return outer;
}

typedef struct {
    AST *assign;      /* the assignment, its map read replaced by the temp */
    AST **gSlot;      /* where the map read sat in assign's tree */
    AST *gParent;
    AST **lmSlot;     /* `m[k] = m[k] + ...`: the `+` chain's left end */
    AST *lmParent;
} AetherMapAssignCtx;

static AST *aetherMapInnerAssign(AetherParser *p, AST *tempRef, void *vctx) {
    AetherMapAssignCtx *c = (AetherMapAssignCtx *)vctx;
    aetherReplaceSlot(c->gSlot, c->gParent, tempRef);
    /* `X = X + ...`: the `+` chain starts from the rewritten target. */
    if (c->lmSlot) aetherReplaceSlot(c->lmSlot, c->lmParent, copyAST(c->assign->left));
    return aetherLowerAssignStatement(p, c->assign);
}

/* `m[k] = v`, `m[k] += v`, `m[k][i] = v`, `m[a][b] = v`, `m[k].f = v`. */
AST *aetherLowerMapStore(AetherParser *p, AST *assign) {
    int line = assign->token ? assign->token->line : 0;
    TokenType op = assign->token ? assign->token->type : TOKEN_ASSIGN;
    /* A compound store into the stored value (`m[k].f += 1`, `m[k][i] += 1`)
     * stays compound: the write after the take is an ordinary one. */
    if (op != TOKEN_ASSIGN && !aetherIsMapGet(assign->left)) op = TOKEN_ASSIGN;
    if (op != TOKEN_ASSIGN) {
        /* `x OP= v` -> `x = x OP v`. */
        Token *opTok = newToken(op, assign->token->value ? assign->token->value : "+", line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, opTok);
        freeToken(opTok);
        setLeft(bin, copyAST(assign->left));
        setRight(bin, assign->right);
        setTypeAST(bin, assign->left->var_type);
        assign->right = NULL;
        setRight(assign, bin);
        assign->token->type = TOKEN_ASSIGN;
        free(assign->token->value);
        assign->token->value = strdup("=");
    }
    AST **gSlot = NULL;
    AST *g = aetherDeepestMapGet(&assign->left, &gSlot);
    if (!g) return assign;
    AST *gParent = (gSlot == &assign->left) ? assign : g->parent;
    AST *rhs = assign->right;

    AetherMapAssignCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.assign = assign;
    ctx.gSlot = gSlot;
    ctx.gParent = gParent;

    if (rhs && rhs->type == AST_BINARY_OP && rhs->token && rhs->token->type == TOKEN_PLUS) {
        AST *lm = rhs;
        while (lm->left && lm->left->type == AST_BINARY_OP && lm->left->token &&
               lm->left->token->type == TOKEN_PLUS)
            lm = lm->left;
        char *tt = (g == assign->left) ? NULL : inferLetTypeName(p, assign->left);
        bool arrayTarget = (g == assign->left) ? g->var_type == TYPE_ARRAY
                                               : aetherTypeNameIsArray(tt);
        free(tt);
        if (arrayTarget && aetherChainEqual(lm->left, assign->left)) {
            /* `m[k] = m[k] + [v]`: append to the taken value in place. */
            AST *dup = lm->left;
            lm->left = NULL;
            freeAST(dup);
            ctx.lmSlot = &lm->left;
            ctx.lmParent = lm;
        }
    }
    if (g == assign->left && !ctx.lmSlot) {
        /* A plain store: aether_map_set(base, key, value). */
        AST *base = g->children[0];
        if (!aetherIsLValueChain(base)) {
            reportAetherAstErrorWithCode(line, "TYPE-001",
                                         "only a map held in a variable, field or element can "
                                         "be written; this map is the result of an expression.",
                                         "bind it first: `let m2 = <expr>; m2[k] = v;`.");
            p->hadError = true;
            freeAST(assign);
            return NULL;
        }
        AST *set = aetherBuildCall("aether_map_set", line);
        g->children[0] = NULL;
        addChild(set, base);
        addChild(set, g->children[1]);
        g->children[1] = NULL;
        assign->right = NULL;
        {
            char *mt = aetherMapTypeNameOfExpr(p, base);
            char *vt = aetherMapValueTypeName(mt);
            addChild(set, aetherMapHoistComposite(p, rhs, vt, line));
            free(mt);
            free(vt);
        }
        setTypeAST(set, TYPE_VOID);
        freeAST(assign); /* frees g too */
        return aetherBuildCallStmt(set);
    }
    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1; /* splice into the surrounding block */
    /* A write into the stored value. A computed right side (it may read the
     * map) is evaluated before the take. */
    if (!ctx.lmSlot && rhs && !aetherIsSimpleOperand(rhs)) {
        char *rtype = inferLetTypeName(p, rhs);
        if (!rtype) rtype = inferLetTypeName(p, assign->left);
        if (rtype) {
            static int valSerial = 0;
            char vname[48];
            snprintf(vname, sizeof(vname), "__aether_mv_%d", ++valSerial);
            assign->right = NULL;
            AST *vdecl = aetherMapTempDecl(p, vname, rtype, rhs, line);
            VarType vvt = aetherVarTypeForTypeName(rtype);
            free(rtype);
            if (!vdecl) { freeAST(outer); return NULL; }
            addChild(outer, vdecl);
            setRight(assign, buildVarRef(vname, vvt, line));
        }
    }
    *gSlot = NULL;
    AST *r = aetherMapTakePut(p, g, aetherMapInnerAssign, &ctx, outer, line);
    if (!r) { freeAST(outer); return NULL; }
    return r;
}

typedef struct {
    AST *call;   /* the mutating call, its map read replaced by the temp */
    AST **gSlot;
    AST *gParent;
} AetherMapCallCtx;

static AST *aetherMapInnerCall(AetherParser *p, AST *tempRef, void *vctx) {
    (void)p;
    AetherMapCallCtx *c = (AetherMapCallCtx *)vctx;
    aetherReplaceSlot(c->gSlot, c->gParent, tempRef);
    return aetherLowerMapMutatorStmt(p, c->call);
}

/* A statement call that writes its first argument (`remove(m, k)`) where the
 * argument is reached through a map read (`remove(m["a"], k)`). */
AST *aetherLowerMapMutatorStmt(AetherParser *p, AST *call) {
    if (!call || call->child_count < 1) return call;
    AST **gSlot = NULL;
    AST *g = aetherDeepestMapGet(&call->children[0], &gSlot);
    if (!g) {
        AST *stmt = newASTNode(AST_EXPR_STMT, call->token);
        setLeft(stmt, call);
        return stmt;
    }
    int line = call->token ? call->token->line : 0;
    AetherMapCallCtx ctx;
    ctx.call = call;
    ctx.gSlot = gSlot;
    ctx.gParent = (gSlot == &call->children[0]) ? call : g->parent;
    *gSlot = NULL;
    AST *outer = newASTNode(AST_COMPOUND, NULL);
    outer->i_val = 1;
    AST *r = aetherMapTakePut(p, g, aetherMapInnerCall, &ctx, outer, line);
    if (!r) { freeAST(outer); return NULL; }
    return r;
}

bool aetherIsMapMutatorCall(const AST *e) {
    return aetherIsMapCallNamed(e, "aether_map_remove") && e->child_count >= 1 &&
           aetherChainHasMapGet(e->children[0]);
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
    /* Checked I/O (D73): effectful, so a call needs `fx` like read_file
     * (aetherStdlibPreludeIsEffectful). One read; the ok flag is per thread. */
    { "try_read_file",
      "fn try_read_file(path: Text) -> (Text, Bool) { let t: Text = \"\"; let ok: Bool = false; "
      "fx { t = aether_try_read_file_text(path); ok = aether_last_io_ok(); } ret (t, ok); }" },
    { "try_read_lines",
      "fn try_read_lines(path: Text) -> (Text[], Bool) { let t: Text[] = []; let ok: Bool = false; "
      "fx { t = aether_try_read_lines_list(path); ok = aether_last_io_ok(); } ret (t, ok); }" },
    { "try_toon_parse_file",
      "fn try_toon_parse_file(path: Text) -> (ToonDoc, Bool) { let d: ToonDoc; let ok: Bool = false; "
      "fx { d = aether_try_toon_read_file(path); ok = aether_last_io_ok(); } ret (d, ok); }" },
    /* Checked SQL (1.0-beta4): sqlite_exec / sqlite_prepare stop the program
     * on a SQL error; these report it instead, and sqlite_errmsg(db) then
     * reads SQLite's message. Effectful, like the sqlite_* builtins. */
    { "try_sqlite_exec",
      "fn try_sqlite_exec(db: Int, sql: Text) -> (Int, Bool) { let rc: Int = 0; "
      "fx { rc = aether_try_sqlite_exec(db, sql); } ret (rc, rc == 0); }" },
    { "try_sqlite_prepare",
      "fn try_sqlite_prepare(db: Int, sql: Text) -> (Int, Bool) { let st: Int = -1; "
      "fx { st = aether_try_sqlite_prepare(db, sql); } ret (st, st >= 0); }" },
};

/* The prelude fns injected into this compile that perform effects (D73). */
static const char *g_preludeEffectful[8];
static int g_preludeEffectfulCount = 0;

bool aetherStdlibPreludeIsEffectful(const char *name) {
    if (!name) return false;
    for (int i = 0; i < g_preludeEffectfulCount; i++)
        if (strcmp(g_preludeEffectful[i], name) == 0) return true;
    return false;
}

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
    g_preludeEffectfulCount = 0;
    if (!source) return NULL;
    char prelude[2048];
    size_t used = 0;
    prelude[0] = '\0';
    for (size_t i = 0; i < sizeof(kAetherPreludeFns) / sizeof(kAetherPreludeFns[0]); i++) {
        bool called = false, declared = false;
        aetherScanPreludeUse(source, kAetherPreludeFns[i].name, &called, &declared);
        if (!called || declared) continue;
        /* A build without SQLite has no engine body for the checked SQL
         * fns: leave them undeclared, so the call is SCOPE-001 with the
         * "not built into this compiler" hint (aetherNameHintFor). */
        if (strncmp(kAetherPreludeFns[i].name, "try_sqlite_", 11) == 0 &&
            getVmBuiltinID("aether_try_sqlite_exec") < 0)
            continue;
        int n = snprintf(prelude + used, sizeof(prelude) - used, "%s\n", kAetherPreludeFns[i].decl);
        if (n < 0 || (size_t)n >= sizeof(prelude) - used) break;
        used += (size_t)n;
        if (strncmp(kAetherPreludeFns[i].name, "try_read_", 9) == 0 ||
            strncmp(kAetherPreludeFns[i].name, "try_sqlite_", 11) == 0 ||
            strcmp(kAetherPreludeFns[i].name, "try_toon_parse_file") == 0) {
            if (g_preludeEffectfulCount < 8)
                g_preludeEffectful[g_preludeEffectfulCount++] = kAetherPreludeFns[i].name;
        }
    }
    return used ? strdup(prelude) : NULL;
}
