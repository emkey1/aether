#include "aether/diagnostics.h"
#include "aether/ast_prepasses.h"
#include "aether/parser.h"
#include "aether/semantic.h"
#include "backend_ast/builtin.h"
#include "vm/vm_fx_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *aetherInferDiagnosticCode(const char *kind, const char *detail) {
    if (kind) {
        if (strcmp(kind, "parser") == 0) {
            return "SYN-001";
        }
        if (strcmp(kind, "compatibility") == 0 && detail) {
            if (strstr(detail, "let mut") || strstr(detail, "`mut` is ignored")) {
                return "MUT-001";
            }
            if (strstr(detail, "missing import") || strstr(detail, "module")) {
                return "IMP-001";
            }
        }
        if (strcmp(kind, "effect") == 0) {
            return "FX-001";
        }
        if (strcmp(kind, "purity") == 0 || strcmp(kind, "contract") == 0) {
            return "ANN-001";
        }
        if (strcmp(kind, "import") == 0) {
            return "IMP-001";
        }
        /* A malformed `toon:` block marker (ast_prepasses.c): a syntax
         * error with its own "TOON" label. */
        if (strcmp(kind, "TOON") == 0) {
            return "SYN-001";
        }
        if (strcmp(kind, "scope") == 0) {
            return "SCOPE-001";
        }
        /* A print placeholder (`println("{} items", n)`, `"%.2f"`): W6-04, D48. */
        if (strcmp(kind, "format") == 0) {
            return "FMT-001";
        }
        /* `//` after an expression, a comment that reads as floor division:
         * W8-14, D45. */
        if (strcmp(kind, "slash-comment") == 0) {
            return "DIV-002";
        }
        /* Wrong argument count for a real builtin. Distinct from BUILT-001,
         * which is "this helper does not exist at all" -- here the name is
         * right and only the call shape is wrong, so the guide fix is the
         * signature, not the helper surface. semantic.c emits this explicitly
         * via reportAetherErrorCoded; this entry is the kind-based backstop. */
        if (strcmp(kind, "builtin") == 0) {
            return "BUILT-002";
        }
        /* Implicit Real -> Int truncation. A warning, not an error: `int(x)` is
         * the explicit spelling and is never flagged. semantic.c emits this
         * explicitly via reportAetherWarningCoded; kind-based backstop only. */
        if (strcmp(kind, "narrowing") == 0) {
            return "NARROW-001";
        }
        /* A scalar parameter its caller expects to move (D77): a warning,
         * emitted explicitly by experiment.c; kind-based backstop only. */
        if (strcmp(kind, "parameter") == 0) {
            return "PARAM-001";
        }
        if (strcmp(kind, "redeclaration") == 0) {
            return "NAME-001";
        }
        /* Tuple feature-limitation diagnostics (narrow tuple returns): a
         * tuple-return call bound to one name, destructuring a non-tuple/undefined
         * callee, an arity mismatch, or a tuple-return method. These all point at
         * the same TUP-001 guide section. Kept as a distinct "tuple" kind (not the
         * former placeholder "feature") so the code->guide map resolves and the
         * human message reads "Aether tuple parser error". Internal "tuple ...
         * rewrite failed" defects and @post-slot errors use other kinds, so they
         * are intentionally not captured here. */
        if (strcmp(kind, "tuple") == 0) {
            return "TUP-001";
        }
        /* Constant record/type field default-value boundary: a declared field
         * default that is not a compile-time constant (references another field,
         * `self`, or calls a function). Points at the FIELD-003 guide section,
         * which documents `field: Type = <const>` and directs computed values to
         * construction-time `new T { field: value }`. */
        if (strcmp(kind, "field-default") == 0) {
            return "FIELD-003";
        }
        /* Array-parameter mutation with no observable effect on the caller:
         * arrays are value-copied at the call boundary (unlike records,
         * which are pointer-backed), so an indexed write into an array
         * parameter inside a Void function never propagates back. Points
         * at the ARR-001 guide section explaining the value/reference
         * asymmetry and the mutate-and-return idiom. */
        if (strcmp(kind, "array-mutation") == 0) {
            return "ARR-001";
        }
        /* The slice sugar `v[a..b]` on a value that is not an array or a Text,
         * or whose type cannot be named where the slice is written. A
         * wrong-type-for-the-operation error. */
        if (strcmp(kind, "slice") == 0) {
            return "TYPE-001";
        }
        /* A one-dimensional array indexed twice (`let dp: Int[]; dp[i][j]`).
         * Distinct from TYPE-001 because the fix is specific and mechanical --
         * change the declaration's rank, or flatten to one index -- and worth
         * its own guide section rather than being folded into the general
         * annotate-your-types advice. */
        if (strcmp(kind, "array-rank") == 0) {
            return "ARR-002";
        }
        /* Bitwise operator whose right operand is a comparison, e.g.
         * `flags & mask != 0`. `& | ^` bind looser than the comparisons, so
         * this groups as `flags & (mask != 0)`, an Int combined with a Bool
         * (an error since D88; a warning before). Also chained comparisons
         * and a bare `not` before one (D32). */
        if (strcmp(kind, "precedence") == 0) {
            return "PREC-001";
        }
        /* A type name in a declaration that never resolved -- neither a builtin
         * type nor a `type` declared in this program or an imported module.
         * Distinct from TYPE-001, which is a real type used wrongly: here the
         * name itself does not exist, so the fix is the spelling and not the
         * usage. It earns its own code because without it the mistake is
         * completely silent -- an unresolved annotation lowers to an untyped
         * slot that accepts any value, so `-> Strng` type-checks nothing and
         * the program runs to completion. */
        if (strcmp(kind, "unknown-type") == 0) {
            return "TYPE-002";
        }
        /* A `fn` declared inside a function body (or any block). Aether has
         * no nested functions or closures (D49, D50); FUNC-001 is the rule
         * that functions are top-level names, not values. */
        if (strcmp(kind, "nested-fn") == 0) {
            return "FUNC-001";
        }
        /* Retired: under D62 a nested function assigning a local or
         * parameter of its enclosing function was FUNC-002. D64 (language
         * 2026-10-18-1) makes that legal, so nothing emits this kind; the
         * code stays reserved so archived diagnostics keep their meaning. */
        if (strcmp(kind, "nested-assign") == 0) {
            return "FUNC-002";
        }
        /* The condition of `all`/`any` (D58) is not a Bool. */
        if (strcmp(kind, "quantifier") == 0) {
            return "TYPE-001";
        }
    }

    if (!detail) {
        return NULL;
    }

    if (strstr(detail, "Unexpected token")) {
        return "SYN-001";
    }
    if (strstr(detail, "explicit return type")) {
        return "SYN-001";
    }
    if (strstr(detail, "type fields must end with ';', not ','")) {
        return "SYN-001";
    }
    if (strstr(detail, "cannot be used as a")) {
        /* reserved word / operator word used as a field or method name */
        return "SYN-001";
    }
    if (strstr(detail, "cannot infer the type of")) {
        return "TYPE-001";
    }
    /* A constant record-field default whose value type does not match the
     * declared field type (e.g. `value: Int = "x"`). A plain type error -- the
     * fix is to match the type or move the value to construction -- so it points
     * at the TYPE-001 guide section, not the FIELD-003 constant-boundary one. */
    if (strstr(detail, "field default value type mismatch")) {
        return "TYPE-001";
    }
    if (strstr(detail, "@pre must annotate the next function declaration") ||
        strstr(detail, "@post must annotate the next function declaration") ||
        strstr(detail, "cannot call effectful builtin") ||
        strstr(detail, "cannot call non-pure function")) {
        return "ANN-001";
    }
    if (strstr(detail, "tuple return types are not supported yet")) {
        return "TUP-001";
    }
    /* TOON handle-kind / helper-argument misuse. The argument-position patterns
     * are anchored on the toon_ helper name ("call to 'toon_...") so an
     * unrelated message that merely mentions "first argument" is not swallowed
     * into TOON-001. The cross-assign / must-use-ToonX backstops mirror the
     * semantic.c sites that now emit [TOON-001] explicitly. */
    if (strstr(detail, "opaque TOON handle") ||
        strstr(detail, "expects a ToonDoc handle") ||
        strstr(detail, "expects a ToonNode handle") ||
        strstr(detail, "cannot assign ToonDoc handle") ||
        strstr(detail, "cannot assign ToonNode handle") ||
        strstr(detail, "must use ToonDoc when initialized") ||
        strstr(detail, "must use ToonNode when initialized") ||
        (strstr(detail, "call to 'toon_") &&
         (strstr(detail, " first argument") ||
          strstr(detail, " second argument") ||
          strstr(detail, " third argument")))) {
        return "TOON-001";
    }
    /* MStream handle misuse: mirrors the semantic.c sites that emit [MS-001]
     * explicitly. Checked before the generic scalar "must use ... when
     * initialized from" backstop so stream-handle mismatches resolve here. */
    if (strstr(detail, "opaque MStream handle") ||
        strstr(detail, "expects a MStream handle") ||
        strstr(detail, "cannot assign MStream handle") ||
        strstr(detail, "must use MStream when initialized")) {
        return "MS-001";
    }
    /* Scalar binding/assignment mismatches from the semantic pass (which now
     * emits [TYPE-001] at the site; these are backstops for any uncoded copy of
     * the same wording). Checked after the ToonDoc/ToonNode block so handle
     * mismatches keep resolving to TOON-001. */
    if ((strstr(detail, "cannot assign") && strstr(detail, " binding")) ||
        (strstr(detail, "must use") && strstr(detail, "when initialized from"))) {
        return "TYPE-001";
    }
    /* An inferred `let`/`const` with neither a type annotation nor an
     * initializer to infer from (ast_parser.c parseLet). Same fix class as
     * "cannot infer the type of": annotate the type. */
    if (strstr(detail, "requires a type or an initializer")) {
        return "TYPE-001";
    }
    if (strstr(detail, "requires an fx block")) {
        return "FX-001";
    }
    if (strstr(detail, "unable to open module") ||
        strstr(detail, "a module cannot `use` a module that uses it")) {
        return "IMP-001";
    }
    if (strstr(detail, "not in scope")) {
        return "SCOPE-001";
    }
    if (strstr(detail, "Unknown field")) {
        return "FIELD-002";
    }
    if (strstr(detail, "fallthrough path with no return value")) {
        return "FLOW-001";
    }
    /* Empty `ret;` in a non-Void function. Distinct from FLOW-001 (fallthrough):
     * here a return statement exists but supplies no value, so the fix differs
     * ("give the return a value" vs "add a return"). */
    if (strstr(detail, "return requires a value")) {
        return "FLOW-002";
    }
    /* Non-call statement inside a `par { ... }` block. Distinct from PAR-001
     * (the shared-record data race, emitted with a hardcoded [PAR-001]); this is
     * the par-arity rule (only direct call statements are allowed). */
    if (strstr(detail, "only direct call statements")) {
        return "PAR-002";
    }
    if (strstr(detail, "let mut") || strstr(detail, "`mut` is ignored")) {
        return "MUT-001";
    }

    return NULL;
}

