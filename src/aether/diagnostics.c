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
         * human message reads "Aether tuple rewrite error". Internal "tuple ...
         * rewrite failed" defects and @post-slot errors use other kinds, so they
         * are intentionally not captured here. */
        if (strcmp(kind, "tuple") == 0) {
            return "TUP-001";
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
    if (strstr(detail, "@pre must annotate the next function declaration") ||
        strstr(detail, "@post must annotate the next function declaration") ||
        strstr(detail, "cannot call effectful builtin") ||
        strstr(detail, "cannot call non-pure function")) {
        return "ANN-001";
    }
    if (strstr(detail, "tuple return types are not supported yet")) {
        return "TUP-001";
    }
    if (strstr(detail, "opaque TOON handle") ||
        strstr(detail, "expects a ToonDoc handle") ||
        strstr(detail, "expects a ToonNode handle") ||
        strstr(detail, " first argument") ||
        strstr(detail, " second argument") ||
        strstr(detail, " third argument")) {
        return "TOON-001";
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
