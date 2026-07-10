#include "aether/diagnostics.h"

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
        if (strcmp(kind, "scope") == 0) {
            return "SCOPE-001";
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
    if (strstr(detail, "unable to open module")) {
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
// No-op when code is NULL (an uncoded diagnostic). The named file is the
// condensed guide every section heading and the troubleshooting table key on.
void aetherReportGuideHelp(const char *code) {
    if (!code) {
        return;
    }
    fprintf(stderr,
            "help: see %s in the Aether guide "
            "(aether_for_llms_with_small_contexts.md)\n",
            code);
}