// Emits a one-line pointer back into the language guide for a diagnostic code,
// so the compiler↔guide self-correction loop does not depend on the model
// already knowing that, e.g., FX-001 maps to the "Effects (FX-001)" section.
// No-op when code is NULL (an uncoded diagnostic). Deliberately guide-agnostic
// (no filename): the caller may have either guide in context, both, or
// neither, and all three guides key their sections on the same diagnostic code.
void aetherReportGuideHelp(const char *code) {
    if (!code) {
        return;
    }
    fprintf(stderr, "help: see %s in the Aether guide\n", code);
}

/* Repair hints for names models invent for the TOON and MStream helpers. The
 * engine prints one under "[SCOPE-001] identifier 'x' not in scope." (rea's
 * unknownIdentifierHint hook); without it the repair pass has only the name
 * and guesses again. The entries are the wrong guesses the Qwen3.8 fine-tune
 * pilot and the boards made (the full guide's "Structured data: TOON"). */
typedef struct {
    const char *wrong;
    const char *hint;
} AetherNameHint;

#define AETHER_TOON_VALUE_HINT(kind) \
    "did you mean 'toon_" kind "_value(node)'? A node's own value takes the " \
    "'_value' suffix; a field of an object node is 'toon_get_" kind "(node, key)'."

static const AetherNameHint kAetherNameHints[] = {
    { "toon_get_root", "did you mean 'toon_root(doc)'? It returns the document's root ToonNode." },
    { "toon_root_node", "did you mean 'toon_root(doc)'? It returns the document's root ToonNode." },
    { "toon_get_key", "did you mean 'toon_key(node, key)'? It returns the field's ToonNode." },
    { "toon_get_field", "did you mean 'toon_key(node, key)'? It returns the field's ToonNode." },
    { "toon_get_node", "did you mean 'toon_key(node, key)'? It returns the field's ToonNode." },
    { "toon_get_at", "did you mean 'toon_at(node, i)'? It returns the array element's ToonNode." },
    { "toon_get_index", "did you mean 'toon_at(node, i)'? It returns the array element's ToonNode." },
    { "toon_get_item", "did you mean 'toon_at(node, i)'? It returns the array element's ToonNode." },
    { "toon_index", "did you mean 'toon_at(node, i)'? It returns the array element's ToonNode." },
    { "toon_get_str", "did you mean 'toon_get_text(node, key)'? TOON strings are Text." },
    { "toon_get_string", "did you mean 'toon_get_text(node, key)'? TOON strings are Text." },
    { "toon_get_float", "did you mean 'toon_get_real(node, key)'?" },
    { "toon_get_double", "did you mean 'toon_get_real(node, key)'?" },
    { "toon_get_number", "did you mean 'toon_get_real(node, key)'?" },
    { "toon_get_boolean", "did you mean 'toon_get_bool(node, key)'?" },
    { "toon_int", AETHER_TOON_VALUE_HINT("int") },
    { "toon_text", AETHER_TOON_VALUE_HINT("text") },
    { "toon_str", AETHER_TOON_VALUE_HINT("text") },
    { "toon_string", AETHER_TOON_VALUE_HINT("text") },
    { "toon_bool", AETHER_TOON_VALUE_HINT("bool") },
    { "toon_real", AETHER_TOON_VALUE_HINT("real") },
    { "toon_float", AETHER_TOON_VALUE_HINT("real") },
    { "toon_length", "did you mean 'toon_len(node)'?" },
    { "toon_size", "did you mean 'toon_len(node)'?" },
    { "toon_count", "did you mean 'toon_len(node)'?" },
    { "toon_free", "did you mean 'toon_close(doc)'?" },
    { "has_toon_parser", "did you mean 'has_toon()'?" },
    { "yyjson_parse_file", "did you mean 'toon_parse_file(path)'? TOON helpers read JSON; it returns a ToonDoc." },
    { "yyjson_read_file", "did you mean 'toon_parse_file(path)'? TOON helpers read JSON; it returns a ToonDoc." },
    { "json_parse_file", "did you mean 'toon_parse_file(path)'? TOON helpers read JSON; it returns a ToonDoc." },
    { "yyjson_parse", "did you mean 'toon_parse(text)'? TOON helpers read JSON; it returns a ToonDoc." },
    { "yyjson_read", "did you mean 'toon_parse(text)'? TOON helpers read JSON; it returns a ToonDoc." },
    /* MStream: invented spellings of the snake_case family (D61, D66). */
    { "mstream_create", "did you mean 'mstream_new()'?" },
    { "mstreamnew", "did you mean 'mstream_new()'?" },
    { "mstream_from_string", "did you mean 'mstream_from_text(text)'?" },
    { "mstream_buffer", "did you mean 'mstream_text(ms)'? It returns the stream's bytes as Text." },
    { "mstream_to_text", "did you mean 'mstream_text(ms)'?" },
    { "mstream_append", "did you mean 'mstream_write_text(ms, text)' or 'mstream_write_byte(ms, b)'?" },
    { "mstream_write", "did you mean 'mstream_write_text(ms, text)' or 'mstream_write_byte(ms, b)'?" },
    { "mstream_append_bytes", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstream_write_bytes", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstream_append_string", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstream_write_string", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstream_write_str", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstream_append_str", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstream_append_char", "did you mean 'mstream_write_byte(ms, ord(ch))'?" },
    { "mstream_write_char", "did you mean 'mstream_write_byte(ms, ord(ch))'?" },
    { "mstream_put", "did you mean 'mstream_write_text(ms, text)' or 'mstream_write_byte(ms, b)'?" },
    { "mstreamappendbytes", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstreamappendtext", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstreamwritetext", "did you mean 'mstream_write_text(ms, text)'?" },
    { "mstreamwritebyte", "did you mean 'mstream_write_byte(ms, b)'?" },
    { "mstreamwrite", "did you mean 'mstream_write_text(ms, text)' or 'mstream_write_byte(ms, b)'?" },
    { "mstreamlength", "did you mean 'mstream_length(ms)'?" },
    { "mstream_size", "did you mean 'mstream_length(ms)'?" },
    { "mstream_len", "did you mean 'mstream_length(ms)'?" },
    { "mstream_load_from_file", "did you mean 'mstream_load(path)'? It returns a new MStream." },
    { "mstream_load_file", "did you mean 'mstream_load(path)'? It returns a new MStream." },
    { "mstreamloadfile", "did you mean 'mstream_load(path)'? It returns a new MStream." },
    { "mstream_save_to_file", "did you mean 'mstream_save(ms, path)'?" },
    { "mstream_close", "did you mean 'mstream_free(ms)'?" },
    /* Files, stdin, arguments and the clock (D60). */
    { "create_file", "did you mean 'write_file(path, text)'?" },
    { "write_text", "did you mean 'write_file(path, text)'?" },
    { "read_text", "did you mean 'read_file(path)'?" },
    { "file_read", "did you mean 'read_file(path)'?" },
    { "file_write", "did you mean 'write_file(path, text)'?" },
    { "remove_file", "did you mean 'delete_file(path)'?" },
    { "exists", "did you mean 'file_exists(path)'?" },
    { "path_exists", "did you mean 'file_exists(path)'?" },
    { "input", "did you mean 'read_line()'? It returns the next stdin line, \"\" at the end." },
    { "readline", "did you mean 'read_line()'?" },
    { "read_input", "did you mean 'read_line()' or 'read_all_stdin()'?" },
    { "read_stdin", "did you mean 'read_all_stdin()'?" },
    { "argv", "did you mean 'args()'? It is a Text[] of the arguments after the program." },
    { "get_args", "did you mean 'args()'?" },
    { "now", "did you mean 'now_text()' or 'clock_ms()'?" },
    { "time_ms", "did you mean 'clock_ms()'?" },
    { "current_time_ms", "did you mean 'clock_ms()'?" },
    /* Quantifiers are expressions, not functions (D58). */
    { "all", "write the quantifier `all x in xs: cond` (or `all i in 0..n: cond`); it is a Bool expression, in a contract too." },
    { "any", "write the quantifier `any x in xs: cond` (or `any i in 0..n: cond`); it is a Bool expression, in a contract too." },
    /* Text and arrays (D56, D57). */
    { "strlen", "did you mean 'len(s)'? It counts the characters of a Text, the elements of an "
                "array or the keys of a map." },
    { "string_length", "did you mean 'len(s)'? It counts the characters of a Text." },
    { "to_upper", "did you mean 'upper(s)'?" },
    { "to_lower", "did you mean 'lower(s)'?" },
    { "uppercase", "did you mean 'upper(s)'?" },
    { "lowercase", "did you mean 'lower(s)'?" },
    { "tolower", "did you mean 'lower(s)'?" },
    { "startswith", "did you mean 'starts_with(s, p)'?" },
    { "endswith", "did you mean 'ends_with(s, p)'?" },
    { "indexof", "did you mean 'index_of(s, x)'? -1 when absent." },
    { "find", "did you mean 'index_of(s, x)'? -1 when absent." },
    { "substring", "take a slice: `s[a..b]` (from a, up to but not including b)." },
    { "substr", "take a slice: `s[a..b]` (from a, up to but not including b)." },
    { "slice", "take a slice: `s[a..b]` (from a, up to but not including b)." },
    { "sort", "did you mean 'sorted(xs)'? It returns a new array." },
    { "reverse", "did you mean 'reversed(xs)'? It returns a new array." },
    { "minimum", "did you mean 'min_of(xs)'?" },
    { "maximum", "did you mean 'max_of(xs)'?" },
    { "parse_int_checked", "did you mean 'try_parse_int(t)'? It returns (value, ok)." },
    { "try_parse_float", "did you mean 'try_parse_real(t)'? It returns (value, ok)." },
    { "round_half_up", "did you mean 'round_to(x, digits)'? It rounds half away from zero." },
    /* Character classes (D67): C/Python spellings without the underscore. */
    { "isdigit", "did you mean 'is_digit(t)'?" },
    { "isalpha", "did you mean 'is_alpha(t)'?" },
    { "isalnum", "did you mean 'is_alnum(t)'?" },
    { "isspace", "did you mean 'is_space(t)'?" },
    { "isupper", "did you mean 'is_upper(t)'?" },
    { "islower", "did you mean 'is_lower(t)'?" },
    { "is_letter", "did you mean 'is_alpha(t)'?" },
    { "is_whitespace", "did you mean 'is_space(t)'?" },
    /* Maps (D68): another language's dictionary spellings. */
    { "dict", "a map is `let m: Map[Text, Int] = {};` (Text keys); read `m[k]`, write `m[k] = v;`." },
    { "Dict", "a map is `let m: Map[Text, Int] = {};` (Text keys); read `m[k]`, write `m[k] = v;`." },
    { "HashMap", "a map is `let m: Map[Text, Int] = {};` (Text keys); read `m[k]`, write `m[k] = v;`." },
    { "Map", "a map literal is `{\"k\": v}`, and `{}` with a declared type: `let m: Map[Text, Int] = {};`." },
    { "map_new", "a map literal is `{\"k\": v}`, and `{}` with a declared type: `let m: Map[Text, Int] = {};`." },
    { "new_map", "a map literal is `{\"k\": v}`, and `{}` with a declared type: `let m: Map[Text, Int] = {};`." },
    { "has_key", "did you mean 'has(m, k)'?" },
    { "contains_key", "did you mean 'has(m, k)'?" },
    { "haskey", "did you mean 'has(m, k)'?" },
    { "get_or", "did you mean 'get(m, k, default)'?" },
    { "get_or_default", "did you mean 'get(m, k, default)'?" },
    { "getordefault", "did you mean 'get(m, k, default)'?" },
    { "map_get", "write `m[k]`, or 'get(m, k, default)' when the key may be absent." },
    { "map_set", "write `m[k] = v;`." },
    { "map_put", "write `m[k] = v;`." },
    { "map_has", "did you mean 'has(m, k)'?" },
    { "map_remove", "did you mean 'remove(m, k)'?" },
    { "map_keys", "did you mean 'keys(m)'?" },
    { "map_values", "did you mean 'values(m)'?" },
    { "items", "loop over a map's keys: `loop k in m { ... m[k] ... }`." },
    { "pop", "did you mean 'remove(m, k)'? Read `m[k]` first if you need the value." },
    { "del", "did you mean 'remove(m, k)'?" },
};

static const char *aetherNameHintFor(const char *name) {
    for (size_t i = 0; i < sizeof(kAetherNameHints) / sizeof(kAetherNameHints[0]); i++) {
        if (strcmp(name, kAetherNameHints[i].wrong) == 0) {
            return kAetherNameHints[i].hint;
        }
    }
    return NULL;
}

/* A camelCase spelling of a snake_case name (`isDigit`, `mstreamLength`,
 * `parseRow` for the program's own `parse_row`). Builtin names are snake_case
 * (D72), so the table above lists only the run-together lowercase misses; a
 * model that writes the camelCase form got no hint at all. Converts the name
 * (an `_` before an uppercase letter that follows a lowercase letter or digit,
 * or that starts a word after an acronym: `parseJSONLine` -> `parse_json_line`;
 * all lowercase) and returns, in order: the table's hint for the snake_case
 * form, the table's hint for the run-together form (`isDigit` -> `isdigit`,
 * which names the parameters), or "did you mean 'snake(...)'?" when the snake
 * form is a top-level fn of this program or a builtin. NULL otherwise. The
 * result lives in a static buffer until the next call (compiles are
 * single-threaded). */
static const char *aetherCamelCaseHint(const char *ident) {
    static char snake[128];
    static char flat[128];
    static char hint[320];
    size_t n = strlen(ident);
    size_t si = 0, fi = 0;
    bool interiorUpper = false;
    const char *found;

    if (n == 0 || n >= 64 || !islower((unsigned char)ident[0])) {
        return NULL;
    }
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)ident[k];
        if (!isalnum(c) && c != '_') {
            return NULL;
        }
        if (isupper(c)) {
            unsigned char prev = (unsigned char)ident[k - 1];
            unsigned char next = (unsigned char)ident[k + 1];
            interiorUpper = true;
            if (prev != '_' && (islower(prev) || isdigit(prev) ||
                                (isupper(prev) && islower(next)))) {
                snake[si++] = '_';
            }
        }
        snake[si++] = (char)tolower(c);
        if (c != '_') {
            flat[fi++] = (char)tolower(c);
        }
    }
    snake[si] = '\0';
    flat[fi] = '\0';
    if (!interiorUpper) {
        return NULL;
    }
    if ((found = aetherNameHintFor(snake)) != NULL || (found = aetherNameHintFor(flat)) != NULL) {
        return found;
    }
    if (aetherAstIsTopLevelUserFunction(snake)) {
        snprintf(hint, sizeof(hint), "did you mean '%s(...)'? The program declares `fn %s`.",
                 snake, snake);
        return hint;
    }
    if (!aetherBuiltinNameRemoved(snake) &&
        (aetherRuntimeBuiltinAlias(snake) != NULL || getVmBuiltinID(snake) >= 0)) {
        snprintf(hint, sizeof(hint), "did you mean '%s(...)'? Builtin names are snake_case (D72).",
                 snake);
        return hint;
    }
    return NULL;
}

/* The run-together lowercase form of a name: `upper_case` -> `uppercase`,
 * `toUpper` -> `toupper`. Empty when it does not fit. */
static void aetherFlatName(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        if (*in == '_') continue;
        out[o++] = (char)tolower((unsigned char)*in);
    }
    out[o] = '\0';
    if (in && *in) out[0] = '\0';
}

/* The table's hint for a name that differs from an entry only by case and
 * underscores (`upper_case` for `uppercase`, `str_len` for `strlen`). */
static const char *aetherFlatTableHint(const char *ident) {
    char flat[96], key[96];
    aetherFlatName(ident, flat, sizeof(flat));
    if (!flat[0] || strcmp(flat, ident) == 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(kAetherNameHints) / sizeof(kAetherNameHints[0]); i++) {
        aetherFlatName(kAetherNameHints[i].wrong, key, sizeof(key));
        if (strcmp(key, flat) == 0) {
            return kAetherNameHints[i].hint;
        }
    }
    return NULL;
}

/* Optimal string alignment distance (Damerau-Levenshtein without repeated
 * edits of a substring), or limit + 1 once it is past limit. */
static int aetherEditDistance(const char *a, const char *b, int limit) {
    size_t n = strlen(a), m = strlen(b);
    if (n > 63 || m > 63) return limit + 1;
    if ((n > m ? n - m : m - n) > (size_t)limit) return limit + 1;
    int d[64][64];
    for (size_t i = 0; i <= n; i++) d[i][0] = (int)i;
    for (size_t j = 0; j <= m; j++) d[0][j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        int rowMin = limit + 1;
        for (size_t j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < v) v = d[i - 1][j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
                d[i - 2][j - 2] + 1 < v)
                v = d[i - 2][j - 2] + 1;
            d[i][j] = v;
            if (v < rowMin) rowMin = v;
        }
        if (rowMin > limit) return limit + 1;
    }
    return d[n][m];
}

typedef struct {
    char flat[96];
    int limit;
    int best;
    int count;
    char name[64];
    char signature[200];
    bool userFn;
} AetherCloseName;

static void aetherCloseNameVisit(const char *name, const char *signature, bool userFn,
                                 AetherCloseName *c) {
    char flat[96];
    if (!name || !*name || name[0] == '_' || strlen(name) >= sizeof(c->name)) return;
    for (const char *s = name; *s; s++) {
        if (isupper((unsigned char)*s)) return; /* an engine spelling, not Aether's (D72) */
    }
    aetherFlatName(name, flat, sizeof(flat));
    if (!flat[0] || flat[0] != c->flat[0]) return;
    int d = aetherEditDistance(c->flat, flat, c->limit);
    if (d > c->limit || d > c->best) return;
    if (d == c->best && strcmp(c->name, name) == 0) return;
    if (d < c->best) {
        c->best = d;
        c->count = 0;
    }
    c->count++;
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->signature, sizeof(c->signature), "%s", signature ? signature : "");
    c->userFn = userFn;
}

static void aetherCloseBuiltinVisit(const char *name, const char *signature, void *ctx) {
    aetherCloseNameVisit(name, signature, false, (AetherCloseName *)ctx);
}

/* A builtin or a function of this program one or two edits away from a name
 * that is not in scope (`lenght`, `starts_wiht`). Conservative, because the
 * name may be a misspelled variable instead: five characters or more, the
 * same first letter, one edit (two from seven characters), and a single
 * closest name. NULL otherwise; the result lives in a static buffer. */
static const char *aetherCloseSpellingHint(const char *ident) {
    static char hint[320];
    AetherCloseName c;
    memset(&c, 0, sizeof(c));
    aetherFlatName(ident, c.flat, sizeof(c.flat));
    size_t n = strlen(c.flat);
    if (n < 5) return NULL;
    c.limit = n >= 7 ? 2 : 1;
    c.best = c.limit + 1;
    /* The program's own names first: `countr` is more likely `counter`, a
     * misspelled local, than the builtin `count`. */
    for (size_t i = 0; i < aetherSemanticDeclaredValueCount(); i++) {
        aetherCloseNameVisit(aetherSemanticDeclaredValueAt(i), NULL, true, &c);
    }
    if (c.best <= c.limit && c.count == 1 && strcmp(c.name, ident) != 0) {
        if (aetherAstIsTopLevelUserFunction(c.name)) {
            snprintf(hint, sizeof(hint), "did you mean '%s(...)'? The program declares `fn %s`.",
                     c.name, c.name);
        } else {
            snprintf(hint, sizeof(hint), "did you mean '%s'?", c.name);
        }
        return hint;
    }
    if (c.best <= c.limit) return NULL; /* two of the program's names fit equally */
    pscalAetherForEachBuiltinName(aetherCloseBuiltinVisit, &c);
    for (size_t i = 0; i < aetherAstTopLevelUserFunctionCount(); i++) {
        aetherCloseNameVisit(aetherAstTopLevelUserFunctionAt(i), NULL, true, &c);
    }
    if (c.best > c.limit || c.count != 1 || strcmp(c.name, ident) == 0) return NULL;
    if (c.userFn) {
        snprintf(hint, sizeof(hint), "did you mean '%s(...)'? The program declares `fn %s`.",
                 c.name, c.name);
    } else if (c.signature[0]) {
        snprintf(hint, sizeof(hint), "did you mean '%s'? `%s`.", c.name, c.signature);
    } else {
        snprintf(hint, sizeof(hint), "did you mean '%s(...)'?", c.name);
    }
    return hint;
}

const char *aetherUnknownIdentifierHint(const char *ident) {
    const char *hint;

    if (!ident) {
        return NULL;
    }
    /* A TOON getter called with the wrong argument count is already a
     * BUILT-002; "" tells the engine not to add a SCOPE-001 for it. */
    if (aetherAstPrepassReportedArity(ident)) {
        return "";
    }
    if ((hint = aetherNameHintFor(ident)) != NULL) {
        return hint;
    }
    /* The checked SQL fns are prelude fns over engine builtins a build
     * without SQLite does not have (aetherStdlibPrelude leaves them out). */
    if (strncmp(ident, "try_sqlite_", 11) == 0 && getVmBuiltinID("aether_try_sqlite_exec") < 0) {
        return "SQLite support is not built into this compiler: rebuild aether with "
               "-DAETHER_ENABLE_SQLITE=ON (on Linux, install the libsqlite3 development package first).";
    }
    if ((hint = aetherCamelCaseHint(ident)) != NULL) {
        return hint;
    }
    if ((hint = aetherFlatTableHint(ident)) != NULL) {
        return hint;
    }
    return aetherCloseSpellingHint(ident);
}
