#include "aether/semantic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/globals.h"
#include "core/type_registry.h"
#include "backend_ast/builtin.h"
#include "aether/diagnostics.h"
#include "aether/ast_prepasses.h"   /* aetherAstCollectImportedTypes (NAME-001) */
#include "aether/parser.h"
#include "vm/vm_fx_policy.h"            /* the builtins_json listing (arity, D-arity fallback) */
#include "aether/experiment.h"
#include "aether/types.h"
#include "rea/semantic.h"

/* Opaque handle kinds tracked by the textual type-flow pass. The TOON pair
 * predates MStream; `kind` was formerly a boolean named isDoc. */
#define AETHER_HANDLE_NODE 0
#define AETHER_HANDLE_DOC 1
#define AETHER_HANDLE_MSTREAM 2

typedef struct AetherOpaqueBinding {
    char *name;
    int kind;
} AetherOpaqueBinding;

typedef struct AetherOpaqueBindingTable {
    AetherOpaqueBinding *items;
    size_t count;
    size_t cap;
} AetherOpaqueBindingTable;

typedef struct AetherScalarBinding {
    char *name;
    const char *typeName;
} AetherScalarBinding;

typedef struct AetherScalarBindingTable {
    AetherScalarBinding *items;
    size_t count;
    size_t cap;
} AetherScalarBindingTable;

static const char *g_aether_source_path = NULL;
static void reportAetherError(const char *kind, int line, const char *detail);
static int expectedOpaqueReturnKind(const char *name, size_t len, int *returnsKind);

/* Aether source-level type name of an opaque handle kind. */
static const char *opaqueHandleTypeName(int kind) {
    switch (kind) {
        case AETHER_HANDLE_DOC: return "ToonDoc";
        case AETHER_HANDLE_MSTREAM: return "MStream";
        default: return "ToonNode";
    }
}

/* Diagnostic code for a handle-discipline error: the TOON pair keeps TOON-001;
 * memory streams point at the MS-001 guide section. */
static const char *opaqueHandleCode(int kind) {
    return kind == AETHER_HANDLE_MSTREAM ? "MS-001" : "TOON-001";
}
static const char *expectedScalarReturnTypeName(const char *name, size_t len);
static const char *skipQuotedString(const char *cursor, int *line);
static int isArithmeticChar(char ch);

static void freeOpaqueBindingTable(AetherOpaqueBindingTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static void freeScalarBindingTable(AetherScalarBindingTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureOpaqueBindingTable(AetherOpaqueBindingTable *table, size_t extra) {
    AetherOpaqueBinding *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 16;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (AetherOpaqueBinding *)realloc(table->items, newCap * sizeof(AetherOpaqueBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int ensureScalarBindingTable(AetherScalarBindingTable *table, size_t extra) {
    AetherScalarBinding *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 16;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (AetherScalarBinding *)realloc(table->items, newCap * sizeof(AetherScalarBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static char *dupRange(const char *start, const char *end) {
    char *copy;
    size_t len;

    if (!start || !end || end < start) {
        return NULL;
    }
    len = (size_t)(end - start);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static const char *skipInlineSpaces(const char *cursor, const char *lineEnd) {
    while (cursor < lineEnd && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    return cursor;
}

static char *sanitizeAetherSourceForSemanticScan(const char *source) {
    size_t len;
    char *copy;
    size_t i = 0;
    int inLineComment = 0;
    int inBlockComment = 0;

    if (!source) {
        return NULL;
    }
    len = strlen(source);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, source, len + 1);

    while (i < len) {
        if (inLineComment) {
            if (copy[i] == '\n') {
                inLineComment = 0;
            } else {
                copy[i] = ' ';
            }
            i++;
            continue;
        }
        if (inBlockComment) {
            if (copy[i] == '*' && i + 1 < len && copy[i + 1] == '/') {
                copy[i] = ' ';
                copy[i + 1] = ' ';
                inBlockComment = 0;
                i += 2;
                continue;
            }
            if (copy[i] != '\n') {
                copy[i] = ' ';
            }
            i++;
            continue;
        }
        if (copy[i] == '"' || copy[i] == '\'') {
            const char *after = skipQuotedString(copy + i, NULL);
            i += (size_t)(after - (copy + i));
            continue;
        }
        if (copy[i] == '/' && i + 1 < len && copy[i + 1] == '/') {
            copy[i] = ' ';
            copy[i + 1] = ' ';
            inLineComment = 1;
            i += 2;
            continue;
        }
        if (copy[i] == '/' && i + 1 < len && copy[i + 1] == '*') {
            copy[i] = ' ';
            copy[i + 1] = ' ';
            inBlockComment = 1;
            i += 2;
            continue;
        }
        i++;
    }

    return copy;
}

static const char *skipQuotedString(const char *cursor, int *line) {
    char quote = *cursor++;

    while (*cursor) {
        if (*cursor == '\n' && line) {
            (*line)++;
        }
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 2;
            continue;
        }
        if (*cursor == quote) {
            cursor++;
            break;
        }
        cursor++;
    }
    return cursor;
}

static int startsWithWord(const char *body, const char *lineEnd, const char *word) {
    size_t len = word ? strlen(word) : 0;

    if (!body || !lineEnd || !word || len == 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) < len) {
        return 0;
    }
    if (strncmp(body, word, len) != 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) == len) {
        return 1;
    }
    return isspace((unsigned char)body[len]) || body[len] == '{' || body[len] == ';';
}

static int isSupportedCostUnit(const char *start, const char *end) {
    size_t len;

    if (!start || !end || end < start) {
        return 0;
    }
    len = (size_t)(end - start);
    return (len == 2 && strncmp(start, "ns", 2) == 0) ||
           (len == 2 && strncmp(start, "us", 2) == 0) ||
           (len == 2 && strncmp(start, "ms", 2) == 0) ||
           (len == 1 && strncmp(start, "s", 1) == 0) ||
           (len == 2 && strncmp(start, "op", 2) == 0) ||
           (len == 3 && strncmp(start, "ops", 3) == 0) ||
           (len == 4 && strncmp(start, "step", 4) == 0) ||
           (len == 5 && strncmp(start, "steps", 5) == 0);
}

static int validateCostAnnotationSyntax(const char *body,
                                        const char *lineEnd,
                                        char *detail,
                                        size_t detailSize) {
    const char *cursor;
    const char *digitsStart;
    const char *digitsEnd;
    const char *unitStart;
    const char *unitEnd;
    long budget = 0;

    if (!body || !lineEnd || !detail || detailSize == 0) {
        return 0;
    }
    detail[0] = '\0';
    if (!startsWithWord(body, lineEnd, "@cost")) {
        snprintf(detail, detailSize, "internal @cost validator mismatch.");
        return 0;
    }

    cursor = skipInlineSpaces(body + 5, lineEnd);
    if (cursor >= lineEnd) {
        snprintf(detail, detailSize, "@cost requires a positive integer budget.");
        return 0;
    }
    digitsStart = cursor;
    while (cursor < lineEnd && isdigit((unsigned char)*cursor)) {
        budget = budget * 10 + (*cursor - '0');
        cursor++;
    }
    digitsEnd = cursor;
    if (digitsEnd == digitsStart) {
        snprintf(detail, detailSize, "@cost requires a positive integer budget.");
        return 0;
    }
    if (budget <= 0) {
        snprintf(detail, detailSize, "@cost budget must be greater than zero.");
        return 0;
    }

    unitStart = skipInlineSpaces(cursor, lineEnd);
    if (unitStart >= lineEnd) {
        return 1;
    }
    unitEnd = unitStart;
    while (unitEnd < lineEnd && isalpha((unsigned char)*unitEnd)) {
        unitEnd++;
    }
    if (unitEnd == unitStart) {
        snprintf(detail, detailSize, "@cost has invalid trailing syntax.");
        return 0;
    }
    if (!isSupportedCostUnit(unitStart, unitEnd)) {
        snprintf(detail,
                 detailSize,
                 "unsupported @cost unit '%.*s'.",
                 (int)(unitEnd - unitStart),
                 unitStart);
        return 0;
    }
    cursor = skipInlineSpaces(unitEnd, lineEnd);
    if (cursor < lineEnd) {
        snprintf(detail, detailSize, "@cost has invalid trailing syntax.");
        return 0;
    }
    return 1;
}

static const char *skipAnnotationTail(const char *cursor, const char *lineEnd) {
    const char *trimmed;

    trimmed = skipInlineSpaces(cursor, lineEnd);
    if (trimmed + 1 < lineEnd && trimmed[0] == '/' && trimmed[1] == '/') {
        return lineEnd;
    }
    return trimmed;
}

static int validateContractExprAnnotation(const char *body,
                                          const char *lineEnd,
                                          const char *directive,
                                          char *detail,
                                          size_t detailSize) {
    const char *cursor;

    if (!body || !lineEnd || !directive || !detail || detailSize == 0) {
        return 0;
    }
    detail[0] = '\0';
    if (!startsWithWord(body, lineEnd, directive)) {
        snprintf(detail, detailSize, "internal %s validator mismatch.", directive);
        return 0;
    }
    cursor = skipAnnotationTail(body + strlen(directive), lineEnd);
    if (cursor >= lineEnd) {
        snprintf(detail, detailSize, "%s requires an expression.", directive);
        return 0;
    }
    return 1;
}

static int validatePureAnnotationSyntax(const char *body,
                                        const char *lineEnd,
                                        char *detail,
                                        size_t detailSize) {
    const char *cursor;

    if (!body || !lineEnd || !detail || detailSize == 0) {
        return 0;
    }
    detail[0] = '\0';
    if (!startsWithWord(body, lineEnd, "@pure")) {
        snprintf(detail, detailSize, "internal @pure validator mismatch.");
        return 0;
    }
    cursor = skipAnnotationTail(body + 5, lineEnd);
    if (cursor < lineEnd) {
        snprintf(detail, detailSize, "@pure does not take arguments.");
        return 0;
    }
    return 1;
}

static int addOpaqueBinding(AetherOpaqueBindingTable *table, const char *name, int kind) {
    size_t i;
    char *copy;

    if (!table || !name) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            table->items[i].kind = kind;
            return 1;
        }
    }
    if (!ensureOpaqueBindingTable(table, 1)) {
        return 0;
    }
    copy = dupRange(name, name + strlen(name));
    if (!copy) {
        return 0;
    }
    table->items[table->count].name = copy;
    table->items[table->count].kind = kind;
    table->count++;
    return 1;
}

static int addScalarBinding(AetherScalarBindingTable *table,
                            const char *name,
                            const char *typeName) {
    size_t i;
    char *copy;

    if (!table || !name || !typeName) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            table->items[i].typeName = typeName;
            return 1;
        }
    }
    if (!ensureScalarBindingTable(table, 1)) {
        return 0;
    }
    copy = dupRange(name, name + strlen(name));
    if (!copy) {
        return 0;
    }
    table->items[table->count].name = copy;
    table->items[table->count].typeName = typeName;
    table->count++;
    return 1;
}

static const AetherOpaqueBinding *findOpaqueBinding(const AetherOpaqueBindingTable *table,
                                                    const char *name,
                                                    size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static const AetherScalarBinding *findScalarBinding(const AetherScalarBindingTable *table,
                                                    const char *name,
                                                    size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Function-scoped textual type-flow.
 *
 * The TOON-handle and scalar tables used to be collected once over the WHOLE
 * source, so `let v: Int` in one function clobbered `let v: Text` in another
 * (last-write-wins), yielding both false TYPE-001/TOON-001 positives and
 * missed negatives. The passes now run per function: each physical line is
 * mapped to the function whose body contains it (0 = top level), the global
 * (top-level) tables are collected first and stay visible program-wide, and
 * every function gets a fresh copy of the globals plus only its own locals.
 * A local that shadows a global simply overwrites the entry in that
 * function's COPY, so the global is intact again for the next function.
 * -------------------------------------------------------------------------- */

/* Map each physical source line to the function whose signature/body contains
 * it: 0 = top level (including `type` field lines and `mod`-level consts),
 * 1..N = the N `fn` declarations (methods and module fns included) in source
 * order. Lines between a `fn` keyword and its opening brace belong to that
 * function. Returns a malloc'd array of *lineCountOut entries (caller frees),
 * or NULL on allocation failure (callers fall back to the flat scan). The
 * source is the sanitized scan source (comments blanked; strings present). */
static int *computeLineFunctionIds(const char *source, size_t *lineCountOut,
                                   int *fnCountOut) {
    size_t lineCount = 1;
    size_t lineIdx = 0;
    const char *s;
    const char *cursor = source;
    int *ids;
    int depth = 0;
    int inFn = 0;
    int pendingFn = 0;
    int fnBodyDepth = 0;
    int curId = 0;
    int nextId = 1;

    if (!source) {
        return NULL;
    }
    for (s = source; *s; s++) {
        if (*s == '\n') {
            lineCount++;
        }
    }
    ids = (int *)calloc(lineCount, sizeof(int));
    if (!ids) {
        return NULL;
    }

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        const char *scan;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        if (!inFn && !pendingFn && startsWithWord(body, lineEnd, "fn")) {
            pendingFn = 1;
            curId = nextId++;
        }
        if (lineIdx < lineCount) {
            ids[lineIdx] = (inFn || pendingFn) ? curId : 0;
        }
        for (scan = lineStart; scan < lineEnd;) {
            if (*scan == '"' || *scan == '\'') {
                const char *after = skipQuotedString(scan, NULL);
                scan = (after > scan) ? after : scan + 1;
                if (scan > lineEnd) {
                    scan = lineEnd; /* unterminated on this line: clamp */
                }
                continue;
            }
            if (*scan == '{') {
                depth++;
                if (pendingFn) {
                    pendingFn = 0;
                    inFn = 1;
                    fnBodyDepth = depth;
                }
            } else if (*scan == '}') {
                if (depth > 0) {
                    depth--;
                }
                if (inFn && depth < fnBodyDepth) {
                    inFn = 0;
                    curId = 0;
                }
            }
            scan++;
        }
        lineIdx++;
        cursor = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;
    }

    if (lineCountOut) {
        *lineCountOut = lineCount;
    }
    if (fnCountOut) {
        *fnCountOut = nextId - 1;
    }
    return ids;
}

/* True when line `lineIdx` (0-based) should be skipped by a pass that is
 * scoped to function `fnId`. A NULL id array disables filtering (legacy
 * whole-source behavior, used only on allocation failure). */
static int lineOutsideScope(const int *lineFnIds, size_t lineCount,
                            size_t lineIdx, int fnId) {
    if (!lineFnIds) {
        return 0;
    }
    if (lineIdx >= lineCount) {
        return fnId != 0;
    }
    return lineFnIds[lineIdx] != fnId;
}

/* Seed a function's working table with the program-wide (top-level) bindings.
 * Returns 0 on allocation failure; dst stays freeable either way. */
static int copyOpaqueBindingTable(AetherOpaqueBindingTable *dst,
                                  const AetherOpaqueBindingTable *src) {
    size_t i;

    dst->items = NULL;
    dst->count = 0;
    dst->cap = 0;
    if (!src || src->count == 0) {
        return 1;
    }
    if (!ensureOpaqueBindingTable(dst, src->count)) {
        return 0;
    }
    for (i = 0; i < src->count; i++) {
        char *name = dupRange(src->items[i].name,
                              src->items[i].name + strlen(src->items[i].name));
        if (!name) {
            return 0;
        }
        dst->items[dst->count].name = name;
        dst->items[dst->count].kind = src->items[i].kind;
        dst->count++;
    }
    return 1;
}

static int copyScalarBindingTable(AetherScalarBindingTable *dst,
                                  const AetherScalarBindingTable *src) {
    size_t i;

    dst->items = NULL;
    dst->count = 0;
    dst->cap = 0;
    if (!src || src->count == 0) {
        return 1;
    }
    if (!ensureScalarBindingTable(dst, src->count)) {
        return 0;
    }
    for (i = 0; i < src->count; i++) {
        char *name = dupRange(src->items[i].name,
                              src->items[i].name + strlen(src->items[i].name));
        if (!name) {
            return 0;
        }
        dst->items[dst->count].name = name;
        dst->items[dst->count].typeName = src->items[i].typeName;
        dst->count++;
    }
    return 1;
}

static int isBoolLiteralRange(const char *start, const char *end) {
    size_t len;

    if (!start || !end || end <= start) {
        return 0;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    len = (size_t)(end - start);
    return (len == 4 && strncmp(start, "true", 4) == 0) ||
           (len == 5 && strncmp(start, "false", 5) == 0);
}

static const char *inferNumericTypeName(const char *start, const char *end) {
    const char *cursor;
    int sawDigit = 0;
    int sawDot = 0;
    int sawExp = 0;

    if (!start || !end || end <= start) {
        return NULL;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start >= end) {
        return NULL;
    }
    cursor = start;
    if (*cursor == '+' || *cursor == '-') {
        cursor++;
    }
    while (cursor < end) {
        if (isdigit((unsigned char)*cursor)) {
            sawDigit = 1;
            cursor++;
            continue;
        }
        if (*cursor == '.' && !sawDot && !sawExp) {
            sawDot = 1;
            cursor++;
            continue;
        }
        if ((*cursor == 'e' || *cursor == 'E') && !sawExp && sawDigit) {
            sawExp = 1;
            sawDot = 1;
            cursor++;
            if (cursor < end && (*cursor == '+' || *cursor == '-')) {
                cursor++;
            }
            continue;
        }
        return NULL;
    }
    if (!sawDigit) {
        return NULL;
    }
    return sawDot ? "Real" : "Int";
}

static const char *mergeNumericOperandTypes(const char *currentType, const char *operandType) {
    if (!operandType) {
        return NULL;
    }
    if (strcmp(operandType, "Int") != 0 && strcmp(operandType, "Real") != 0) {
        return NULL;
    }
    if (!currentType) {
        return operandType;
    }
    if (strcmp(currentType, "Real") == 0 || strcmp(operandType, "Real") == 0) {
        return "Real";
    }
    return "Int";
}

static const char *inferScalarExpressionTypeName(const char *start,
                                                 const char *end,
                                                 const AetherScalarBindingTable *scalarBindings) {
    const char *trimmedStart = start;
    const char *trimmedEnd = end;
    const char *cursor;
    const char *mergedType = NULL;
    int sawArithmetic = 0;

    if (!start || !end || end <= start) {
        return NULL;
    }
    while (trimmedStart < trimmedEnd && isspace((unsigned char)*trimmedStart)) {
        trimmedStart++;
    }
    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
        trimmedEnd--;
    }
    if (trimmedEnd > trimmedStart && trimmedEnd[-1] == ';') {
        trimmedEnd--;
    }
    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
        trimmedEnd--;
    }
    if (trimmedStart >= trimmedEnd) {
        return NULL;
    }

    cursor = trimmedStart;
    while (cursor < trimmedEnd) {
        const char *tokenStart;
        const char *tokenEnd;
        const char *operandType;

        while (cursor < trimmedEnd && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (cursor >= trimmedEnd) {
            break;
        }
        if (*cursor == '"' || *cursor == '\'') {
            return NULL;
        }
        if (isArithmeticChar(*cursor)) {
            sawArithmetic = 1;
            cursor++;
            continue;
        }
        if (*cursor == '(' || *cursor == ')' || *cursor == ',') {
            cursor++;
            continue;
        }

        tokenStart = cursor;
        if (isdigit((unsigned char)*cursor) ||
            ((*cursor == '+' || *cursor == '-') &&
             cursor + 1 < trimmedEnd &&
             isdigit((unsigned char)cursor[1]))) {
            cursor++;
            while (cursor < trimmedEnd &&
                   (isdigit((unsigned char)*cursor) || *cursor == '.' ||
                    *cursor == 'e' || *cursor == 'E' ||
                    *cursor == '+' || *cursor == '-')) {
                cursor++;
            }
            tokenEnd = cursor;
            operandType = inferNumericTypeName(tokenStart, tokenEnd);
        } else if (isalpha((unsigned char)*cursor) || *cursor == '_') {
            while (cursor < trimmedEnd &&
                   (isalnum((unsigned char)*cursor) || *cursor == '_')) {
                cursor++;
            }
            tokenEnd = cursor;
            cursor = skipInlineSpaces(cursor, trimmedEnd);
            if (cursor < trimmedEnd && *cursor == '(') {
                operandType = expectedScalarReturnTypeName(tokenStart,
                                                           (size_t)(tokenEnd - tokenStart));
            } else {
                const AetherScalarBinding *binding;

                binding = findScalarBinding(scalarBindings,
                                            tokenStart,
                                            (size_t)(tokenEnd - tokenStart));
                operandType = binding ? binding->typeName : NULL;
            }
        } else {
            return NULL;
        }

        mergedType = mergeNumericOperandTypes(mergedType, operandType);
        if (!mergedType) {
            return NULL;
        }
    }

    return sawArithmetic ? mergedType : NULL;
}

static const char *inferInitializerTypeName(const char *start,
                                            const char *end,
                                            const AetherOpaqueBindingTable *opaqueBindings,
                                            const AetherScalarBindingTable *scalarBindings,
                                            int *isOpaqueDoc) {
    const char *trimmedStart = start;
    const char *trimmedEnd = end;
    const char *nameEnd;
    const AetherOpaqueBinding *opaqueBinding;
    const AetherScalarBinding *scalarBinding;
    const char *numericType;
    int returnsDoc = 0;

    if (isOpaqueDoc) {
        *isOpaqueDoc = 0;
    }
    if (!start || !end || end < start) {
        return NULL;
    }
    while (trimmedStart < trimmedEnd && isspace((unsigned char)*trimmedStart)) {
        trimmedStart++;
    }
    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
        trimmedEnd--;
    }
    if (trimmedEnd > trimmedStart && trimmedEnd[-1] == ';') {
        trimmedEnd--;
    }
    while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
        trimmedEnd--;
    }
    if (trimmedStart >= trimmedEnd) {
        return NULL;
    }
    if (*trimmedStart == '"' && trimmedEnd[-1] == '"') {
        return "Text";
    }
    if (isBoolLiteralRange(trimmedStart, trimmedEnd)) {
        return "Bool";
    }
    numericType = inferNumericTypeName(trimmedStart, trimmedEnd);
    if (numericType) {
        return numericType;
    }
    nameEnd = trimmedStart;
    if (!(isalpha((unsigned char)*nameEnd) || *nameEnd == '_')) {
        return NULL;
    }
    while (nameEnd < trimmedEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    if (nameEnd == trimmedEnd) {
        opaqueBinding = findOpaqueBinding(opaqueBindings,
                                          trimmedStart,
                                          (size_t)(trimmedEnd - trimmedStart));
        if (opaqueBinding) {
            if (isOpaqueDoc) {
                *isOpaqueDoc = opaqueBinding->kind;
            }
            return opaqueHandleTypeName(opaqueBinding->kind);
        }
        scalarBinding = findScalarBinding(scalarBindings,
                                          trimmedStart,
                                          (size_t)(trimmedEnd - trimmedStart));
        if (scalarBinding) {
            return scalarBinding->typeName;
        }
        return NULL;
    }
    if (*skipInlineSpaces(nameEnd, trimmedEnd) != '(') {
        return NULL;
    }
    if (expectedOpaqueReturnKind(trimmedStart, (size_t)(nameEnd - trimmedStart), &returnsDoc)) {
        if (isOpaqueDoc) {
            *isOpaqueDoc = returnsDoc;
        }
        return opaqueHandleTypeName(returnsDoc);
    }
    if ((size_t)(nameEnd - trimmedStart) == 7 && strncmp(trimmedStart, "ai_chat", 7) == 0) {
        return "Text";
    }
    if ((size_t)(nameEnd - trimmedStart) == 13 && strncmp(trimmedStart, "builtins_json", 13) == 0) {
        return "Text";
    }
    if ((size_t)(nameEnd - trimmedStart) == 12 && strncmp(trimmedStart, "builtin_info", 12) == 0) {
        return "Text";
    }
    if ((size_t)(nameEnd - trimmedStart) == 8 && strncmp(trimmedStart, "has_toon", 8) == 0) {
        return "Bool";
    }
    if ((size_t)(nameEnd - trimmedStart) == 6 && strncmp(trimmedStart, "has_ai", 6) == 0) {
        return "Bool";
    }
    if ((size_t)(nameEnd - trimmedStart) == 11 && strncmp(trimmedStart, "has_builtin", 11) == 0) {
        return "Bool";
    }
    return expectedScalarReturnTypeName(trimmedStart, (size_t)(nameEnd - trimmedStart));
}

static void collectOpaqueBindings(const char *source, AetherOpaqueBindingTable *table,
                                  const int *lineFnIds, size_t lineCount, int fnId) {
    const char *cursor = source;
    size_t lineIdx = 0;

    while (cursor && *cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        size_t thisLine = lineIdx++;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        if (lineOutsideScope(lineFnIds, lineCount, thisLine, fnId)) {
            cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
            continue;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
            const char *scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
            const char *nameStart;
            const char *nameEnd;
            const char *colon;
            const char *typeStart = NULL;
            const char *typeEnd = NULL;
            const char *equals;
            int isDoc = 0;

            scan = skipInlineSpaces(scan, lineEnd);
            nameStart = scan;
            while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
                scan++;
            }
            nameEnd = scan;
            colon = skipInlineSpaces(scan, lineEnd);
            if (nameEnd > nameStart && colon < lineEnd && *colon == ':') {
                typeStart = skipInlineSpaces(colon + 1, lineEnd);
                typeEnd = typeStart;
                while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';' &&
                       !isspace((unsigned char)*typeEnd)) {
                    typeEnd++;
                }
                if ((size_t)(typeEnd - typeStart) == 7 &&
                    strncmp(typeStart, "ToonDoc", 7) == 0) {
                    isDoc = AETHER_HANDLE_DOC;
                } else if ((size_t)(typeEnd - typeStart) == 8 &&
                           strncmp(typeStart, "ToonNode", 8) == 0) {
                    isDoc = AETHER_HANDLE_NODE;
                } else if ((size_t)(typeEnd - typeStart) == 7 &&
                           strncmp(typeStart, "MStream", 7) == 0) {
                    isDoc = AETHER_HANDLE_MSTREAM;
                } else {
                    cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                    continue;
                }
            } else {
                equals = colon;
                while (equals < lineEnd && *equals != '=') {
                    equals++;
                }
                if (equals >= lineEnd || *equals != '=') {
                    cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                    continue;
                }
                if (!inferInitializerTypeName(skipInlineSpaces(equals + 1, lineEnd),
                                             lineEnd,
                                             table,
                                             NULL,
                                             &isDoc)) {
                    cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                    continue;
                }
                {
                    const char *typeName = inferInitializerTypeName(skipInlineSpaces(equals + 1, lineEnd),
                                                                    lineEnd,
                                                                    table,
                                                                    NULL,
                                                                    &isDoc);
                    if (!typeName ||
                        !(strcmp(typeName, "ToonDoc") == 0 ||
                          strcmp(typeName, "ToonNode") == 0 ||
                          strcmp(typeName, "MStream") == 0)) {
                        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                        continue;
                    }
                }
            }
            {
                char *name = dupRange(nameStart, nameEnd);
                if (name) {
                    addOpaqueBinding(table, name, isDoc);
                    free(name);
                }
            }
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
}

static void collectScalarBindings(const char *source, AetherScalarBindingTable *table,
                                  const int *lineFnIds, size_t lineCount, int fnId) {
    const char *cursor = source;
    size_t lineIdx = 0;

    while (cursor && *cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        size_t thisLine = lineIdx++;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        if (lineOutsideScope(lineFnIds, lineCount, thisLine, fnId)) {
            cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
            continue;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
            const char *scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
            const char *nameStart;
            const char *nameEnd;
            const char *colon;
            const char *typeStart = NULL;
            const char *typeEnd = NULL;
            const char *equals;
            char *name;
            const char *inferredType = NULL;

            scan = skipInlineSpaces(scan, lineEnd);
            nameStart = scan;
            while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
                scan++;
            }
            nameEnd = scan;
            colon = skipInlineSpaces(scan, lineEnd);
            if (nameEnd == nameStart) {
                cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                continue;
            }
            if (colon < lineEnd && *colon == ':') {
                typeStart = skipInlineSpaces(colon + 1, lineEnd);
                typeEnd = typeStart;
                while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';' &&
                       !isspace((unsigned char)*typeEnd)) {
                    typeEnd++;
                }
            } else {
                equals = colon;
                while (equals < lineEnd && *equals != '=') {
                    equals++;
                }
                if (equals >= lineEnd || *equals != '=') {
                    cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                    continue;
                }
                inferredType = inferInitializerTypeName(skipInlineSpaces(equals + 1, lineEnd),
                                                        lineEnd,
                                                        NULL,
                                                        table,
                                                        NULL);
            }

            name = dupRange(nameStart, nameEnd);
            if (!name) {
                cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
                continue;
            }
            if (typeStart && (size_t)(typeEnd - typeStart) == 4 && strncmp(typeStart, "Text", 4) == 0) {
                addScalarBinding(table, name, "Text");
            } else if (typeStart && (size_t)(typeEnd - typeStart) == 4 &&
                       strncmp(typeStart, "TOON", 4) == 0) {
                addScalarBinding(table, name, "TOON");
            } else if (typeStart && (size_t)(typeEnd - typeStart) == 3 &&
                       strncmp(typeStart, "Int", 3) == 0) {
                addScalarBinding(table, name, "Int");
            } else if (typeStart && (size_t)(typeEnd - typeStart) == 4 &&
                       strncmp(typeStart, "Real", 4) == 0) {
                addScalarBinding(table, name, "Real");
            } else if (typeStart && (size_t)(typeEnd - typeStart) == 4 &&
                       strncmp(typeStart, "Bool", 4) == 0) {
                addScalarBinding(table, name, "Bool");
            } else if (inferredType &&
                       (strcmp(inferredType, "Text") == 0 ||
                        strcmp(inferredType, "TOON") == 0 ||
                        strcmp(inferredType, "Int") == 0 ||
                        strcmp(inferredType, "Real") == 0 ||
                        strcmp(inferredType, "Bool") == 0)) {
                addScalarBinding(table, name, inferredType);
            }
            free(name);
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
}

static void reportDetachedAnnotation(const char *directive, int line) {
    char detail[256];

    snprintf(detail, sizeof(detail), "%s must annotate the next function declaration.", directive);
    reportAetherError("contract", line, detail);
}

static void validateContractAnnotations(const char *source) {
    const char *cursor = source;
    int line = 1;
    int pendingPreLine = 0;
    int pendingPostLine = 0;
    int pendingPureLine = 0;
    int pendingCostLine = 0;

    while (cursor && *cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        char detail[256];

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipInlineSpaces(lineStart, lineEnd);

        if (startsWithWord(body, lineEnd, "@pre")) {
            if (!validateContractExprAnnotation(body, lineEnd, "@pre", detail, sizeof(detail))) {
                reportAetherError("contract", line, detail);
            }
            if (pendingPreLine == 0) {
                pendingPreLine = line;
            }
        } else if (startsWithWord(body, lineEnd, "@post")) {
            if (!validateContractExprAnnotation(body, lineEnd, "@post", detail, sizeof(detail))) {
                reportAetherError("contract", line, detail);
            }
            if (pendingPostLine == 0) {
                pendingPostLine = line;
            }
        } else if (startsWithWord(body, lineEnd, "@pure")) {
            if (!validatePureAnnotationSyntax(body, lineEnd, detail, sizeof(detail))) {
                reportAetherError("contract", line, detail);
            }
            if (pendingPureLine != 0) {
                reportAetherError("contract",
                                  line,
                                  "duplicate @pure annotation before function declaration.");
            }
            pendingPureLine = line;
        } else if (startsWithWord(body, lineEnd, "@cost")) {
            if (!validateCostAnnotationSyntax(body, lineEnd, detail, sizeof(detail))) {
                reportAetherError("contract", line, detail);
            }
            if (pendingCostLine != 0) {
                reportAetherError("contract",
                                  line,
                                  "duplicate @cost annotation before function declaration.");
            }
            pendingCostLine = line;
        } else if (startsWithWord(body, lineEnd, "fn") ||
                   (startsWithWord(body, lineEnd, "export") &&
                    startsWithWord(skipInlineSpaces(body + 6, lineEnd), lineEnd, "fn"))) {
            /* `export fn` is the guide-canonical form inside a module body
             * (`@pure` sits above `export fn`), so it satisfies the annotation
             * the same way a bare `fn` does. */
            pendingPreLine = 0;
            pendingPostLine = 0;
            pendingPureLine = 0;
            pendingCostLine = 0;
        } else if (body < lineEnd && !(body[0] == '/' && body + 1 < lineEnd && body[1] == '/')) {
            if (pendingPreLine != 0) {
                reportDetachedAnnotation("@pre", pendingPreLine);
                pendingPreLine = 0;
            }
            if (pendingPostLine != 0) {
                reportDetachedAnnotation("@post", pendingPostLine);
                pendingPostLine = 0;
            }
            if (pendingPureLine != 0) {
                reportDetachedAnnotation("@pure", pendingPureLine);
                pendingPureLine = 0;
            }
            if (pendingCostLine != 0) {
                reportDetachedAnnotation("@cost", pendingCostLine);
                pendingCostLine = 0;
            }
        }

        if (*lineEnd == '\n') {
            line++;
            cursor = lineEnd + 1;
        } else {
            cursor = lineEnd;
        }
    }

    if (pendingPreLine != 0) {
        reportDetachedAnnotation("@pre", pendingPreLine);
    }
    if (pendingPostLine != 0) {
        reportDetachedAnnotation("@post", pendingPostLine);
    }
    if (pendingPureLine != 0) {
        reportDetachedAnnotation("@pure", pendingPureLine);
    }
    if (pendingCostLine != 0) {
        reportDetachedAnnotation("@cost", pendingCostLine);
    }
}

/* ast_stdlib.c: a prelude fn (try_read_file, ...) injected into this compile
 * that performs effects (D73). */
bool aetherStdlibPreludeIsEffectful(const char *name);

static int aetherIsEffectfulBuiltin(const char *name, size_t len) {
    /* Effectfulness is single-sourced in pscal-core. Use the *live* check
     * (pscalBuiltinNameEffectMaskLive), not the static-table-only
     * pscalBuiltinNameIsEffectful: the static table can't know about a
     * builtin an ext_builtins category, or -- since VM 2.0 Phase 7 -- a
     * dlopen plugin, registers at runtime with its own explicit effect
     * mask. A --ext-loaded plugin's builtins are already registered by the
     * time semantic analysis runs (registerAllBuiltins() happens in
     * main() before parsing/semantic analysis), so the live registry has
     * the answer; the static table alone would silently treat a
     * plugin-provided effectful builtin as pure, letting it escape both
     * the FX-001 fx-block fence and the @pure/ANN-001 purity check. Copy
     * the possibly non-terminated slice and delegate. */
    char buf[128];
    if (!name || len == 0 || len >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, name, len);
    buf[len] = '\0';
    return pscalBuiltinNameEffectMaskLive(buf) != FX_PURE ? 1 : 0;
}

static int expectedOpaqueArgKind(const char *name, size_t len, int *expectsKind) {
    if (!name || !expectsKind) {
        return 0;
    }
    if ((len == 9 && strncmp(name, "toon_root", len) == 0) ||
        (len == 10 && strncmp(name, "toon_close", len) == 0)) {
        *expectsKind = AETHER_HANDLE_DOC;
        return 1;
    }
    /* Memory-stream consumers: first argument must be an MStream handle.
     * The snake_case names are the Aether surface (D61); the engine
     * spellings stay so a raw lowering is still checked. */
    if ((len == 12 && strncmp(name, "mstream_text", len) == 0) ||
        (len == 12 && strncmp(name, "mstream_free", len) == 0) ||
        (len == 12 && strncmp(name, "mstream_save", len) == 0) ||
        (len == 14 && strncmp(name, "mstream_length", len) == 0) ||
        (len == 18 && strncmp(name, "mstream_write_text", len) == 0) ||
        (len == 18 && strncmp(name, "mstream_write_byte", len) == 0) ||
        (len == 13 && strncasecmp(name, "mstreambuffer", len) == 0) ||
        (len == 11 && strncasecmp(name, "mstreamfree", len) == 0) ||
        (len == 17 && strncasecmp(name, "mstreamsavetofile", len) == 0) ||
        (len == 19 && strncasecmp(name, "mstreamloadfromfile", len) == 0)) {
        *expectsKind = AETHER_HANDLE_MSTREAM;
        return 1;
    }
    if ((len == 8 && strncmp(name, "toon_key", len) == 0) ||
        (len == 11 && strncmp(name, "toon_key_or", len) == 0) ||
        (len == 12 && strncmp(name, "toon_has_key", len) == 0) ||
        (len == 7 && strncmp(name, "toon_at", len) == 0) ||
        (len == 11 && strncmp(name, "toon_has_at", len) == 0) ||
        (len == 8 && strncmp(name, "toon_len", len) == 0) ||
        (len == 9 && strncmp(name, "toon_type", len) == 0) ||
        (len == 9 && strncmp(name, "toon_free", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_text", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_int", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_real", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_bool", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_null", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_arr", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_obj", len) == 0) ||
        (len == 15 && strncmp(name, "toon_text_value", len) == 0) ||
        (len == 14 && strncmp(name, "toon_int_value", len) == 0) ||
        (len == 15 && strncmp(name, "toon_real_value", len) == 0) ||
        (len == 15 && strncmp(name, "toon_bool_value", len) == 0) ||
        (len == 15 && strncmp(name, "toon_null_value", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_text", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) ||
        (len == 12 && strncmp(name, "toon_get_int", len) == 0) ||
        (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_real", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_bool", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0)) {
        *expectsKind = AETHER_HANDLE_NODE;
        return 1;
    }
    return 0;
}

static const char *expectedSecondaryArgTypeName(const char *name, size_t len) {
    if ((len == 8 && strncmp(name, "toon_key", len) == 0) ||
        (len == 11 && strncmp(name, "toon_key_or", len) == 0) ||
        (len == 12 && strncmp(name, "toon_has_key", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_text", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) ||
        (len == 12 && strncmp(name, "toon_get_int", len) == 0) ||
        (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_real", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_bool", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0)) {
        return "Text";
    }
    if ((len == 7 && strncmp(name, "toon_at", len) == 0) ||
        (len == 11 && strncmp(name, "toon_has_at", len) == 0)) {
        return "Int";
    }
    return NULL;
}

static const char *expectedPrimaryArgTypeName(const char *name, size_t len) {
    if (len == 10 && strncmp(name, "toon_parse", len) == 0) {
        return "TextOrTOON";
    }
    if (len == 15 && strncmp(name, "toon_parse_file", len) == 0) {
        return "Text";
    }
    return NULL;
}

static int expectedOpaqueReturnKind(const char *name, size_t len, int *returnsKind) {
    if (!name || !returnsKind) {
        return 0;
    }
    if ((len == 10 && strncmp(name, "toon_parse", len) == 0) ||
        (len == 15 && strncmp(name, "toon_parse_file", len) == 0)) {
        *returnsKind = AETHER_HANDLE_DOC;
        return 1;
    }
    if ((len == 9 && strncmp(name, "toon_root", len) == 0) ||
        (len == 8 && strncmp(name, "toon_key", len) == 0) ||
        (len == 11 && strncmp(name, "toon_key_or", len) == 0) ||
        (len == 9 && strncmp(name, "toon_null", len) == 0) ||
        (len == 7 && strncmp(name, "toon_at", len) == 0)) {
        *returnsKind = AETHER_HANDLE_NODE;
        return 1;
    }
    /* Memory-stream constructors are the raw vm builtin names; VM lookup is
     * case-insensitive, so match them case-insensitively here too. */
    if ((len == 11 && strncmp(name, "mstream_new", len) == 0) ||
        (len == 17 && strncmp(name, "mstream_from_text", len) == 0) ||
        (len == 12 && strncmp(name, "mstream_load", len) == 0) ||
        (len == 13 && strncasecmp(name, "mstreamcreate", len) == 0) ||
        (len == 17 && strncasecmp(name, "mstreamfromstring", len) == 0) ||
        (len == 13 && strncasecmp(name, "socketreceive", len) == 0) ||
        (len == 14 && strncmp(name, "socket_receive", len) == 0)) {
        *returnsKind = AETHER_HANDLE_MSTREAM;
        return 1;
    }
    return 0;
}

static const char *expectedScalarReturnTypeName(const char *name, size_t len) {
    if ((len == 9 && strncmp(name, "toon_type", len) == 0) ||
        (len == 13 && strncmp(name, "toon_get_text", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) ||
        (len == 15 && strncmp(name, "toon_text_value", len) == 0)) {
        return "Text";
    }
    if ((len == 8 && strncmp(name, "toon_len", len) == 0) ||
        (len == 12 && strncmp(name, "toon_get_int", len) == 0) ||
        (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) ||
        (len == 14 && strncmp(name, "toon_int_value", len) == 0)) {
        return "Int";
    }
    if ((len == 13 && strncmp(name, "toon_get_real", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) ||
        (len == 15 && strncmp(name, "toon_real_value", len) == 0)) {
        return "Real";
    }
    if ((len == 13 && strncmp(name, "toon_get_bool", len) == 0) ||
        (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0) ||
        (len == 15 && strncmp(name, "toon_bool_value", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_text", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_int", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_real", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_bool", len) == 0) ||
        (len == 12 && strncmp(name, "toon_is_null", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_arr", len) == 0) ||
        (len == 11 && strncmp(name, "toon_is_obj", len) == 0) ||
        (len == 12 && strncmp(name, "toon_has_key", len) == 0) ||
        (len == 11 && strncmp(name, "toon_has_at", len) == 0) ||
        (len == 15 && strncmp(name, "toon_null_value", len) == 0)) {
        return "Bool";
    }
    return NULL;
}

/* A builtin whose result type the source-level handle and scalar checks
 * above read off its name, without looking for a user function of that name. */
int aetherIsNameTypedBuiltin(const char *name) {
    int kind = 0;
    if (!name) return 0;
    size_t len = strlen(name);
    return expectedOpaqueReturnKind(name, len, &kind) ||
           expectedScalarReturnTypeName(name, len) != NULL;
}

static const char *expectedTertiaryArgTypeName(const char *name, size_t len) {
    if (len == 16 && strncmp(name, "toon_get_text_or", len) == 0) {
        return "Text";
    }
    if (len == 15 && strncmp(name, "toon_get_int_or", len) == 0) {
        return "Int";
    }
    if (len == 16 && strncmp(name, "toon_get_real_or", len) == 0) {
        return "Real";
    }
    if (len == 16 && strncmp(name, "toon_get_bool_or", len) == 0) {
        return "Bool";
    }
    return NULL;
}

/* Like reportAetherError, but the caller names the diagnostic code explicitly
 * instead of relying on aetherInferDiagnosticCode's message-pattern table. Used
 * by the scalar/opaque type-checking family below so those diagnostics always
 * carry [TYPE-001]/[TOON-001] at the emission site (the inference table keeps
 * only narrow backstop patterns for them). */
static void reportAetherErrorCoded(const char *code, const char *kind, int line,
                                   const char *detail) {
    if (code) {
        fprintf(stderr,
                "%s:%d: [%s] Aether %s error: %s\n",
                g_aether_source_path ? g_aether_source_path : "<aether>",
                line,
                code,
                kind,
                detail ? detail : "unknown error");
    } else {
        fprintf(stderr,
                "%s:%d: Aether %s error: %s\n",
                g_aether_source_path ? g_aether_source_path : "<aether>",
                line,
                kind,
                detail ? detail : "unknown error");
    }
    aetherReportGuideHelp(code);
    pascal_semantic_error_count++;
}

static void reportAetherError(const char *kind, int line, const char *detail) {
    reportAetherErrorCoded(aetherInferDiagnosticCode(kind, detail), kind, line, detail);
}

/* Like reportAetherErrorCoded, but for diagnostics that should NOT fail
 * compilation -- deliberately does not touch pascal_semantic_error_count.
 * First user: ARR-001 (array-parameter mutation with no observable caller
 * effect), which is a strong "you probably meant something else" signal but
 * not something that should block a build the way a real error does (a
 * function may legitimately mutate an array parameter as pure internal
 * scratch space and never intend the caller to see it). */
static void reportAetherWarningCoded(const char *code, const char *kind, int line,
                                     const char *detail) {
    if (code) {
        fprintf(stderr,
                "%s:%d: warning: [%s] Aether %s warning: %s\n",
                g_aether_source_path ? g_aether_source_path : "<aether>",
                line,
                code,
                kind,
                detail ? detail : "unknown warning");
    } else {
        fprintf(stderr,
                "%s:%d: warning: Aether %s warning: %s\n",
                g_aether_source_path ? g_aether_source_path : "<aether>",
                line,
                kind,
                detail ? detail : "unknown warning");
    }
    aetherReportGuideHelp(code);
}

static int parseOpaqueTypeName(const char *start, const char *end, int *kind) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if ((size_t)(end - start) == 7 && strncmp(start, "ToonDoc", 7) == 0) {
        if (kind) {
            *kind = AETHER_HANDLE_DOC;
        }
        return 1;
    }
    if ((size_t)(end - start) == 8 && strncmp(start, "ToonNode", 8) == 0) {
        if (kind) {
            *kind = AETHER_HANDLE_NODE;
        }
        return 1;
    }
    if ((size_t)(end - start) == 7 && strncmp(start, "MStream", 7) == 0) {
        if (kind) {
            *kind = AETHER_HANDLE_MSTREAM;
        }
        return 1;
    }
    return 0;
}

static void validateOpaqueAssignmentLine(const char *body,
                                         const char *lineEnd,
                                         int line,
                                         const AetherOpaqueBindingTable *opaqueBindings) {
    const char *lhsStart;
    const char *lhsEnd;
    const char *rhsStart;
    const char *rhsEnd;
    const AetherOpaqueBinding *rhsBinding;
    int lhsIsDoc = 0;
    char detail[256];

    if (!body || !lineEnd || !opaqueBindings) {
        return;
    }

    if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
        const char *scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
        const char *colon;
        const char *typeStart;
        const char *typeEnd;
        const char *equals;

        scan = skipInlineSpaces(scan, lineEnd);
        lhsStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        lhsEnd = scan;
        colon = skipInlineSpaces(scan, lineEnd);
        if (lhsEnd == lhsStart || colon >= lineEnd || *colon != ':') {
            return;
        }
        typeStart = skipInlineSpaces(colon + 1, lineEnd);
        equals = typeStart;
        while (equals < lineEnd && *equals != '=') {
            equals++;
        }
        if (equals >= lineEnd || *equals != '=') {
            return;
        }
        typeEnd = equals;
        if (!parseOpaqueTypeName(typeStart, typeEnd, &lhsIsDoc)) {
            return;
        }
        rhsStart = skipInlineSpaces(equals + 1, lineEnd);
    } else {
        const AetherOpaqueBinding *lhsBinding;
        const char *scan = body;
        const char *equals;
        int rhsReturnsDoc = 0;

        lhsStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        lhsEnd = scan;
        equals = skipInlineSpaces(scan, lineEnd);
        if (lhsEnd == lhsStart || equals >= lineEnd || *equals != '=') {
            return;
        }
        lhsBinding = findOpaqueBinding(opaqueBindings, lhsStart, (size_t)(lhsEnd - lhsStart));
        if (!lhsBinding) {
            return;
        }
        lhsIsDoc = lhsBinding->kind;
        rhsStart = skipInlineSpaces(equals + 1, lineEnd);
        rhsEnd = rhsStart;
        while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
            rhsEnd++;
        }
        if (rhsEnd > rhsStart &&
            expectedOpaqueReturnKind(rhsStart, (size_t)(rhsEnd - rhsStart), &rhsReturnsDoc)) {
            if (rhsReturnsDoc != lhsIsDoc) {
                snprintf(detail,
                         sizeof(detail),
                         "binding for '%.*s' must use %s when initialized from '%.*s'.",
                         (int)(lhsEnd - lhsStart),
                         lhsStart,
                         opaqueHandleTypeName(rhsReturnsDoc),
                         (int)(rhsEnd - rhsStart),
                         rhsStart);
                reportAetherErrorCoded(opaqueHandleCode(rhsReturnsDoc), "type", line, detail);
            }
            return;
        }
    }

    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }
    rhsBinding = findOpaqueBinding(opaqueBindings, rhsStart, (size_t)(rhsEnd - rhsStart));
    if (!rhsBinding) {
        return;
    }
    if (rhsBinding->kind != lhsIsDoc) {
        snprintf(detail,
                 sizeof(detail),
                 "cannot assign %s handle '%.*s' to %s binding.",
                 opaqueHandleTypeName(rhsBinding->kind),
                 (int)(rhsEnd - rhsStart),
                 rhsStart,
                 opaqueHandleTypeName(lhsIsDoc));
        reportAetherErrorCoded(opaqueHandleCode(rhsBinding->kind), "type", line, detail);
    }
}

static void validateOpaqueReturnBindingLine(const char *body, const char *lineEnd, int line) {
    const char *scan;
    const char *nameEnd;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    const char *equals;
    const char *rhsStart;
    const char *rhsEnd;
    int lhsIsDoc = 0;
    int lhsIsOpaque = 0;
    int rhsIsDoc = 0;
    char detail[256];

    if (!body || !lineEnd) {
        return;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return;
    }

    scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    scan = skipInlineSpaces(scan, lineEnd);
    while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
        scan++;
    }
    nameEnd = scan;
    colon = skipInlineSpaces(scan, lineEnd);
    if (colon >= lineEnd || *colon != ':') {
        return;
    }
    typeStart = skipInlineSpaces(colon + 1, lineEnd);
    equals = typeStart;
    while (equals < lineEnd && *equals != '=') {
        equals++;
    }
    if (equals >= lineEnd || *equals != '=') {
        return;
    }
    typeEnd = equals;
    lhsIsOpaque = parseOpaqueTypeName(typeStart, typeEnd, &lhsIsDoc);

    rhsStart = skipInlineSpaces(equals + 1, lineEnd);
    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }

    if (!expectedOpaqueReturnKind(rhsStart, (size_t)(rhsEnd - rhsStart), &rhsIsDoc)) {
        return;
    }

    if (!lhsIsOpaque) {
        snprintf(detail,
                 sizeof(detail),
                 "binding for '%.*s' must use %s when initialized from '%.*s'.",
                 (int)(nameEnd - skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd)),
                 skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd),
                 opaqueHandleTypeName(rhsIsDoc),
                 (int)(rhsEnd - rhsStart),
                 rhsStart);
        reportAetherErrorCoded(opaqueHandleCode(rhsIsDoc), "type", line, detail);
        return;
    }

    if (lhsIsDoc != rhsIsDoc) {
        snprintf(detail,
                 sizeof(detail),
                 "binding for '%.*s' must use %s when initialized from '%.*s'.",
                 (int)(nameEnd - skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd)),
                 skipInlineSpaces(body + (startsWithWord(body, lineEnd, "const") ? 5 : 3), lineEnd),
                 opaqueHandleTypeName(rhsIsDoc),
                 (int)(rhsEnd - rhsStart),
                 rhsStart);
        reportAetherErrorCoded(opaqueHandleCode(rhsIsDoc), "type", line, detail);
    }
}

/* True when the call that starts at `nameEnd` (a callee name, then its
 * argument list) is the whole right-hand side: only `;`, a `//` comment or the
 * end of the line follows it. `toon_int_value(c) * 1.0` is not, so its type is
 * the expression's, which this line scan cannot know; the AST checks own it. */
static int scalarCallIsWholeRhs(const char *nameEnd, const char *lineEnd) {
    const char *scan = skipInlineSpaces(nameEnd, lineEnd);
    int depth = 0;

    if (scan < lineEnd && *scan == '(') {
        while (scan < lineEnd) {
            if (*scan == '"' || *scan == '\'') {
                int ignoredLine = 0;
                scan = skipQuotedString(scan, &ignoredLine);
                if (scan > lineEnd) {
                    return 0;
                }
                continue;
            }
            if (*scan == '(') {
                depth++;
            } else if (*scan == ')' && --depth == 0) {
                scan++;
                break;
            }
            scan++;
        }
        if (depth != 0) {
            return 0;
        }
        scan = skipInlineSpaces(scan, lineEnd);
    }
    return scan >= lineEnd || *scan == ';' ||
           (*scan == '/' && scan + 1 < lineEnd && scan[1] == '/');
}

static void validateScalarReturnBindingLine(const char *body,
                                            const char *lineEnd,
                                            int line,
                                            const AetherScalarBindingTable *scalarBindings) {
    const char *scan;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    const char *equals;
    const char *rhsStart;
    const char *rhsEnd;
    const char *expectedType;
    const AetherScalarBinding *lhsBinding = NULL;
    char detail[256];

    if (!body || !lineEnd) {
        return;
    }
    if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
        scan = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
        scan = skipInlineSpaces(scan, lineEnd);
        nameStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        nameEnd = scan;
        colon = skipInlineSpaces(scan, lineEnd);
        if (nameEnd == nameStart || colon >= lineEnd || *colon != ':') {
            return;
        }
        typeStart = skipInlineSpaces(colon + 1, lineEnd);
        equals = typeStart;
        while (equals < lineEnd && *equals != '=') {
            equals++;
        }
        if (equals >= lineEnd || *equals != '=') {
            return;
        }
        typeEnd = equals;
        while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
            typeEnd--;
        }
    } else {
        const char *equals;

        scan = body;
        nameStart = scan;
        while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
            scan++;
        }
        nameEnd = scan;
        equals = skipInlineSpaces(scan, lineEnd);
        if (nameEnd == nameStart || equals >= lineEnd || *equals != '=') {
            return;
        }
        lhsBinding = findScalarBinding(scalarBindings, nameStart, (size_t)(nameEnd - nameStart));
        if (!lhsBinding) {
            return;
        }
        typeStart = lhsBinding->typeName;
        typeEnd = lhsBinding->typeName + strlen(lhsBinding->typeName);
        equals = skipInlineSpaces(scan, lineEnd);
        rhsStart = skipInlineSpaces(equals + 1, lineEnd);
        goto check_rhs;
    }

    rhsStart = skipInlineSpaces(equals + 1, lineEnd);
check_rhs:
    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }
    expectedType = expectedScalarReturnTypeName(rhsStart, (size_t)(rhsEnd - rhsStart));
    if (!expectedType || !scalarCallIsWholeRhs(rhsEnd, lineEnd)) {
        return;
    }
    /* An Int result widens into a Real binding, as `let x: Real = n;` does. */
    if ((size_t)(typeEnd - typeStart) == 4 && strncmp(typeStart, "Real", 4) == 0 &&
        strcmp(expectedType, "Int") == 0) {
        return;
    }
    if ((size_t)(typeEnd - typeStart) != strlen(expectedType) ||
        strncmp(typeStart, expectedType, strlen(expectedType)) != 0) {
        snprintf(detail,
                 sizeof(detail),
                 "binding for '%.*s' must use %s when initialized from '%.*s'.",
                 (int)(nameEnd - nameStart),
                 nameStart,
                 expectedType,
                 (int)(rhsEnd - rhsStart),
                 rhsStart);
        reportAetherErrorCoded("TYPE-001", "type", line, detail);
    }
}

/* Int <- Real and Real <- Int are not type errors. Real -> Int truncates, and
 * the AST pass warns NARROW-001 on it (aetherWalkNarrowing), exactly as it does
 * for a Real literal or a Real-returning call; Int -> Real widens. Only an
 * incompatible pair (Text, Bool, ...) is TYPE-001. */
static int scalarAssignPairIsNumeric(const char *lhsType, const char *rhsType) {
    return lhsType && rhsType &&
           ((strcmp(lhsType, "Int") == 0 && strcmp(rhsType, "Real") == 0) ||
            (strcmp(lhsType, "Real") == 0 && strcmp(rhsType, "Int") == 0));
}

static void validateScalarAssignmentLine(const char *body,
                                         const char *lineEnd,
                                         int line,
                                         const AetherScalarBindingTable *scalarBindings) {
    const char *scan;
    const char *lhsStart;
    const char *lhsEnd;
    const char *equals;
    const char *rhsStart;
    const char *rhsEnd;
    const char *rhsTail;
    const AetherScalarBinding *lhsBinding;
    const AetherScalarBinding *rhsBinding;
    const char *rhsExprType;
    char detail[256];

    if (!body || !lineEnd || !scalarBindings) {
        return;
    }
    if (startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const")) {
        return;
    }

    scan = body;
    lhsStart = scan;
    while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
        scan++;
    }
    lhsEnd = scan;
    equals = skipInlineSpaces(scan, lineEnd);
    if (lhsEnd == lhsStart || equals >= lineEnd || *equals != '=') {
        return;
    }

    lhsBinding = findScalarBinding(scalarBindings, lhsStart, (size_t)(lhsEnd - lhsStart));
    if (!lhsBinding) {
        return;
    }

    rhsStart = skipInlineSpaces(equals + 1, lineEnd);
    rhsEnd = rhsStart;
    while (rhsEnd < lineEnd && (isalnum((unsigned char)*rhsEnd) || *rhsEnd == '_')) {
        rhsEnd++;
    }
    if (rhsEnd == rhsStart) {
        return;
    }

    rhsTail = skipInlineSpaces(rhsEnd, lineEnd);
    if (rhsTail < lineEnd && *rhsTail != ';') {
        rhsExprType = inferScalarExpressionTypeName(rhsStart, lineEnd, scalarBindings);
        if (!rhsExprType) {
            return;
        }
        if (strcmp(lhsBinding->typeName, rhsExprType) == 0 ||
            scalarAssignPairIsNumeric(lhsBinding->typeName, rhsExprType)) {
            return;
        }

        snprintf(detail,
                 sizeof(detail),
                 "cannot assign %s expression to %s binding '%.*s'.",
                 rhsExprType,
                 lhsBinding->typeName,
                 (int)(lhsEnd - lhsStart),
                 lhsStart);
        reportAetherErrorCoded("TYPE-001", "type", line, detail);
        return;
    }

    rhsBinding = findScalarBinding(scalarBindings, rhsStart, (size_t)(rhsEnd - rhsStart));
    if (!rhsBinding) {
        rhsExprType = inferScalarExpressionTypeName(rhsStart, lineEnd, scalarBindings);
        if (!rhsExprType) {
            return;
        }
        if (strcmp(lhsBinding->typeName, rhsExprType) == 0 ||
            scalarAssignPairIsNumeric(lhsBinding->typeName, rhsExprType)) {
            return;
        }

        snprintf(detail,
                 sizeof(detail),
                 "cannot assign %s expression to %s binding '%.*s'.",
                 rhsExprType,
                 lhsBinding->typeName,
                 (int)(lhsEnd - lhsStart),
                 lhsStart);
        reportAetherErrorCoded("TYPE-001", "type", line, detail);
        return;
    }
    if (strcmp(lhsBinding->typeName, rhsBinding->typeName) == 0 ||
        scalarAssignPairIsNumeric(lhsBinding->typeName, rhsBinding->typeName)) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "cannot assign %s binding '%.*s' to %s binding '%.*s'.",
             rhsBinding->typeName,
             (int)(rhsEnd - rhsStart),
             rhsStart,
             lhsBinding->typeName,
             (int)(lhsEnd - lhsStart),
             lhsStart);
    reportAetherErrorCoded("TYPE-001", "type", line, detail);
}

static int isArithmeticChar(char ch) {
    return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%';
}

static const char *previousSignificantChar(const char *lineStart, const char *cursor) {
    const char *scan = cursor;

    while (scan > lineStart) {
        scan--;
        if (!isspace((unsigned char)*scan)) {
            return scan;
        }
    }
    return NULL;
}

static const char *nextSignificantChar(const char *cursor, const char *lineEnd) {
    const char *scan = cursor;

    while (scan < lineEnd) {
        if (!isspace((unsigned char)*scan)) {
            return scan;
        }
        scan++;
    }
    return NULL;
}

static void validateOpaqueCallKind(const char *callName,
                                   size_t callNameLen,
                                   const char *openParen,
                                   const char *lineEnd,
                                   int line,
                                   const AetherOpaqueBindingTable *opaqueBindings) {
    const char *argStart;
    const char *argEnd;
    const AetherOpaqueBinding *binding;
    int expectsDoc = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !opaqueBindings) {
        return;
    }
    if (!expectedOpaqueArgKind(callName, callNameLen, &expectsDoc)) {
        return;
    }
    argStart = skipInlineSpaces(openParen + 1, lineEnd);
    argEnd = argStart;
    while (argEnd < lineEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
        argEnd++;
    }
    if (argEnd == argStart) {
        return;
    }
    binding = findOpaqueBinding(opaqueBindings, argStart, (size_t)(argEnd - argStart));
    if (!binding) {
        return;
    }
    if (binding->kind != expectsDoc) {
        snprintf(detail,
                 sizeof(detail),
                 "call to '%.*s' expects a %s handle, but '%.*s' is %s.",
                 (int)callNameLen,
                 callName,
                 opaqueHandleTypeName(expectsDoc),
                 (int)(argEnd - argStart),
                 argStart,
                 opaqueHandleTypeName(binding->kind));
        reportAetherErrorCoded(opaqueHandleCode(expectsDoc), "type", line, detail);
    }
}

/* LEN-001 at compile time: `len(n)` / `length(n)` on a variable declared as an
 * opaque handle. It compiled and stopped at run time with the engine's
 * uncoded "Length expects a string or array argument."; the type oracle sees
 * a handle variable only as the Int it is lowered to, so the check lives here
 * with the handle bindings. */
static void validateLengthOfHandle(const char *callName,
                                   size_t callNameLen,
                                   const char *openParen,
                                   const char *lineEnd,
                                   int line,
                                   const AetherOpaqueBindingTable *opaqueBindings) {
    const char *argStart;
    const char *argEnd;
    const char *after;
    const AetherOpaqueBinding *binding;
    char detail[320];

    if (!callName || !openParen || !lineEnd || !opaqueBindings) {
        return;
    }
    if (!((callNameLen == 3 && strncmp(callName, "len", 3) == 0) ||
          (callNameLen == 6 && strncmp(callName, "length", 6) == 0))) {
        return;
    }
    argStart = skipInlineSpaces(openParen + 1, lineEnd);
    argEnd = argStart;
    while (argEnd < lineEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
        argEnd++;
    }
    after = skipInlineSpaces(argEnd, lineEnd);
    if (argEnd == argStart || after >= lineEnd || *after != ')') {
        return;
    }
    binding = findOpaqueBinding(opaqueBindings, argStart, (size_t)(argEnd - argStart));
    if (!binding) {
        return;
    }
    if (binding->kind == AETHER_HANDLE_MSTREAM) {
        snprintf(detail, sizeof(detail),
                 "`%.*s` counts an array, a Text or a map, and '%.*s' is an MStream: its size "
                 "in bytes is `mstream_length(%.*s)`.",
                 (int)callNameLen, callName, (int)(argEnd - argStart), argStart,
                 (int)(argEnd - argStart), argStart);
    } else if (binding->kind == AETHER_HANDLE_DOC) {
        snprintf(detail, sizeof(detail),
                 "`%.*s` counts an array, a Text or a map, and '%.*s' is a ToonDoc: count its "
                 "root array with `toon_len(toon_root(%.*s))`.",
                 (int)callNameLen, callName, (int)(argEnd - argStart), argStart,
                 (int)(argEnd - argStart), argStart);
    } else {
        snprintf(detail, sizeof(detail),
                 "`%.*s` counts an array, a Text or a map, and '%.*s' is a ToonNode: count a "
                 "TOON array with `toon_len(%.*s)`.",
                 (int)callNameLen, callName, (int)(argEnd - argStart), argStart,
                 (int)(argEnd - argStart), argStart);
    }
    reportAetherErrorCoded(opaqueHandleCode(binding->kind), "type", line, detail);
}

static void validateSecondaryHelperArgType(const char *callName,
                                           size_t callNameLen,
                                           const char *openParen,
                                           const char *lineEnd,
                                           int line,
                                           const AetherScalarBindingTable *scalarBindings) {
    const char *expectedType;
    const char *cursor;
    const char *secondStart;
    const char *secondEnd;
    const AetherScalarBinding *binding;
    int depth = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !scalarBindings) {
        return;
    }
    expectedType = expectedSecondaryArgTypeName(callName, callNameLen);
    if (!expectedType) {
        return;
    }

    cursor = openParen + 1;
    while (cursor < lineEnd && *cursor) {
        if (*cursor == '"' || *cursor == '\'') {
            int ignoredLine = line;
            cursor = skipQuotedString(cursor, &ignoredLine);
            continue;
        }
        if (*cursor == '(') {
            depth++;
        } else if (*cursor == ')') {
            if (depth == 0) {
                return;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0) {
            cursor++;
            break;
        }
        cursor++;
    }
    if (cursor >= lineEnd) {
        return;
    }

    secondStart = skipInlineSpaces(cursor, lineEnd);
    secondEnd = secondStart;
    while (secondEnd < lineEnd && (isalnum((unsigned char)*secondEnd) || *secondEnd == '_')) {
        secondEnd++;
    }
    if (secondEnd == secondStart) {
        return;
    }

    binding = findScalarBinding(scalarBindings, secondStart, (size_t)(secondEnd - secondStart));
    if (!binding) {
        return;
    }
    if (strcmp(binding->typeName, expectedType) == 0) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "call to '%.*s' expects a %s second argument, but '%.*s' is %s.",
             (int)callNameLen,
             callName,
             expectedType,
             (int)(secondEnd - secondStart),
             secondStart,
             binding->typeName);
    reportAetherErrorCoded("TOON-001", "type", line, detail);
}

static void validateTertiaryHelperArgType(const char *callName,
                                          size_t callNameLen,
                                          const char *openParen,
                                          const char *lineEnd,
                                          int line,
                                          const AetherScalarBindingTable *scalarBindings) {
    const char *expectedType;
    const char *cursor;
    const char *thirdStart = NULL;
    const char *thirdEnd = NULL;
    const AetherScalarBinding *binding;
    int depth = 0;
    int commaCount = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !scalarBindings) {
        return;
    }
    expectedType = expectedTertiaryArgTypeName(callName, callNameLen);
    if (!expectedType) {
        return;
    }

    cursor = openParen + 1;
    while (cursor < lineEnd && *cursor) {
        if (*cursor == '"' || *cursor == '\'') {
            int ignoredLine = line;
            cursor = skipQuotedString(cursor, &ignoredLine);
            continue;
        }
        if (*cursor == '(') {
            depth++;
        } else if (*cursor == ')') {
            if (depth == 0) {
                if (thirdStart) {
                    thirdEnd = cursor;
                }
                break;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0) {
            commaCount++;
            if (commaCount == 2) {
                thirdStart = skipInlineSpaces(cursor + 1, lineEnd);
            } else if (commaCount > 2) {
                break;
            }
        }
        cursor++;
    }
    if (!thirdStart || !thirdEnd || thirdStart >= thirdEnd) {
        return;
    }

    while (thirdEnd > thirdStart && isspace((unsigned char)thirdEnd[-1])) {
        thirdEnd--;
    }
    while (thirdStart < thirdEnd && isspace((unsigned char)*thirdStart)) {
        thirdStart++;
    }
    if (thirdEnd == thirdStart) {
        return;
    }

    {
        const char *identEnd = thirdStart;
        while (identEnd < thirdEnd && (isalnum((unsigned char)*identEnd) || *identEnd == '_')) {
            identEnd++;
        }
        if (identEnd != thirdEnd) {
            return;
        }
        binding = findScalarBinding(scalarBindings, thirdStart, (size_t)(thirdEnd - thirdStart));
    }
    if (!binding) {
        return;
    }
    if (strcmp(binding->typeName, expectedType) == 0) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "call to '%.*s' expects a %s third argument, but '%.*s' is %s.",
             (int)callNameLen,
             callName,
             expectedType,
             (int)(thirdEnd - thirdStart),
             thirdStart,
             binding->typeName);
    reportAetherErrorCoded("TOON-001", "type", line, detail);
}

static void validatePrimaryHelperArgType(const char *callName,
                                         size_t callNameLen,
                                         const char *openParen,
                                         const char *lineEnd,
                                         int line,
                                         const AetherScalarBindingTable *scalarBindings) {
    const char *expectedType;
    const char *argStart;
    const char *argEnd;
    const AetherScalarBinding *binding;
    const char *expectedLabel;
    int matches = 0;
    char detail[256];

    if (!callName || !openParen || !lineEnd || !scalarBindings) {
        return;
    }
    expectedType = expectedPrimaryArgTypeName(callName, callNameLen);
    if (!expectedType) {
        return;
    }

    argStart = skipInlineSpaces(openParen + 1, lineEnd);
    argEnd = argStart;
    while (argEnd < lineEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
        argEnd++;
    }
    if (argEnd == argStart) {
        return;
    }

    binding = findScalarBinding(scalarBindings, argStart, (size_t)(argEnd - argStart));
    if (!binding) {
        return;
    }

    if (strcmp(expectedType, "TextOrTOON") == 0) {
        matches = strcmp(binding->typeName, "Text") == 0 || strcmp(binding->typeName, "TOON") == 0;
        expectedLabel = "Text or TOON";
    } else {
        matches = strcmp(binding->typeName, expectedType) == 0;
        expectedLabel = expectedType;
    }
    if (matches) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "call to '%.*s' expects a %s first argument, but '%.*s' is %s.",
             (int)callNameLen,
             callName,
             expectedLabel,
             (int)(argEnd - argStart),
             argStart,
             binding->typeName);
    reportAetherErrorCoded("TOON-001", "type", line, detail);
}

static void validateAetherSource(const char *source,
                                 const AetherOpaqueBindingTable *opaqueBindings,
                                 const AetherScalarBindingTable *scalarBindings,
                                 const int *lineFnIds, size_t lineCount, int fnId) {
    /* Textual TOON handle-typing / scalar-flow pass. The fx effect fence and
     * @pure purity checks that used to live in this line scan moved to the AST
     * walk (aetherValidateEffectsAndPurity); this scan keeps only the checks
     * that are still line-oriented by design. Runs once per scope (top level +
     * each function) against that scope's tables; lines belonging to other
     * scopes are skipped (they get their own pass). */
    const char *cursor = source;
    int line = 1;

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        const char *scan;
        const char *lastWord = NULL;
        size_t lastWordLen = 0;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        if (lineOutsideScope(lineFnIds, lineCount, (size_t)(line - 1), fnId)) {
            if (*lineEnd == '\n') {
                line++;
                cursor = lineEnd + 1;
            } else {
                cursor = lineEnd;
            }
            continue;
        }
        body = skipInlineSpaces(lineStart, lineEnd);
        validateOpaqueAssignmentLine(body, lineEnd, line, opaqueBindings);
        validateOpaqueReturnBindingLine(body, lineEnd, line);
        validateScalarReturnBindingLine(body, lineEnd, line, scalarBindings);
        validateScalarAssignmentLine(body, lineEnd, line, scalarBindings);

        scan = lineStart;
        while (scan < lineEnd) {
            if (scan[0] == '/' && scan + 1 < lineEnd && scan[1] == '/') {
                break;
            }
            if (*scan == '"' || *scan == '\'') {
                int ignoredLine = line;
                scan = skipQuotedString(scan, &ignoredLine);
                continue;
            }
            if (*scan == '{' || *scan == '}') {
                lastWord = NULL;
                lastWordLen = 0;
                scan++;
                continue;
            }
            if (isalpha((unsigned char)*scan) || *scan == '_') {
                const char *start = scan;
                const char *afterName;
                size_t nameLen;

                scan++;
                while (scan < lineEnd && (isalnum((unsigned char)*scan) || *scan == '_')) {
                    scan++;
                }
                afterName = skipInlineSpaces(scan, lineEnd);
                nameLen = (size_t)(scan - start);

                if (*afterName == '(' &&
                    !(lastWordLen == 2 && strncmp(lastWord, "fn", 2) == 0)) {
                    validateOpaqueCallKind(start,
                                           nameLen,
                                           afterName,
                                           lineEnd,
                                           line,
                                           opaqueBindings);
                    validateLengthOfHandle(start,
                                           nameLen,
                                           afterName,
                                           lineEnd,
                                           line,
                                           opaqueBindings);
                    validatePrimaryHelperArgType(start,
                                                 nameLen,
                                                 afterName,
                                                 lineEnd,
                                                 line,
                                                 scalarBindings);
                    validateSecondaryHelperArgType(start,
                                                   nameLen,
                                                   afterName,
                                                   lineEnd,
                                                   line,
                                                   scalarBindings);
                    validateTertiaryHelperArgType(start,
                                                  nameLen,
                                                  afterName,
                                                  lineEnd,
                                                  line,
                                                  scalarBindings);
                }

                /* `b.orders` names a field, not the local handle `orders`;
                 * `0..orders` is a range bound and still checked. */
                const char *prevSig = previousSignificantChar(lineStart, start);
                int isFieldName = prevSig && *prevSig == '.' &&
                                  !(prevSig > lineStart && prevSig[-1] == '.');

                if (!isFieldName && findOpaqueBinding(opaqueBindings, start, nameLen) != NULL) {
                    const char *nextSig = nextSignificantChar(scan, lineEnd);
                    char detail[256];

                    if ((prevSig && isArithmeticChar(*prevSig) &&
                         !(prevSig > lineStart && prevSig[-1] == '=')) ||
                        (nextSig && isArithmeticChar(*nextSig))) {
                        const AetherOpaqueBinding *arith =
                            findOpaqueBinding(opaqueBindings, start, nameLen);
                        int arithKind = arith ? arith->kind : AETHER_HANDLE_NODE;
                        /* Keep the historical "TOON" label for the doc/node pair. */
                        snprintf(detail,
                                 sizeof(detail),
                                 "opaque %s handle '%.*s' cannot be used in arithmetic expressions.",
                                 arithKind == AETHER_HANDLE_MSTREAM ? "MStream" : "TOON",
                                 (int)nameLen,
                                 start);
                        reportAetherErrorCoded(opaqueHandleCode(arithKind), "type", line, detail);
                    }
                }

                lastWord = start;
                lastWordLen = nameLen;
                continue;
            }

            if (!strchr(":@", *scan)) {
                lastWord = NULL;
                lastWordLen = 0;
            }
            scan++;
        }

        if (*lineEnd == '\n') {
            line++;
            cursor = lineEnd + 1;
        } else {
            cursor = lineEnd;
        }
    }
}

/* NAME-001 value-binding checks (W4-22, decision D17).
 *
 * Aether value names ignore case (the shared engine folds them), so a second
 * binding that differs only in case is the same variable: `let Count` in a
 * loop overwrote `count`, `fn check(limit: Int) { ret LIMIT; }` returned the
 * argument, and a top-level `let Total` beside `let total` only failed at run
 * time. A text scan over the sanitized source (comments neutralized; strings
 * are skipped here) tracks every value binding with its brace depth:
 *
 *   - a `let`/`const` that matches a binding of the same depth, in any case
 *     (a second top-level `let`, or a body-level re-`let` of a parameter);
 *   - a local, parameter or loop variable that matches a top-level const or
 *     `let`, or a const/let exported by a `use`d module, in any case;
 *   - a range/foreach loop variable that matches any visible local or
 *     parameter of the same function (`let i = 0; loop i in 0..3 {}` left i
 *     at 0);
 *   - interim, until the engine compiles an initializer before activating the
 *     new slot (W5): `let x = <expr reading x>` while an outer x exists, which
 *     reads the new, still-empty slot (`let n: Int = n * 2` gave 0).
 *
 * Shadowing in a nested block and reuse across sibling scopes stay legal, and
 * so do type/value pairs (`let board: Board`) and fn/value pairs: types and
 * functions are not value bindings. */
enum {
    AETHER_DECL_LOCAL,
    AETHER_DECL_PARAM,
    AETHER_DECL_LOOP,
    AETHER_DECL_GLOBAL,       /* top-level let */
    AETHER_DECL_GLOBAL_CONST, /* top-level const */
    AETHER_DECL_IMPORT        /* const/let exported by a `use`d module */
};

typedef struct {
    char *name;
    int depth;
    int line;
    int kind;
} AetherLocalDecl;

typedef struct {
    AetherLocalDecl *items;
    size_t count;
    size_t cap;
} AetherDeclList;

static int declListPush(AetherDeclList *list, const char *start, const char *end,
                        int depth, int line, int kind) {
    char *name = dupRange(start, end);
    if (!name) {
        return 0;
    }
    if (list->count == list->cap) {
        size_t newCap = list->cap ? list->cap * 2 : 16;
        AetherLocalDecl *grown =
            (AetherLocalDecl *)realloc(list->items, newCap * sizeof(*grown));
        if (!grown) {
            free(name);
            return 0;
        }
        list->items = grown;
        list->cap = newCap;
    }
    list->items[list->count].name = name;
    list->items[list->count].depth = depth;
    list->items[list->count].line = line;
    list->items[list->count].kind = kind;
    list->count++;
    return 1;
}

static void declListPopAbove(AetherDeclList *list, int depth) {
    while (list->count > 0 && list->items[list->count - 1].depth > depth) {
        free(list->items[list->count - 1].name);
        list->count--;
    }
}

static void declListFree(AetherDeclList *list) {
    size_t k;
    for (k = 0; k < list->count; k++) {
        free(list->items[k].name);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

/* Most recent binding named `name` (any case) with minDepth <= depth <=
 * maxDepth, or NULL. */
static const AetherLocalDecl *declListFind(const AetherDeclList *list, const char *name,
                                           int minDepth, int maxDepth) {
    size_t k = list->count;
    while (k > 0) {
        const AetherLocalDecl *d = &list->items[--k];
        if (d->depth >= minDepth && d->depth <= maxDepth && strcasecmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

static int isIdentStartChar(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int isIdentChar(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Does [start, end) read the value `name` (any case)? A word preceded by `.`
 * (a field) or followed by `(` (a call) is not a value read. */
static int textReadsName(const char *start, const char *end, const char *name) {
    size_t len = strlen(name);
    const char *c = start;
    while (c < end) {
        if (*c == '"' || *c == '\'') {
            const char *after = skipQuotedString(c, NULL);
            c = (after > c && after <= end) ? after : end;
            continue;
        }
        if (isIdentStartChar(*c) && (c == start || !isIdentChar(c[-1]))) {
            const char *w = c;
            while (c < end && isIdentChar(*c)) {
                c++;
            }
            if ((size_t)(c - w) == len && strncasecmp(w, name, len) == 0 &&
                !(w > start && w[-1] == '.')) {
                const char *after = c;
                while (after < end && (*after == ' ' || *after == '\t')) {
                    after++;
                }
                if (after >= end || *after != '(') {
                    return 1;
                }
            }
            continue;
        }
        c++;
    }
    return 0;
}

static int g_nameCollisionReports = 0;

static void reportNameCollision(int line, const char *detail, const char *hint) {
    /* D5: keep a run's diagnostics short; the first collisions are enough. */
    if (g_nameCollisionReports >= 3) {
        pascal_semantic_error_count++;
        return;
    }
    g_nameCollisionReports++;
    fprintf(stderr, "%s:%d: [NAME-001] Aether redeclaration error: %s\n",
            g_aether_source_path ? g_aether_source_path : "<aether>", line, detail);
    if (hint && *hint) {
        fprintf(stderr, "hint: %s\n", hint);
    }
    aetherReportGuideHelp("NAME-001");
    pascal_semantic_error_count++;
}

static void collectImportedValueName(void *ctx, const char *name, const char *aetherType,
                                     int isFunction) {
    AetherDeclList *list = (AetherDeclList *)ctx;
    (void)aetherType;
    if (isFunction || !name || !*name || strchr(name, '.')) {
        return;
    }
    declListPush(list, name, name + strlen(name), 0, 0, AETHER_DECL_IMPORT);
}

/* Top-level `let`/`const` names (brace depth 0), with their lines, so a local
 * is checked against a global declared later in the file too. */
static void collectTopLevelValueNames(const char *source, AetherDeclList *globals) {
    const char *cursor = source;
    int line = 1;
    int depth = 0;
    int atStmtStart = 1;
    while (*cursor) {
        char c = *cursor;
        if (c == '\n') {
            line++;
            cursor++;
            atStmtStart = 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char *after = skipQuotedString(cursor, NULL);
            cursor = (after > cursor) ? after : cursor + 1;
            atStmtStart = 0;
            continue;
        }
        if (c == '{' || c == '}' || c == ';') {
            if (c == '{') depth++;
            if (c == '}' && depth > 0) depth--;
            cursor++;
            atStmtStart = 1;
            continue;
        }
        if (isspace((unsigned char)c)) {
            cursor++;
            continue;
        }
        if (atStmtStart && depth == 0) {
            const char *lineEnd = cursor;
            while (*lineEnd && *lineEnd != '\n') lineEnd++;
            int isConst = startsWithWord(cursor, lineEnd, "const");
            if (isConst || startsWithWord(cursor, lineEnd, "let")) {
                const char *scan = skipInlineSpaces(cursor + (isConst ? 5 : 3), lineEnd);
                if (startsWithWord(scan, lineEnd, "mut")) {
                    scan = skipInlineSpaces(scan + 3, lineEnd);
                }
                const char *nameStart = scan;
                while (scan < lineEnd && isIdentChar(*scan)) scan++;
                if (scan > nameStart) {
                    declListPush(globals, nameStart, scan, 0, line,
                                 isConst ? AETHER_DECL_GLOBAL_CONST : AETHER_DECL_GLOBAL);
                }
                cursor = scan;
                atStmtStart = 0;
                continue;
            }
        }
        atStmtStart = 0;
        cursor++;
    }
}

/* The top-level const/let, or imported const, that `name` collides with. */
static const AetherLocalDecl *findGlobalValue(const AetherDeclList *globals, const char *name,
                                              int line) {
    size_t k;
    for (k = 0; k < globals->count; k++) {
        const AetherLocalDecl *g = &globals->items[k];
        if (g->line != line && strcasecmp(g->name, name) == 0) {
            return g;
        }
    }
    return NULL;
}

static void describeGlobal(const AetherLocalDecl *g, char *out, size_t outSize) {
    if (g->kind == AETHER_DECL_IMPORT) {
        snprintf(out, outSize, "the imported const '%s'", g->name);
    } else {
        snprintf(out, outSize, "the top-level %s '%s' (line %d)",
                 g->kind == AETHER_DECL_GLOBAL_CONST ? "const" : "let", g->name, g->line);
    }
}

/* `fn name(a: T, b: U)`: push each parameter at the body's depth. `open`
 * points just past `fn`. A parameter matching a global is reported here. */
static void scanFnParams(const char *open, int bodyDepth, int line,
                         AetherDeclList *decls, const AetherDeclList *globals) {
    const char *c = open;
    while (*c && *c != '(' && *c != '{' && *c != ';') c++;
    if (*c != '(') {
        return;
    }
    c++;
    int parens = 1;
    while (*c && parens > 0) {
        if (*c == '"' || *c == '\'') {
            const char *after = skipQuotedString(c, NULL);
            c = (after > c) ? after : c + 1;
            continue;
        }
        if (*c == '(' || *c == '[') { parens++; c++; continue; }
        if (*c == ')' || *c == ']') { parens--; c++; continue; }
        if (parens == 1 && isIdentStartChar(*c) && !isIdentChar(c[-1])) {
            const char *w = c;
            while (*c && isIdentChar(*c)) c++;
            const char *after = c;
            while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r') after++;
            if (*after == ':' && after[1] != ':') {
                char *name = dupRange(w, c);
                if (name) {
                    const AetherLocalDecl *g = findGlobalValue(globals, name, -1);
                    if (g) {
                        char what[160], detail[320];
                        describeGlobal(g, what, sizeof(what));
                        snprintf(detail, sizeof(detail),
                                 "parameter '%s' is the same name as %s: %sa parameter "
                                 "must not reuse a const or global name.", name, what,
                                 strcmp(name, g->name) ? "two names may not differ only by case, and " : "");
                        reportNameCollision(line, detail, "rename the parameter.");
                    }
                    free(name);
                }
                declListPush(decls, w, c, bodyDepth, line, AETHER_DECL_PARAM);
            }
            continue;
        }
        c++;
    }
}

/* A quantifier's bound name (D58) against the visible locals, parameters and
 * globals: the range/foreach loop-variable rule. Not pushed: it is visible
 * only in its condition. */
static void checkQuantifierName(const char *start, const char *end, int line, int depth,
                                const AetherDeclList *decls, const AetherDeclList *globals) {
    char *name = dupRange(start, end);
    if (!name) return;
    const AetherLocalDecl *outer = declListFind(decls, name, 0, depth);
    const AetherLocalDecl *g = outer ? NULL : findGlobalValue(globals, name, line);
    char detail[320];
    if (outer) {
        snprintf(detail, sizeof(detail),
                 "the `all`/`any` variable '%s' reuses the name of '%s' (line %d); the "
                 "quantifier gets its own '%s'.", name, outer->name, outer->line, name);
        reportNameCollision(line, detail, "pick a new name for the quantifier variable.");
    } else if (g) {
        char what[160];
        describeGlobal(g, what, sizeof(what));
        snprintf(detail, sizeof(detail),
                 "the `all`/`any` variable '%s' is the same name as %s.", name, what);
        reportNameCollision(line, detail, "pick a new name for the quantifier variable.");
    }
    free(name);
}

static void validateNoDuplicateLocals(const char *source) {
    const char *cursor = source;
    int line = 1;
    int depth = 0;
    int atStmtStart = 1;
    AetherDeclList decls = {0};
    AetherDeclList globals = {0};
    AetherDeclList contractQuants = {0}; /* quantifier names in @pre/@post */

    if (!source) {
        return;
    }
    g_nameCollisionReports = 0;
    collectTopLevelValueNames(source, &globals);
    aetherAstCollectImportedTypes(source, g_aether_source_path, collectImportedValueName,
                                  &globals);

    while (*cursor) {
        char c = *cursor;

        if (c == '\n') {
            line++;
            cursor++;
            atStmtStart = 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char *after = skipQuotedString(cursor, NULL);
            cursor = (after > cursor) ? after : cursor + 1;
            atStmtStart = 0;
            continue;
        }
        if (c == '{') {
            depth++;
            cursor++;
            atStmtStart = 1;
            continue;
        }
        if (c == '}') {
            if (depth > 0) {
                depth--;
            }
            declListPopAbove(&decls, depth);
            cursor++;
            atStmtStart = 1;
            continue;
        }
        if (c == ';') {
            cursor++;
            atStmtStart = 1;
            continue;
        }
        if (isspace((unsigned char)c)) {
            cursor++;
            continue;
        }

        if (atStmtStart) {
            const char *lineEnd = cursor;
            const char *word = cursor;
            int isConst;

            while (*lineEnd && *lineEnd != '\n') {
                lineEnd++;
            }
            if (startsWithWord(word, lineEnd, "export")) {
                word = skipInlineSpaces(word + 6, lineEnd);
            }
            if (startsWithWord(word, lineEnd, "fn")) {
                scanFnParams(word + 2, depth + 1, line, &decls, &globals);
                for (size_t q = 0; q < contractQuants.count; q++) {
                    const char *qn = contractQuants.items[q].name;
                    checkQuantifierName(qn, qn + strlen(qn), contractQuants.items[q].line,
                                        depth + 1, &decls, &globals);
                }
                declListPopAbove(&contractQuants, -1);
                cursor = word + 2;
                atStmtStart = 0;
                continue;
            }
            if (startsWithWord(word, lineEnd, "loop")) {
                const char *scan = skipInlineSpaces(word + 4, lineEnd);
                const char *nameStart = scan;
                while (scan < lineEnd && isIdentChar(*scan)) scan++;
                const char *inWord = skipInlineSpaces(scan, lineEnd);
                if (scan > nameStart && isIdentStartChar(*nameStart) &&
                    startsWithWord(inWord, lineEnd, "in")) {
                    char *name = dupRange(nameStart, scan);
                    if (name) {
                        const AetherLocalDecl *outer = declListFind(&decls, name, 0, depth);
                        const AetherLocalDecl *g = outer ? NULL
                                                         : findGlobalValue(&globals, name, line);
                        char detail[320];
                        if (outer) {
                            snprintf(detail, sizeof(detail),
                                     "loop variable '%s' reuses the name of '%s' (line %d); the "
                                     "loop gets its own '%s', so the outer one is left "
                                     "unchanged.", name, outer->name, outer->line, name);
                            reportNameCollision(line, detail,
                                                "pick a new loop variable name, and assign the "
                                                "outer variable inside the loop if it must "
                                                "change.");
                        } else if (g) {
                            char what[160];
                            describeGlobal(g, what, sizeof(what));
                            snprintf(detail, sizeof(detail),
                                     "loop variable '%s' is the same name as %s; the loop "
                                     "gets its own '%s'.", name, what, name);
                            reportNameCollision(line, detail, "pick a new loop variable name.");
                        }
                        free(name);
                    }
                    declListPush(&decls, nameStart, scan, depth + 1, line, AETHER_DECL_LOOP);
                }
                cursor = scan > word ? scan : word + 4;
                atStmtStart = 0;
                continue;
            }
            isConst = startsWithWord(word, lineEnd, "const");
            if (isConst || startsWithWord(word, lineEnd, "let")) {
                const char *scan = skipInlineSpaces(word + (isConst ? 5 : 3), lineEnd);
                const char *nameStart;

                if (startsWithWord(scan, lineEnd, "mut")) {
                    scan = skipInlineSpaces(scan + 3, lineEnd);
                }
                nameStart = scan;
                while (scan < lineEnd && isIdentChar(*scan)) {
                    scan++;
                }
                if (scan > nameStart) {
                    char *name = dupRange(nameStart, scan);
                    if (name) {
                        /* The initializer, up to the statement's `;` on this line. */
                        const char *init = NULL;
                        const char *initEnd = lineEnd;
                        const char *q = scan;
                        while (q < lineEnd) {
                            if (*q == '"' || *q == '\'') {
                                const char *after = skipQuotedString(q, NULL);
                                q = (after > q) ? after : q + 1;
                                continue;
                            }
                            if (!init && *q == '=' && q[1] != '=' &&
                                (q == scan || (q[-1] != '!' && q[-1] != '<' && q[-1] != '>' &&
                                               q[-1] != '='))) {
                                init = q + 1;
                            } else if (init && *q == ';') {
                                initEnd = q;
                                break;
                            }
                            q++;
                        }
                        int selfRef = init && textReadsName(init, initEnd, name);
                        char assignHint[200];
                        if (init) {
                            const char *is = skipInlineSpaces(init, initEnd);
                            int n = (int)(initEnd - is);
                            while (n > 0 && isspace((unsigned char)is[n - 1])) n--;
                            if (n > 48) n = 48;
                            snprintf(assignHint, sizeof(assignHint),
                                     "assign instead: `%s = %.*s;` (for arrays, pick a new "
                                     "name).", name, n, is);
                        } else {
                            snprintf(assignHint, sizeof(assignHint),
                                     "assign to the existing '%s' instead, or pick a new name.",
                                     name);
                        }

                        const AetherLocalDecl *same = declListFind(&decls, name, depth, depth);
                        const AetherLocalDecl *g = NULL;
                        const AetherLocalDecl *outer = NULL;
                        if (!same) {
                            g = findGlobalValue(&globals, name, line);
                            if (g && depth == 0 && g->kind != AETHER_DECL_IMPORT) {
                                g = NULL; /* depth 0 is checked as `same` */
                            }
                        }
                        if (!same && !g && selfRef && depth > 0) {
                            outer = declListFind(&decls, name, 0, depth - 1);
                        }
                        char detail[320];
                        if (same && same->kind == AETHER_DECL_PARAM) {
                            snprintf(detail, sizeof(detail),
                                     "'%s' is already the parameter '%s' of this function "
                                     "(line %d); this `let` makes a second, empty '%s'.",
                                     name, same->name, same->line, name);
                            reportNameCollision(line, detail, assignHint);
                        } else if (same && strcmp(same->name, name) == 0) {
                            snprintf(detail, sizeof(detail),
                                     "local '%s' is already declared in this scope.", name);
                            reportNameCollision(line, detail, NULL);
                        } else if (same) {
                            snprintf(detail, sizeof(detail),
                                     "'%s' differs from '%s' (line %d) only by case: two names "
                                     "may not differ only by case.", name, same->name, same->line);
                            reportNameCollision(line, detail, "rename one of them.");
                        } else if (g) {
                            char what[160];
                            describeGlobal(g, what, sizeof(what));
                            snprintf(detail, sizeof(detail),
                                     "'%s' is the same name as %s: %sa local must not "
                                     "reuse a const or global name.", name, what,
                                     strcmp(name, g->name) ? "two names may not differ only by "
                                                             "case, and "
                                                           : "");
                            reportNameCollision(line, detail, "pick a new name for the local.");
                        } else if (outer) {
                            snprintf(detail, sizeof(detail),
                                     "'%s' is read in its own initializer, where it already "
                                     "names the new, still-empty '%s' (the outer one is on "
                                     "line %d).", name, name, outer->line);
                            reportNameCollision(line, detail, assignHint);
                        }
                        free(name);
                    }
                    declListPush(&decls, nameStart, scan, depth, line, AETHER_DECL_LOCAL);
                }
                cursor = scan;
                atStmtStart = 0;
                continue;
            }
        }

        /* `all NAME in` / `any NAME in` (D58): NAME is a loop variable of
         * the quantifier's condition and follows the loop-variable rule. In a
         * `@pre`/`@post` clause the parameters are not pushed yet: the name
         * waits for the next `fn`. */
        if (isIdentStartChar(c) && (cursor == source || (!isIdentChar(cursor[-1]) &&
                                                         cursor[-1] != '.'))) {
            const char *lineEnd = cursor;
            while (*lineEnd && *lineEnd != '\n') lineEnd++;
            if (startsWithWord(cursor, lineEnd, "all") || startsWithWord(cursor, lineEnd, "any")) {
                const char *scan = skipInlineSpaces(cursor + 3, lineEnd);
                const char *nameStart = scan;
                while (scan < lineEnd && isIdentChar(*scan)) scan++;
                const char *inWord = skipInlineSpaces(scan, lineEnd);
                if (scan > nameStart && isIdentStartChar(*nameStart) &&
                    startsWithWord(inWord, lineEnd, "in")) {
                    const char *ls = cursor;
                    while (ls > source && ls[-1] != '\n') ls--;
                    ls = skipInlineSpaces(ls, lineEnd);
                    if (*ls == '@') {
                        declListPush(&contractQuants, nameStart, scan, 0, line,
                                     AETHER_DECL_LOOP);
                    } else {
                        checkQuantifierName(nameStart, scan, line, depth, &decls, &globals);
                    }
                    cursor = scan;
                    atStmtStart = 0;
                    continue;
                }
            }
            while (*cursor && isIdentChar(*cursor)) cursor++;
            atStmtStart = 0;
            continue;
        }
        atStmtStart = 0;
        cursor++;
    }

    declListFree(&decls);
    declListFree(&globals);
    declListFree(&contractQuants);
}

/* --------------------------------------------------------------------------
 * AST-based effect fence (FX-001) and purity (ANN-001) checks.
 *
 * These replace the old physical-line source scan, which had three verified
 * defects: a call split across lines escaped the fence entirely, `fx` with its
 * `{` on the next line drew a spurious FX-001, and an fx block inside a @pure
 * function was not rejected. The parser registers the erased facts (fx blocks,
 * @pure decls, alias surface spellings) in side registries -- see parser.h --
 * and this walk enforces the rules on the real AST, so token layout is
 * irrelevant and node tokens supply true line numbers.
 * -------------------------------------------------------------------------- */

/* Line for a diagnostic at `node`: its own token, else the first line found in
 * its immediate operands (mirrors the compiler's getLine, then goes deeper). */
static int aetherAstNodeLine(const AST *node) {
    int i;

    if (!node) {
        return 0;
    }
    if (node->token && node->token->line > 0) {
        return node->token->line;
    }
    if (node->left) {
        int line = aetherAstNodeLine(node->left);
        if (line > 0) {
            return line;
        }
    }
    for (i = 0; i < node->child_count; i++) {
        int line = aetherAstNodeLine(node->children[i]);
        if (line > 0) {
            return line;
        }
    }
    return 0;
}

/* Display name for a function: methods are declared under the mangled
 * `Type.method` name, but diagnostics quote the bare method name (matching
 * the wording the old source scan produced from the `fn` line). */
static const char *aetherBareFunctionName(const char *name) {
    name = aetherAstFunctionDisplayName(name);
    const char *dot = name ? strrchr(name, '.') : NULL;
    return (dot && dot[1]) ? dot + 1 : name;
}

/* Compile-time arity guard for a small, hand-verified set of fixed-arity
 * builtins (BUILT-002).
 *
 * These all previously reached the VM and failed at *runtime*, so a wrong-arity
 * call sailed past `--no-run` and only died once that line executed -- a
 * generated program could pass a compile-only gate and then blow up on its
 * first formatted number. `format_float` is the motivating case: the guide's
 * Real->Text row lists canonical `format_float(r, prec)` beside the forbidden,
 * println-only `r:0:prec`, and models splice the width slot out of the second
 * into the first, producing `format_float(r, 0, prec)` (see the 2026-07-26
 * transcript findings in docs/ideas_and_todo.md).
 *
 * Every min/max pair below is read off the corresponding `arg_count` guard in
 * pscal-core (builtin.c / ext_builtins/strings/parse.c) -- do NOT add an entry
 * from a doc signature alone. `format_float`'s published signature says two
 * parameters, but the VM accepts one or two; guessing from the signature would
 * have rejected the valid one-argument call. Builtins with genuinely variadic
 * or overloaded arity (length, println, write) are deliberately absent. */
typedef struct {
    const char *name;
    size_t nameLen;
    int minArgs;
    int maxArgs;
} AetherBuiltinArity;

static const AetherBuiltinArity kAetherBuiltinArity[] = {
    {"formatfloat", 11, 1, 2},
    {"realtostr",    9, 1, 1},
    {"clamp",        5, 3, 3},
    {"min",          3, 2, 2},
    {"max",          3, 2, 2},
    {"ord",          3, 1, 1},
    {"chr",          3, 1, 1},
    {"copy",         4, 3, 3},
    {"pos",          3, 2, 2},
    {"trim",         4, 1, 1},
    {"parse_int",    9, 1, 1},
    {"parse_float", 11, 1, 1},
    {"parse_bool",  10, 1, 1},
    {"split",        5, 2, 2},
    /* The modern standard surface (D56-D61): arities of pscal-core's
     * ext_builtins/strings/stdlib.c and of the aliased mstream/file builtins. */
    {"aether_repeat_array", 19, 2, 2},
    {"aether_upper", 12, 1, 1},
    {"aether_lower", 12, 1, 1},
    {"aether_replace", 14, 3, 3},
    {"aether_contains", 15, 2, 2},
    {"aether_starts_with", 18, 2, 2},
    {"aether_ends_with", 16, 2, 2},
    {"aether_index_of", 15, 2, 2},
    {"aether_repeat", 13, 2, 2},
    {"aether_join", 11, 2, 2},
    {"aether_same", 11, 2, 2},
    {"aether_is_digit", 15, 1, 1},
    {"aether_is_alpha", 15, 1, 1},
    {"aether_is_alnum", 15, 1, 1},
    {"aether_is_space", 15, 1, 1},
    {"aether_is_upper", 15, 1, 1},
    {"aether_is_lower", 15, 1, 1},
    {"aether_sum", 10, 1, 1},
    {"aether_min_of", 13, 1, 1},
    {"aether_max_of", 13, 1, 1},
    {"aether_sorted", 13, 1, 1},
    {"aether_sorted_desc", 18, 1, 1},
    {"aether_reversed", 15, 1, 1},
    {"aether_count", 12, 2, 2},
    {"aether_sorted_by", 16, 2, 2},
    {"aether_sorted_by_desc", 21, 2, 2},
    {"aether_round_to", 15, 2, 2},
    {"aether_read_file", 16, 1, 1},
    {"aether_read_lines", 17, 1, 1},
    {"aether_write_file", 17, 2, 2},
    {"aether_append_file", 18, 2, 2},
    {"aether_delete_file", 18, 1, 1},
    {"aether_rename_file", 18, 2, 2},
    {"fileexists", 10, 1, 1},
    {"aether_read_line", 16, 0, 0},
    {"aether_read_all_stdin", 21, 0, 0},
    {"aether_args", 11, 0, 0},
    {"aether_clock_ms", 15, 0, 0},
    {"aether_now_text", 15, 0, 0},
    {"mstreamcreate", 13, 0, 0},
    {"mstreamfromstring", 17, 1, 1},
    {"mstreambuffer", 13, 1, 1},
    {"mstreamappendbyte", 17, 2, 2},
    {"mstreamsavetofile", 17, 2, 2},
    {"mstreamfree", 11, 1, 1},
    {"aether_mstream_append_text", 26, 2, 2},
    {"aether_mstream_length", 21, 1, 1},
    {"aether_mstream_load", 19, 1, 1},
    /* The environment, working-directory, DNS, HTTP and socket builtins
     * (1.0-beta2): each guard is pscal-core's arg_count check (builtin.c,
     * builtin_network_api.c, ext_builtins/system/getcurrentdir.c). */
    {"getenv", 6, 1, 1},
    {"getenvint", 9, 2, 2},
    {"getcurrentdir", 13, 0, 0},
    {"dnslookup", 9, 1, 1},
    {"httpsession", 11, 0, 0},
    {"httpclose", 9, 1, 1},
    {"httpsetheader", 13, 3, 3},
    {"httprequest", 11, 5, 5},
    {"httperrorcode", 13, 1, 1},
    {"httplasterror", 13, 1, 1},
    {"httpgetheader", 13, 2, 2},
    {"httpgetlastheaders", 18, 1, 1},
    {"httpclearheaders", 16, 1, 1},
    {"httpsetoption", 13, 3, 3},
    {"httprequesttofile", 17, 5, 5},
    {"httprequestasync", 16, 4, 4},
    {"httpisdone", 10, 1, 1},
    {"httpcancel", 10, 1, 1},
    {"httpawait", 9, 2, 2},
    {"httptryawait", 12, 2, 2},
    {"httprequestasynctofile", 22, 5, 5},
    {"httpgetasyncprogress", 20, 1, 1},
    {"httpgetasynctotal", 17, 1, 1},
    {"socketcreate", 12, 1, 2},
    {"socketconnect", 13, 3, 3},
    {"socketbind", 10, 2, 2},
    {"socketbindaddr", 14, 3, 3},
    {"socketlisten", 12, 2, 2},
    {"socketaccept", 12, 1, 1},
    {"socketpeeraddr", 14, 1, 1},
    {"socketsend", 10, 2, 2},
    {"socketreceive", 13, 2, 2},
    {"socketsetblocking", 17, 2, 2},
    {"socketpoll", 10, 3, 3},
    {"socketclose", 11, 1, 1},
    {"socketlasterror", 15, 0, 0},
    /* SQLite (ext_builtins/sqlite/sqlite_builtins.c guards); a statement
     * builtin with no guard of its own takes the handle alone. */
    {"sqliteopen", 10, 1, 1},
    {"sqliteclose", 11, 1, 1},
    {"sqliteexec", 10, 2, 2},
    {"sqliteprepare", 13, 2, 2},
    {"sqlitefinalize", 14, 1, 1},
    {"sqlitestep", 10, 1, 1},
    {"sqlitereset", 11, 1, 1},
    {"sqliteclearbindings", 19, 1, 1},
    {"sqlitecolumncount", 17, 1, 1},
    {"sqlitecolumntype", 16, 2, 2},
    {"sqlitecolumnname", 16, 2, 2},
    {"sqlitecolumnint", 15, 2, 2},
    {"sqlitecolumndouble", 18, 2, 2},
    {"sqlitecolumntext", 16, 2, 2},
    {"sqlitebindint", 13, 3, 3},
    {"sqlitebinddouble", 16, 3, 3},
    {"sqlitebindtext", 14, 3, 3},
    {"sqlitebindnull", 14, 2, 2},
    {"sqlitechanges", 13, 1, 1},
    {"sqlitelastinsertrowid", 21, 1, 1},
    {"sqliteerrmsg", 12, 1, 1},
    /* Maps (D68). */
    {"aether_map_has", 14, 2, 2},
    {"aether_map_get_or", 17, 3, 3},
    {"aether_map_remove", 17, 2, 2},
    {"aether_map_keys", 15, 1, 1},
    {"aether_map_values", 17, 1, 1},
    {"aether_map_length", 17, 1, 1},
};

/* The argument counts the builtins_json listing documents, for a builtin the
 * table above does not list (`task_stats_json(h)` reached the engine, which
 * stopped with its own "ThreadStatsJson expects no arguments."). Built once
 * from each signature: `name(a: T, b: T = d) -> R`, overloads joined by `;`;
 * a parameter with a default is optional, `...` makes the count open. */
typedef struct {
    char name[64];
    int minArgs;
    int maxArgs;
    char ret[32]; /* the documented return type when every overload names the same
                   * concrete one (Int, Text[], ToonNode, ...); "" otherwise */
} AetherSigArity;

static AetherSigArity *g_sigArity = NULL;
static int g_sigArityCount = 0;
static int g_sigArityCap = 0;
static int g_sigArityLoaded = 0;

/* One overload's parameter counts; false when it does not parse. */
static bool parseOneSignatureArity(const char *sig, const char *end, int *required, int *total) {
    const char *open = memchr(sig, '(', (size_t)(end - sig));
    if (!open) return false;
    int depth = 0, params = 0, optional = 0;
    bool any = false, variadic = false, hasDefault = false, paramVariadic = false;
    const char *p = open + 1;
    for (; p < end; p++) {
        char c = *p;
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth > 0) depth--;
        else if (c == ')' && depth == 0) break;
        else if (c == ',' && depth == 0) {
            if (any) { params++; if (hasDefault || paramVariadic) optional++; }
            any = false; hasDefault = false; paramVariadic = false;
            continue;
        } else if (c == '=' && depth == 0) hasDefault = true;
        else if (c == '.' && p + 2 < end && p[1] == '.' && p[2] == '.') variadic = paramVariadic = true;
        if (!isspace((unsigned char)c)) any = true;
    }
    if (p >= end) return false;
    /* `...` is zero or more: `println()` prints an empty line. */
    if (any) { params++; if (hasDefault || paramVariadic) optional++; }
    *required = params - optional;
    *total = variadic ? 255 : params;
    return true;
}

/* An overload's return type, when it is one an inferred `let` can carry:
 * a scalar, a TOON/stream handle, or an array of those. */
static bool signatureReturnType(const char *seg, const char *end, char *out, size_t cap) {
    const char *arrow = NULL;
    for (const char *s = seg; s + 1 < end; s++) {
        if (s[0] == '-' && s[1] == '>') arrow = s + 2;
    }
    if (!arrow) return false;
    while (arrow < end && isspace((unsigned char)*arrow)) arrow++;
    const char *stop = end;
    while (stop > arrow && isspace((unsigned char)stop[-1])) stop--;
    size_t n = (size_t)(stop - arrow);
    if (n == 0 || n >= cap) return false;
    size_t base = n;
    while (base >= 2 && arrow[base - 2] == '[' && arrow[base - 1] == ']') base -= 2;
    static const char *const kConcrete[] = {"Int", "Real", "Text", "Bool", "ToonDoc", "ToonNode",
                                            "MStream"};
    bool ok = false;
    for (size_t i = 0; i < sizeof(kConcrete) / sizeof(kConcrete[0]); i++) {
        if (strlen(kConcrete[i]) == base && strncmp(arrow, kConcrete[i], base) == 0) ok = true;
    }
    if (!ok) return false;
    memcpy(out, arrow, n);
    out[n] = '\0';
    return true;
}

static void aetherSigArityVisit(const char *name, const char *signature, void *ctx) {
    (void)ctx;
    if (!name || !signature || strlen(name) >= sizeof(g_sigArity[0].name)) return;
    int minArgs = 255, maxArgs = -1;
    char ret[32] = "";
    bool retAgreed = true;
    const char *seg = signature;
    while (*seg) {
        const char *end = strchr(seg, ';');
        if (!end) end = seg + strlen(seg);
        int req = 0, tot = 0;
        if (!parseOneSignatureArity(seg, end, &req, &tot)) return;
        if (req < minArgs) minArgs = req;
        if (tot > maxArgs) maxArgs = tot;
        char r[32];
        if (!signatureReturnType(seg, end, r, sizeof(r))) retAgreed = false;
        else if (!ret[0]) snprintf(ret, sizeof(ret), "%s", r);
        else if (strcmp(ret, r) != 0) retAgreed = false;
        seg = *end ? end + 1 : end;
        while (*seg == ' ') seg++;
    }
    if (maxArgs < 0) return;
    if (!retAgreed) ret[0] = '\0';
    for (int i = 0; i < g_sigArityCount; i++) {
        if (strcmp(g_sigArity[i].name, name) == 0) {
            if (minArgs < g_sigArity[i].minArgs) g_sigArity[i].minArgs = minArgs;
            if (maxArgs > g_sigArity[i].maxArgs) g_sigArity[i].maxArgs = maxArgs;
            if (strcmp(g_sigArity[i].ret, ret) != 0) g_sigArity[i].ret[0] = '\0';
            return;
        }
    }
    if (g_sigArityCount == g_sigArityCap) {
        int ncap = g_sigArityCap ? g_sigArityCap * 2 : 128;
        AetherSigArity *g = (AetherSigArity *)realloc(g_sigArity, (size_t)ncap * sizeof(*g));
        if (!g) return;
        g_sigArity = g;
        g_sigArityCap = ncap;
    }
    AetherSigArity *e = &g_sigArity[g_sigArityCount++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->minArgs = minArgs;
    e->maxArgs = maxArgs;
    snprintf(e->ret, sizeof(e->ret), "%s", ret);
}

static const AetherSigArity *aetherSignatureArity(const char *aetherName) {
    if (!aetherName) return NULL;
    if (!g_sigArityLoaded) {
        g_sigArityLoaded = 1;
        pscalAetherForEachBuiltinName(aetherSigArityVisit, NULL);
    }
    for (int i = 0; i < g_sigArityCount; i++) {
        if (strcmp(g_sigArity[i].name, aetherName) == 0) return &g_sigArity[i];
    }
    return NULL;
}

const char *aetherBuiltinDocumentedReturnType(const char *name) {
    if (!name) return NULL;
    const char *aetherName = pscalAetherNameForBuiltin(name);
    const AetherSigArity *sa = aetherSignatureArity(aetherName ? aetherName : name);
    return (sa && sa->ret[0]) ? sa->ret : NULL;
}

static void aetherReportArity(const char *display, int minArgs, int maxArgs, int given, int line) {
    char detail[256];
    if (minArgs == maxArgs) {
        snprintf(detail, sizeof(detail), "'%s' takes %d argument%s, but %d %s given.", display,
                 minArgs, minArgs == 1 ? "" : "s", given, given == 1 ? "was" : "were");
    } else if (maxArgs >= 255) {
        snprintf(detail, sizeof(detail), "'%s' takes at least %d argument%s, but %d %s given.",
                 display, minArgs, minArgs == 1 ? "" : "s", given, given == 1 ? "was" : "were");
    } else {
        snprintf(detail, sizeof(detail), "'%s' takes %d to %d arguments, but %d %s given.", display,
                 minArgs, maxArgs, given, given == 1 ? "was" : "were");
    }
    reportAetherErrorCoded("BUILT-002", "builtin", line, detail);
}

static void aetherCheckBuiltinArity(const AST *node,
                                    const char *canonical,
                                    const char *display,
                                    int line) {
    size_t len;
    size_t i;

    if (!node || !canonical) {
        return;
    }
    /* A user's own top-level `fn max(a, b, c)` shadows the builtin; judge the
     * call against the declaration, not this table. Method calls carry a
     * receiver in ->left and are resolved by the field/method checker, so a
     * `type` with its own `min` method is not our business either. */
    if (node->left || aetherAstIsTopLevelUserFunction(canonical)) {
        return;
    }
    len = strlen(canonical);
    for (i = 0; i < sizeof(kAetherBuiltinArity) / sizeof(kAetherBuiltinArity[0]); ++i) {
        const AetherBuiltinArity *entry = &kAetherBuiltinArity[i];
        char detail[256];
        int given;

        if (len != entry->nameLen || strncmp(canonical, entry->name, len) != 0) {
            continue;
        }
        given = node->child_count;
        if (given >= entry->minArgs && given <= entry->maxArgs) {
            return;
        }
        if (entry->minArgs == entry->maxArgs) {
            snprintf(detail,
                     sizeof(detail),
                     "'%s' takes %d argument%s, but %d %s given.",
                     display,
                     entry->minArgs,
                     entry->minArgs == 1 ? "" : "s",
                     given,
                     given == 1 ? "was" : "were");
        } else {
            snprintf(detail,
                     sizeof(detail),
                     "'%s' takes %d to %d arguments, but %d %s given.",
                     display,
                     entry->minArgs,
                     entry->maxArgs,
                     given,
                     given == 1 ? "was" : "were");
        }
        reportAetherErrorCoded("BUILT-002", "builtin", line, detail);
        return;
    }
    /* Not in the table: the signature builtins_json documents, for a call
     * written with the Aether name (an engine name such as threadpoolsubmit
     * takes the engine's own arguments) that the prepass has not already
     * judged (the TOON getters). */
    const char *aetherName = pscalAetherNameForBuiltin(canonical);
    if (!aetherName) aetherName = canonical;
    if (!display || strcmp(display, aetherName) != 0 || aetherAstPrepassReportedArity(aetherName)) {
        return;
    }
    const AetherSigArity *sa = aetherSignatureArity(aetherName);
    if (sa) {
        int given = node->child_count;
        if (given < sa->minArgs || given > sa->maxArgs) {
            aetherReportArity(display ? display : sa->name, sa->minArgs, sa->maxArgs, given, line);
        }
    }
}

/* D69: a call inside a @pure function whose callee is not declared in this
 * file may name an imported module's export, and modules load only after this
 * walk (reaPerformSemanticAnalysis). Such calls wait here and are judged once
 * the modules are in (aetherValidateModuleEffects). */
typedef struct {
    char *canonical;
    char *display;
    char *pureFn;
    int line;
} AetherDeferredPurityCall;

static AetherDeferredPurityCall *g_aether_deferred_purity = NULL;
static size_t g_aether_deferred_purity_count = 0;
static size_t g_aether_deferred_purity_cap = 0;
static int g_aether_defer_unknown_callees = 0;

static void aetherDeferPurityCall(const char *canonical, const char *display,
                                  const char *pureFn, int line) {
    if (g_aether_deferred_purity_count == g_aether_deferred_purity_cap) {
        size_t cap = g_aether_deferred_purity_cap ? g_aether_deferred_purity_cap * 2 : 8;
        AetherDeferredPurityCall *grown = (AetherDeferredPurityCall *)realloc(
            g_aether_deferred_purity, cap * sizeof(*grown));
        if (!grown) return;
        g_aether_deferred_purity = grown;
        g_aether_deferred_purity_cap = cap;
    }
    AetherDeferredPurityCall *d = &g_aether_deferred_purity[g_aether_deferred_purity_count];
    d->canonical = strdup(canonical);
    d->display = strdup(display ? display : canonical);
    d->pureFn = strdup(pureFn);
    d->line = line;
    if (!d->canonical || !d->display || !d->pureFn) {
        free(d->canonical);
        free(d->display);
        free(d->pureFn);
        return;
    }
    g_aether_deferred_purity_count++;
}

static void aetherFreeDeferredPurityCalls(void) {
    for (size_t i = 0; i < g_aether_deferred_purity_count; i++) {
        free(g_aether_deferred_purity[i].canonical);
        free(g_aether_deferred_purity[i].display);
        free(g_aether_deferred_purity[i].pureFn);
    }
    free(g_aether_deferred_purity);
    g_aether_deferred_purity = NULL;
    g_aether_deferred_purity_count = 0;
    g_aether_deferred_purity_cap = 0;
}

static void aetherCheckCallNode(const AST *node,
                                const char *canonical,
                                int fxDepth,
                                const char *pureFnName) {
    char detail[256];
    const char *display;
    int isEffectful;
    int line;

    if (!node || !canonical || !*canonical) {
        return;
    }
    /* Diagnostics quote the surface spelling the user wrote (println, sleep,
     * task_spawn, ai_chat, ...), not the canonical builtin the AST carries.
     * Two alias layers exist: parsePrimary's aliasBuiltinName records per call
     * NODE; the text pre-pass (which rewrites same-line `name(` spans before
     * the lexer runs) records per (line, canonical). */
    line = aetherAstNodeLine(node);
    display = aetherAstCallSurfaceName(node);
    if (!display) {
        display = aetherAstAliasSurfaceAtLine(line, canonical);
    }
    if (!display) {
        display = aetherAstFunctionDisplayName(canonical);
    }
    aetherCheckBuiltinArity(node, canonical, display, line);
    isEffectful = aetherIsEffectfulBuiltin(canonical, strlen(canonical));
    if (isEffectful && aetherAstIsTopLevelUserFunction(canonical)) {
        /* A user-declared top-level function shadows a same-named vm_builtin
         * for effect purposes: the call targets the user's own declaration,
         * not the builtin, so it is judged by aetherAstLookupFunctionPurity
         * below instead (see the `swap` collision writeup in
         * docs/ideas_and_todo.md). A same-named builtin with no user
         * declaration still falls through to the check unchanged. */
        isEffectful = 0;
    }
    /* The checked-I/O helpers (D73) are declared fns over hidden builtins, but
     * they read files exactly as read_file does, so a call needs `fx` too. */
    if (aetherStdlibPreludeIsEffectful(canonical)) isEffectful = 1;

    if (isEffectful && fxDepth == 0) {
        snprintf(detail, sizeof(detail), "call to '%s' requires an fx block.", display);
        reportAetherError("effect", line, detail);
    }
    if (pureFnName) {
        if (isEffectful) {
            snprintf(detail,
                     sizeof(detail),
                     "pure function '%s' cannot call effectful builtin '%s'.",
                     pureFnName,
                     display);
            reportAetherError("purity", line, detail);
        } else {
            int calleePure = 0;
            if (aetherAstLookupFunctionPurity(canonical, &calleePure)) {
                if (!calleePure) {
                    snprintf(detail,
                             sizeof(detail),
                             "pure function '%s' cannot call non-pure function '%s'.",
                             pureFnName,
                             display);
                    reportAetherError("purity", line, detail);
                }
            } else if (g_aether_defer_unknown_callees) {
                aetherDeferPurityCall(canonical, display, pureFnName, line);
            }
        }
    }
}

static void aetherWalkEffectsAndPurity(const AST *node, int fxDepth, const char *pureFnName);

static void aetherWalkEffectsAndPurityOperands(const AST *node,
                                               int fxDepth,
                                               const char *pureFnName) {
    int i;

    aetherWalkEffectsAndPurity(node->left, fxDepth, pureFnName);
    aetherWalkEffectsAndPurity(node->right, fxDepth, pureFnName);
    aetherWalkEffectsAndPurity(node->extra, fxDepth, pureFnName);
    for (i = 0; i < node->child_count; i++) {
        aetherWalkEffectsAndPurity(node->children[i], fxDepth, pureFnName);
    }
}

static void aetherWalkEffectsAndPurity(const AST *node, int fxDepth, const char *pureFnName) {
    if (!node) {
        return;
    }
    if (aetherAstNodeIsSynthesizedSubtree(node)) {
        /* Compiler-injected machinery (contract-guard failure paths call
         * writeln/halt); not user code, so not subject to the fence. */
        return;
    }

    switch (node->type) {
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL: {
            /* Function boundary: neither the effect fence nor the purity
             * context crosses into a declaration's subtree. */
            const char *declName =
                (node->token && node->token->value) ? node->token->value : NULL;
            const char *innerPure = NULL;
            int isPure = 0;

            if (declName && aetherAstLookupFunctionPurity(declName, &isPure) && isPure) {
                innerPure = aetherBareFunctionName(declName);
            }
            aetherWalkEffectsAndPurityOperands(node, 0, innerPure);
            return;
        }
        case AST_PROCEDURE_CALL:
            if (node->token && node->token->value) {
                aetherCheckCallNode(node, node->token->value, fxDepth, pureFnName);
            }
            break;
        case AST_WRITELN:
            aetherCheckCallNode(node, "writeln", fxDepth, pureFnName);
            break;
        case AST_WRITE:
            aetherCheckCallNode(node, "write", fxDepth, pureFnName);
            break;
        default:
            break;
    }

    if (aetherAstNodeIsFxBlock(node)) {
        /* The guide's exclusion: @pure functions may not contain fx blocks
         * (an effect region inside a purity contract is a contradiction even
         * when the block is empty today). */
        if (pureFnName) {
            char detail[256];
            int fxLine = aetherAstFxBlockLine(node);
            if (fxLine <= 0) {
                fxLine = aetherAstNodeLine(node);
            }
            snprintf(detail,
                     sizeof(detail),
                     "pure function '%s' contains an fx block.",
                     pureFnName);
            reportAetherError("purity", fxLine, detail);
        }
        fxDepth++;
    }
    aetherWalkEffectsAndPurityOperands(node, fxDepth, pureFnName);
}

static void aetherValidateEffectsAndPurity(const AST *root) {
    aetherFreeDeferredPurityCalls();
    g_aether_defer_unknown_callees = 1;
    aetherWalkEffectsAndPurity(root, 0, NULL);
    g_aether_defer_unknown_callees = 0;
}

/* D69: an imported module follows the same effect rules as the importer. Its
 * AST is separate and loads only during reaPerformSemanticAnalysis, so the
 * walk above never saw it: a module function could call `println` with no
 * `fx`, a module's @pure export could call an effectful helper, and a @pure
 * function could call an effectful export, all silently. Each module is walked
 * under its own path (as aetherValidateTypeNames does), then the importer's
 * deferred @pure calls are judged against the now-registered module
 * functions. */
static void aetherValidateModuleEffects(const AST *root) {
    int moduleCount = aetherGetLoadedModuleCount();
    for (int i = 0; i < moduleCount; i++) {
        const AST *m = aetherGetModuleAST(i);
        if (!m || m == root) continue;
        const char *savedPath = g_aether_source_path;
        const char *path = aetherGetModulePath(i);
        if (path) g_aether_source_path = path;
        aetherWalkEffectsAndPurity(m, 0, NULL);
        g_aether_source_path = savedPath;
    }
    for (size_t i = 0; i < g_aether_deferred_purity_count; i++) {
        const AetherDeferredPurityCall *d = &g_aether_deferred_purity[i];
        int calleePure = 0;
        if (aetherAstLookupFunctionPurity(d->canonical, &calleePure) && !calleePure) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "pure function '%s' cannot call non-pure function '%s'.",
                     d->pureFn, d->display);
            reportAetherError("purity", d->line, detail);
        }
    }
    aetherFreeDeferredPurityCalls();
}

/* NARROW-001 (warning): a Real-valued expression stored into an Int target.
 *
 * Every implicit Real -> Int narrowing used to be completely silent, in every
 * position -- `let n: Int = 3.7;` quietly became 3 even though the literal and
 * the declared type sit on the same line. The worst instance is `random()`,
 * whose no-argument form returns a Real in [0, 1): assigned to an Int it
 * truncates to a constant 0, so every "random" choice in the program becomes
 * the same choice and nothing ever says so.
 *
 * This is a warning, not an error: truncation is sometimes exactly what the
 * author meant. `int(expr)` is the explicit spelling and is never flagged --
 * `int` returns Int, so it simply fails the Real test below.
 *
 * The node's own resolved `var_type` is NOT trustworthy for this decision, and
 * that is the whole reason for the curated table. Sampling the annotations:
 * `min(3, 5)` is annotated REAL though it returns Int (min/max/clamp/abs
 * preserve their operand type), `7 / 2` is annotated REAL though Int / Int
 * evaluates to Int, `sqr(3)` comes through as VOID, and zero-argument
 * `random()` is annotated INTEGER even though that arity is the Real one.
 * Trusting the annotation would fire on `min` and on every integer division.
 *
 * So a value counts as Real only when it is one of:
 *   - a literal carrying a decimal point,
 *   - a call to a builtin verified to always return Real (table below),
 *   - `random` at arity 0 specifically,
 *   - a call to a user function whose *declared* return type is Real (the
 *     annotation is reliable here -- it comes from the signature),
 *   - a variable declared Real,
 *   - an arithmetic expression with a Real operand by these same rules.
 * Anything else is left alone, which is why Int / Int and min/max/clamp/abs
 * stay quiet.
 */
static const char *const kAetherAlwaysRealBuiltins[] = {
    "sqrt", "pow", "power", "exp", "ln", "log10",
    "sin", "cos", "tan", "arcsin", "arccos", "arctan", "atan2", "cotan",
    "sinh", "cosh", "tanh",
    "parse_float", "toon_get_real", "toon_get_real_or", "toon_real_value",
};

int aetherIsAlwaysRealBuiltin(const char *name) {
    size_t i;
    if (!name) {
        return 0;
    }
    for (i = 0; i < sizeof(kAetherAlwaysRealBuiltins) / sizeof(kAetherAlwaysRealBuiltins[0]); ++i) {
        if (strcmp(name, kAetherAlwaysRealBuiltins[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int aetherTypeIsIntegral(VarType t) {
    return t == TYPE_INTEGER || t == TYPE_INT64 || t == TYPE_INT32 ||
           t == TYPE_INT16 || t == TYPE_INT8 || t == TYPE_BYTE || t == TYPE_WORD;
}

static int aetherTypeIsReal(VarType t) {
    return t == TYPE_REAL || t == TYPE_DOUBLE || t == TYPE_FLOAT ||
           t == TYPE_LONG_DOUBLE;
}

/* Returns a short description of why the value is Real (for the message), or
 * NULL when it is not provably Real. */
static const char *aetherRealValuedReason(const AST *node, int depth) {
    if (!node || depth > 6) {
        return NULL;
    }
    switch (node->type) {
        case AST_NUMBER:
            if (aetherTypeIsReal(node->var_type)) {
                return "a Real literal";
            }
            return NULL;
        case AST_VARIABLE:
            if (aetherTypeIsReal(node->var_type)) {
                return "a Real value";
            }
            return NULL;
        case AST_PROCEDURE_CALL: {
            const char *name = (node->token && node->token->value) ? node->token->value : NULL;
            if (!name) {
                return NULL;
            }
            /* random() is Real; random(n) is Int. The annotation does not
             * distinguish them, so key off the argument count directly. */
            if (strcmp(name, "random") == 0) {
                return node->child_count == 0 ? "random() -- the no-argument form returns a Real in [0, 1)" : NULL;
            }
            {
                /* The call node's own var_type is still 0 here, so consult the
                 * declared return type recorded when the function was parsed.
                 * A user function wins over a builtin of the same name: `fn
                 * pow(b: Int, e: Int) -> Int` is Int, not "a Real-returning
                 * builtin". */
                int returnsReal = 0;
                if (aetherAstLookupFunctionReturnsReal(name, &returnsReal)) {
                    return returnsReal ? "a function declared to return Real" : NULL;
                }
            }
            if (aetherIsAlwaysRealBuiltin(name)) {
                return "a Real-returning builtin";
            }
            return NULL;
        }
        case AST_BINARY_OP: {
            const char *op = (node->token && node->token->value) ? node->token->value : NULL;
            const char *reason;
            if (!op || !(strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                         strcmp(op, "*") == 0 || strcmp(op, "/") == 0)) {
                return NULL;
            }
            reason = aetherRealValuedReason(node->left, depth + 1);
            if (reason) {
                return reason;
            }
            return aetherRealValuedReason(node->right, depth + 1);
        }
        case AST_UNARY_OP:
            return aetherRealValuedReason(node->left, depth + 1);
        default:
            return NULL;
    }
}

static void aetherReportNarrowing(const AST *site, const char *target, const char *reason) {
    char detail[288];
    snprintf(detail, sizeof(detail),
             "%s is Int, but the value assigned is %s; the fractional part is "
             "discarded silently. Write int(...) if the truncation is intended.",
             target, reason);
    reportAetherWarningCoded("NARROW-001", "narrowing", aetherAstNodeLine(site), detail);
}

static void aetherWalkNarrowing(const AST *node) {
    int i;
    if (!node) {
        return;
    }
    /* Only an explicitly written `: Int` can contradict its initializer. An
     * inferred `let x = a * realVal;` still reads as integral here (its type
     * resolves after this pass), and warning on it would be plain wrong -- the
     * binding becomes Real. tests/numeric_expr_inference_pass.aether is exactly
     * that shape and caught this during the corpus sweep. */
    if (node->type == AST_VAR_DECL && aetherAstDeclHasExplicitType(node) &&
        aetherTypeIsIntegral(node->var_type) && node->left) {
        const char *reason = aetherRealValuedReason(node->left, 0);
        if (reason) {
            const char *name = NULL;
            for (i = 0; i < node->child_count; i++) {
                const AST *c = node->children[i];
                if (c && c->type == AST_VARIABLE && c->token && c->token->value) {
                    name = c->token->value;
                    break;
                }
            }
            if (name) {
                char target[96];
                snprintf(target, sizeof(target), "'%s'", name);
                aetherReportNarrowing(node, target, reason);
            } else {
                aetherReportNarrowing(node, "this binding", reason);
            }
        }
    }
    if (node->type == AST_ASSIGN && node->left && node->right &&
        node->left->type == AST_VARIABLE &&
        aetherTypeIsIntegral(node->left->var_type)) {
        const char *reason = aetherRealValuedReason(node->right, 0);
        if (reason) {
            char target[96];
            const char *name = (node->left->token && node->left->token->value)
                                   ? node->left->token->value : NULL;
            if (name) {
                snprintf(target, sizeof(target), "'%s'", name);
            } else {
                snprintf(target, sizeof(target), "the assignment target");
            }
            aetherReportNarrowing(node, target, reason);
        }
    }
    aetherWalkNarrowing(node->left);
    aetherWalkNarrowing(node->right);
    aetherWalkNarrowing(node->extra);
    for (i = 0; i < node->child_count; i++) {
        aetherWalkNarrowing(node->children[i]);
    }
}


/* --------------------------------------------------------------------------
 * PAR-001, whole program: races the parse-time check cannot see.
 *
 * parseParBlock rejects the same record *name* handed to two branches, but it
 * runs before the callees are parsed and compares names only. Three shapes got
 * through, and each raced: lost updates on an Int global (332962 of 400000),
 * and on a shared Text or record, a refcount double-release that killed the VM
 * (exit 133/134/138):
 *   - a global one branch's call graph writes and another branch's uses;
 *   - one record under two names (`let b: S = a; par { f(a); f(b); }`);
 *   - the same field or element expression in two branches (`f(h.c)` twice),
 *     or a record in one branch and a part of it in another (`f(h); g(h.c)`).
 * Distinct elements (`f(ts[0]); f(ts[1])`) stay legal: they are the taught
 * "each branch its own record" shape.
 * -------------------------------------------------------------------------- */

typedef struct {
    const char **items;
    int count, cap;
} ParNameSet;

static bool parSetHas(const ParNameSet *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (strcasecmp(s->items[i], name) == 0) return true;
    return false;
}

static void parSetAdd(ParNameSet *s, const char *name) {
    if (!name || parSetHas(s, name)) return;
    if (s->count == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 16;
        const char **n = (const char **)realloc(s->items, (size_t)ncap * sizeof(*n));
        if (!n) return;
        s->items = n;
        s->cap = ncap;
    }
    s->items[s->count++] = name;
}

static void parSetFree(ParNameSet *s) {
    free(s->items);
    memset(s, 0, sizeof(*s));
}

typedef struct {
    ParNameSet globals;       /* top-level `let` names */
    const AST **fns;          /* every function / procedure declaration */
    int fnCount, fnCap;
} ParIndex;

static void parIndexProgram(ParIndex *ix, const AST *n, bool topLevel) {
    if (!n) return;
    if (n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) {
        if (ix->fnCount == ix->fnCap) {
            int ncap = ix->fnCap ? ix->fnCap * 2 : 32;
            const AST **f = (const AST **)realloc(ix->fns, (size_t)ncap * sizeof(*f));
            if (!f) return;
            ix->fns = f;
            ix->fnCap = ncap;
        }
        ix->fns[ix->fnCount++] = n;
        topLevel = false;
    } else if (n->type == AST_VAR_DECL) {
        if (topLevel) {
            for (int i = 0; i < n->child_count; i++) {
                const AST *v = n->children[i];
                if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                    parSetAdd(&ix->globals, v->token->value);
            }
        }
        return;
    }
    parIndexProgram(ix, n->left, topLevel);
    parIndexProgram(ix, n->right, topLevel);
    parIndexProgram(ix, n->extra, topLevel);
    for (int i = 0; i < n->child_count; i++) parIndexProgram(ix, n->children[i], topLevel);
}

/* The declarations a call can reach: an exact (case-insensitive) name, or for
 * `recv.method` / `Type.method`, every method of that name. */
static bool parCallMatches(const char *callName, const AST *fn) {
    const char *fname = fn->token ? fn->token->value : NULL;
    if (!fname || !callName) return false;
    if (strcasecmp(callName, fname) == 0) return true;
    const char *cdot = strrchr(callName, '.');
    const char *fdot = strrchr(fname, '.');
    return cdot && fdot && strcasecmp(cdot + 1, fdot + 1) == 0;
}

/* The variable an lvalue chain is rooted at (`g`, `g.f`, `g[i].f`), or NULL. */
static const AST *parLValueRoot(const AST *n) {
    while (n && (n->type == AST_FIELD_ACCESS || n->type == AST_ARRAY_ACCESS)) n = n->left;
    return (n && n->type == AST_VARIABLE && n->token && n->token->value) ? n : NULL;
}

typedef struct {
    const ParIndex *ix;
    ParNameSet locals;   /* params and lets of the function being scanned */
    ParNameSet written;  /* globals written */
    ParNameSet used;     /* globals read or written */
    const AST **seen;    /* functions already scanned (recursion guard) */
    int seenCount, seenCap;
} ParAccess;

static void parScanFunction(ParAccess *a, const AST *fn);

static bool parIsGlobal(const ParAccess *a, const char *name) {
    return name && parSetHas(&a->ix->globals, name) && !parSetHas(&a->locals, name);
}

static void parScan(ParAccess *a, const AST *n) {
    if (!n) return;
    switch (n->type) {
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            return; /* a nested declaration is scanned when it is called */
        case AST_VAR_DECL:
            for (int i = 0; i < n->child_count; i++) {
                const AST *v = n->children[i];
                if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                    parSetAdd(&a->locals, v->token->value);
            }
            parScan(a, n->left);
            return;
        case AST_ASSIGN: {
            const AST *root = parLValueRoot(n->left);
            if (root && parIsGlobal(a, root->token->value)) {
                parSetAdd(&a->written, root->token->value);
                parSetAdd(&a->used, root->token->value);
            }
            break;
        }
        case AST_VARIABLE:
            if (n->token && parIsGlobal(a, n->token->value)) parSetAdd(&a->used, n->token->value);
            return;
        case AST_PROCEDURE_CALL:
            if (n->token && n->token->value) {
                for (int i = 0; i < a->ix->fnCount; i++)
                    if (parCallMatches(n->token->value, a->ix->fns[i]))
                        parScanFunction(a, a->ix->fns[i]);
            }
            break;
        default:
            break;
    }
    parScan(a, n->left);
    parScan(a, n->right);
    parScan(a, n->extra);
    for (int i = 0; i < n->child_count; i++) parScan(a, n->children[i]);
}

static void parScanFunction(ParAccess *a, const AST *fn) {
    for (int i = 0; i < a->seenCount; i++)
        if (a->seen[i] == fn) return;
    if (a->seenCount == a->seenCap) {
        int ncap = a->seenCap ? a->seenCap * 2 : 16;
        const AST **s = (const AST **)realloc(a->seen, (size_t)ncap * sizeof(*s));
        if (!s) return;
        a->seen = s;
        a->seenCap = ncap;
    }
    a->seen[a->seenCount++] = fn;
    /* Locals are per function: a callee's `let x` must not hide the caller's
     * view of a global `x`, nor the other way round. */
    ParNameSet saved = a->locals;
    memset(&a->locals, 0, sizeof(a->locals));
    for (int i = 0; i < fn->child_count; i++) {
        const AST *prm = fn->children[i];
        if (!prm || prm->type != AST_VAR_DECL) continue;
        for (int j = 0; j < prm->child_count; j++) {
            const AST *v = prm->children[j];
            if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                parSetAdd(&a->locals, v->token->value);
        }
    }
    parScan(a, fn->type == AST_FUNCTION_DECL ? fn->extra : fn->right);
    parScan(a, fn->type == AST_FUNCTION_DECL ? fn->right : fn->extra);
    parSetFree(&a->locals);
    a->locals = saved;
}

/* A record-ish argument expression, printed for comparison (`h.c`, `ts[1]`). */
static bool parExprText(const AST *n, char *buf, size_t cap, size_t *len) {
    if (!n) return false;
    if (n->type == AST_VARIABLE && n->token && n->token->value) {
        int w = snprintf(buf + *len, cap - *len, "%s", n->token->value);
        if (w < 0 || (size_t)w >= cap - *len) return false;
        *len += (size_t)w;
        return true;
    }
    if (n->type == AST_NUMBER && n->token && n->token->value) {
        int w = snprintf(buf + *len, cap - *len, "%s", n->token->value);
        if (w < 0 || (size_t)w >= cap - *len) return false;
        *len += (size_t)w;
        return true;
    }
    if (n->type == AST_FIELD_ACCESS && n->right && n->right->token && n->right->token->value) {
        if (!parExprText(n->left, buf, cap, len)) return false;
        int w = snprintf(buf + *len, cap - *len, ".%s", n->right->token->value);
        if (w < 0 || (size_t)w >= cap - *len) return false;
        *len += (size_t)w;
        return true;
    }
    if (n->type == AST_ARRAY_ACCESS && n->child_count == 1) {
        if (!parExprText(n->left, buf, cap, len)) return false;
        if (*len + 1 >= cap) return false;
        buf[(*len)++] = '[';
        if (!parExprText(n->children[0], buf, cap, len)) return false;
        if (*len + 1 >= cap) return false;
        buf[(*len)++] = ']';
        buf[*len] = '\0';
        return true;
    }
    return false;
}

static bool parIsAggregate(const AST *n) {
    return n && (n->var_type == TYPE_POINTER || n->var_type == TYPE_RECORD ||
                 n->var_type == TYPE_ARRAY);
}

/* Record aliases in the function around a par block: `let b: S = a;` and
 * `b = a;` put a and b in one class. A tiny union-find over names. */
typedef struct {
    const char *name[128];
    int parent[128];
    int count;
} ParAliases;

static int parAliasFind(ParAliases *al, const char *name) {
    int idx = -1;
    for (int i = 0; i < al->count; i++)
        if (strcasecmp(al->name[i], name) == 0) { idx = i; break; }
    if (idx < 0) {
        if (al->count >= 128) return -1;
        idx = al->count++;
        al->name[idx] = name;
        al->parent[idx] = idx;
    }
    while (al->parent[idx] != idx) idx = al->parent[idx];
    return idx;
}

static void parAliasUnion(ParAliases *al, const char *a, const char *b) {
    int ra = parAliasFind(al, a), rb = parAliasFind(al, b);
    if (ra >= 0 && rb >= 0 && ra != rb) al->parent[ra] = rb;
}

static void parReport(int line, const char *detail) {
    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s The par branches run at the same time, so they race on it (lost updates, "
             "or a crash on a Text or record). Give each branch its own record and combine "
             "the results after the `par { ... }` block.",
             detail);
    aetherSemanticReportCoded("PAR-001", "par", line > 0 ? line : 1, msg, 1);
}

static const AST *parSpawnCall(const AST *stmt) {
    if (stmt && stmt->type == AST_VAR_DECL && stmt->left &&
        stmt->left->type == AST_THREAD_SPAWN && stmt->left->left &&
        stmt->left->left->type == AST_PROCEDURE_CALL)
        return stmt->left->left;
    return NULL;
}

static void parCheckBlock(const ParIndex *ix, ParAliases *al, const AST *block) {
    const AST *calls[64];
    int n = 0;
    for (int i = 0; i < block->child_count && n < 64; i++) {
        const AST *c = parSpawnCall(block->children[i]);
        if (c) calls[n++] = c;
    }
    if (n < 2) return;
    int line = calls[0]->token ? calls[0]->token->line : 0;

    /* Globals. */
    ParAccess acc[64];
    memset(acc, 0, sizeof(acc));
    for (int b = 0; b < n; b++) {
        acc[b].ix = ix;
        for (int i = 0; i < calls[b]->child_count; i++) parScan(&acc[b], calls[b]->children[i]);
        parScan(&acc[b], calls[b]);
    }
    bool reported = false;
    for (int b = 0; b < n && !reported; b++) {
        for (int w = 0; w < acc[b].written.count && !reported; w++) {
            const char *g = acc[b].written.items[w];
            for (int o = 0; o < n && !reported; o++) {
                if (o == b || !parSetHas(&acc[o].used, g)) continue;
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "the global '%s' is written by par branch %d (%s) and used by "
                         "branch %d (%s).",
                         g, b + 1, calls[b]->token ? calls[b]->token->value : "?", o + 1,
                         calls[o]->token ? calls[o]->token->value : "?");
                parReport(line, detail);
                reported = true;
            }
        }
    }
    for (int b = 0; b < n; b++) {
        parSetFree(&acc[b].written);
        parSetFree(&acc[b].used);
        parSetFree(&acc[b].locals);
        free(acc[b].seen);
    }
    if (reported) return;

    /* Aliased or overlapping aggregate arguments. */
    for (int b = 0; b < n; b++) {
        for (int i = 0; i < calls[b]->child_count; i++) {
            const AST *x = calls[b]->children[i];
            if (!parIsAggregate(x)) continue;
            for (int o = b + 1; o < n; o++) {
                for (int j = 0; j < calls[o]->child_count; j++) {
                    const AST *y = calls[o]->children[j];
                    if (!parIsAggregate(y)) continue;
                    char tx[160], ty[160];
                    size_t lx = 0, ly = 0;
                    tx[0] = ty[0] = '\0';
                    if (!parExprText(x, tx, sizeof(tx), &lx) ||
                        !parExprText(y, ty, sizeof(ty), &ly))
                        continue;
                    const AST *rx = parLValueRoot(x), *ry = parLValueRoot(y);
                    bool same = strcasecmp(tx, ty) == 0;
                    bool whole = rx && ry && strcasecmp(rx->token->value, ry->token->value) == 0 &&
                                 (x->type == AST_VARIABLE || y->type == AST_VARIABLE);
                    bool alias = x->type == AST_VARIABLE && y->type == AST_VARIABLE && !same &&
                                 parAliasFind(al, tx) >= 0 &&
                                 parAliasFind(al, tx) == parAliasFind(al, ty);
                    if (!same && !whole && !alias) continue;
                    char detail[320];
                    if (alias)
                        snprintf(detail, sizeof(detail),
                                 "'%s' (branch %d) and '%s' (branch %d) are the same record: "
                                 "one was assigned from the other.",
                                 tx, b + 1, ty, o + 1);
                    else if (same)
                        snprintf(detail, sizeof(detail),
                                 "'%s' is passed to par branches %d and %d.", tx, b + 1, o + 1);
                    else
                        snprintf(detail, sizeof(detail),
                                 "'%s' (branch %d) and '%s' (branch %d) overlap: one is "
                                 "part of the other.",
                                 tx, b + 1, ty, o + 1);
                    parReport(line, detail);
                    return;
                }
            }
        }
    }
}

/* Walk statements in source order, collecting record aliases as they are made,
 * and check each par block with the aliases made before it. */
static void parWalk(const ParIndex *ix, ParAliases *al, const AST *n) {
    if (!n) return;
    if (n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) {
        ParAliases fresh;
        memset(&fresh, 0, sizeof(fresh));
        parWalk(ix, &fresh, n->left);
        parWalk(ix, &fresh, n->right);
        parWalk(ix, &fresh, n->extra);
        for (int i = 0; i < n->child_count; i++) parWalk(ix, &fresh, n->children[i]);
        return;
    }
    if (n->type == AST_VAR_DECL && n->left && n->left->type == AST_VARIABLE &&
        n->left->token && n->left->token->value && parIsAggregate(n->left)) {
        for (int i = 0; i < n->child_count; i++) {
            const AST *v = n->children[i];
            if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                parAliasUnion(al, v->token->value, n->left->token->value);
        }
    }
    if (n->type == AST_ASSIGN && n->left && n->right && n->left->type == AST_VARIABLE &&
        n->right->type == AST_VARIABLE && n->left->token && n->right->token &&
        n->left->token->value && n->right->token->value && parIsAggregate(n->right)) {
        parAliasUnion(al, n->left->token->value, n->right->token->value);
    }
    if (n->type == AST_COMPOUND) {
        bool isPar = false;
        for (int i = 0; i < n->child_count && !isPar; i++) isPar = parSpawnCall(n->children[i]) != NULL;
        if (isPar) parCheckBlock(ix, al, n);
    }
    parWalk(ix, al, n->left);
    parWalk(ix, al, n->right);
    parWalk(ix, al, n->extra);
    for (int i = 0; i < n->child_count; i++) parWalk(ix, al, n->children[i]);
}

/* `break` / `continue` outside a loop reached the backend, which reported it
 * with no code and line 0: "L0: Compiler error: 'break' statement outside of
 * a loop." */
/* --------------------------------------------------------------------------
 * D70: names are case-sensitive.
 *
 * The shared engine resolves names ignoring case, so `total` reached a `let
 * Total`, `Println(x)` printed and `new point()` built a `Point`. A reference
 * must now be spelled exactly as its declaration: a name that matches a
 * declared one only up to case is SCOPE-001 "did you mean `Total`?". Two
 * declarations that differ only by case stay NAME-001 (the engine cannot keep
 * them apart). The pass runs on the parsed program, before rea folds names;
 * a name declared nowhere in it (an import, a builtin) is left to the passes
 * that know those, and builtin calls are checked where they are parsed.
 * -------------------------------------------------------------------------- */

typedef struct {
    const char **names;
    int count, cap;
} CaseNameSet;

static void caseSetAdd(CaseNameSet *s, const char *name) {
    if (!name || !*name) return;
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return;
    if (s->count == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 64;
        const char **n = (const char **)realloc(s->names, (size_t)ncap * sizeof(*n));
        if (!n) return;
        s->names = n;
        s->cap = ncap;
    }
    s->names[s->count++] = name;
}

/* 1: declared exactly; 0: declared only in another case (*declared set);
 * -1: not declared here at all. */
static int caseLookup(const CaseNameSet *s, const char *name, size_t len, const char **declared) {
    int found = -1;
    for (int i = 0; i < s->count; i++) {
        const char *d = s->names[i];
        if (strlen(d) != len || strncasecmp(d, name, len) != 0) continue;
        if (strncmp(d, name, len) == 0) return 1;
        found = 0;
        *declared = d;
    }
    return found;
}

static bool caseNameIsInternal(const char *name) {
    return !name || name[0] == '_' || strcmp(name, "myself") == 0 ||
           strcmp(name, "self") == 0 || strchr(name, '@') != NULL;
}

/* Types and values are separate namespaces (`let board: Board` is legal), so
 * each reference is compared with its own kind of declaration. A record's
 * fields are neither: FIELD-002 checks them. */
typedef struct {
    CaseNameSet values; /* lets, params, consts, functions, methods */
    CaseNameSet types;
} CaseDecls;

static void caseCollect(CaseDecls *d, const AST *n) {
    if (!n) return;
    switch (n->type) {
        case AST_RECORD_TYPE:
            /* fields: skip the declarations, but methods inside still count */
            for (int i = 0; i < n->child_count; i++)
                if (n->children[i] && n->children[i]->type != AST_VAR_DECL)
                    caseCollect(d, n->children[i]);
            return;
        case AST_VAR_DECL:
            for (int i = 0; i < n->child_count; i++) {
                const AST *v = n->children[i];
                if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                    caseSetAdd(&d->values, v->token->value);
            }
            break;
        case AST_CONST_DECL:
            if (n->token && n->token->value) caseSetAdd(&d->values, n->token->value);
            break;
        case AST_TYPE_DECL:
            if (n->token && n->token->value) caseSetAdd(&d->types, n->token->value);
            break;
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            if (n->token && n->token->value) {
                const char *dot = strrchr(n->token->value, '.');
                caseSetAdd(&d->values, dot ? dot + 1 : n->token->value);
            }
            break;
        default:
            break;
    }
    caseCollect(d, n->left);
    caseCollect(d, n->right);
    caseCollect(d, n->extra);
    for (int i = 0; i < n->child_count; i++) caseCollect(d, n->children[i]);
}

static void caseReport(const AST *at, const char *written, size_t len, const char *declared) {
    char detail[256];
    snprintf(detail, sizeof(detail),
             "identifier '%.*s' not in scope: names are case-sensitive, and the one declared "
             "is '%s'. Did you mean `%s`?",
             (int)len, written, declared, declared);
    int line = at && at->token ? at->token->line : 0;
    reportAetherErrorCoded("SCOPE-001", "scope", line > 0 ? line : 1, detail);
}

static void caseCheckName(const CaseNameSet *s, const AST *at, const char *name) {
    if (!name || caseNameIsInternal(name)) return;
    const char *declared = NULL;
    size_t len = strlen(name);
    if (caseLookup(s, name, len, &declared) == 0) caseReport(at, name, len, declared);
}

static void caseCheckRefs(const CaseDecls *d, const AST *n) {
    if (!n) return;
    switch (n->type) {
        case AST_VAR_DECL:
            /* the declared names are not references; the type node and the
             * initializer are */
            caseCheckRefs(d, n->left);
            caseCheckRefs(d, n->right);
            return;
        case AST_VARIABLE:
            if (n->token) caseCheckName(&d->values, n, n->token->value);
            return;
        case AST_FIELD_ACCESS:
            caseCheckRefs(d, n->left); /* the field name is FIELD-002's */
            return;
        case AST_PROCEDURE_CALL:
            if (n->token && n->token->value) {
                const char *dot = strrchr(n->token->value, '.');
                caseCheckName(&d->values, n, dot ? dot + 1 : n->token->value);
            }
            break;
        case AST_TYPE_IDENTIFIER:
        case AST_TYPE_REFERENCE:
        case AST_NEW:
            if (n->token) caseCheckName(&d->types, n, n->token->value);
            break;
        default:
            break;
    }
    caseCheckRefs(d, n->left);
    caseCheckRefs(d, n->right);
    caseCheckRefs(d, n->extra);
    for (int i = 0; i < n->child_count; i++) caseCheckRefs(d, n->children[i]);
}

/* The value names of the program being compiled (they point into its AST),
 * kept for aetherUnknownIdentifierHint's close-spelling pass. */
static CaseNameSet g_declaredValues;

size_t aetherSemanticDeclaredValueCount(void) {
    return (size_t)g_declaredValues.count;
}

const char *aetherSemanticDeclaredValueAt(size_t i) {
    return i < (size_t)g_declaredValues.count ? g_declaredValues.names[i] : NULL;
}

static void aetherValidateNameCase(const AST *root) {
    CaseDecls d;
    memset(&d, 0, sizeof(d));
    caseCollect(&d, root);
    caseCheckRefs(&d, root);
    free(g_declaredValues.names);
    g_declaredValues = d.values;
    free(d.types.names);
}

/* --------------------------------------------------------------------------
 * Members are not implicit: inside a method a field is `self.v`, never `v`.
 *
 * The shared engine gives a bare name in a method Pascal's meaning, a field
 * of the hidden global `myself`, which Aether never sets (its receiver is a
 * parameter). So `ret v + 1;` compiled and stopped at run time with the
 * uncoded "Cannot access field on a non-record/non-pointer type", `v = x;`
 * failed with an uncoded "Undefined variable 'v'", and a top-level let named
 * like a field was shadowed by the same crash. A bare name that is a field of
 * the method's type, and not one of its parameters or locals, is SCOPE-001
 * naming `self.v`. Contract clauses are scoped by the parser (`@pre v > 0`
 * reads `self.v`) and never reach here as a bare name.
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *name;            /* the type's name */
    CaseNameSet fields;
} BareFieldType;

typedef struct {
    BareFieldType *types;
    int count, cap;
    CaseNameSet globals;         /* top-level lets and consts */
    const AST *globalDecls[256]; /* their declarations, for the line */
    int globalCount;
} BareFieldIndex;

static void bareFieldCollect(BareFieldIndex *ix, const AST *n, bool inRoutine) {
    if (!n) return;
    switch (n->type) {
        case AST_TYPE_DECL:
            if (n->token && n->token->value && n->left && n->left->type == AST_RECORD_TYPE) {
                if (ix->count == ix->cap) {
                    int ncap = ix->cap ? ix->cap * 2 : 16;
                    BareFieldType *g = (BareFieldType *)realloc(ix->types, (size_t)ncap * sizeof(*g));
                    if (!g) return;
                    ix->types = g;
                    ix->cap = ncap;
                }
                BareFieldType *t = &ix->types[ix->count++];
                memset(t, 0, sizeof(*t));
                t->name = n->token->value;
                const AST *rec = n->left;
                for (int i = 0; i < rec->child_count; i++) {
                    const AST *f = rec->children[i];
                    if (!f || f->type != AST_VAR_DECL) continue;
                    for (int k = 0; k < f->child_count; k++) {
                        const AST *v = f->children[k];
                        if (v && v->type == AST_VARIABLE && v->token && v->token->value &&
                            !caseNameIsInternal(v->token->value))
                            caseSetAdd(&t->fields, v->token->value);
                    }
                }
            }
            return;
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            inRoutine = true;
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            if (!inRoutine) {
                const AST *v = n->type == AST_CONST_DECL ? n : (n->child_count > 0 ? n->children[0] : NULL);
                const char *name = v && v->token ? v->token->value : NULL;
                if (name && !caseNameIsInternal(name)) {
                    caseSetAdd(&ix->globals, name);
                    if (ix->globalCount < (int)(sizeof(ix->globalDecls) / sizeof(ix->globalDecls[0])))
                        ix->globalDecls[ix->globalCount++] = n;
                }
            }
            break;
        default:
            break;
    }
    bareFieldCollect(ix, n->left, inRoutine);
    bareFieldCollect(ix, n->right, inRoutine);
    bareFieldCollect(ix, n->extra, inRoutine);
    for (int i = 0; i < n->child_count; i++) bareFieldCollect(ix, n->children[i], inRoutine);
}

/* Every name a routine declares (parameters, lets, loop and quantifier
 * variables, hidden temps): Aether's function scope is flat. */
static void bareFieldLocals(CaseNameSet *locals, const AST *n) {
    if (!n) return;
    if (n->type == AST_VAR_DECL) {
        for (int i = 0; i < n->child_count; i++) {
            const AST *v = n->children[i];
            if (v && v->type == AST_VARIABLE && v->token && v->token->value)
                caseSetAdd(locals, v->token->value);
        }
    }
    bareFieldLocals(locals, n->left);
    bareFieldLocals(locals, n->right);
    bareFieldLocals(locals, n->extra);
    for (int i = 0; i < n->child_count; i++) bareFieldLocals(locals, n->children[i]);
}

static int bareFieldGlobalLine(const BareFieldIndex *ix, const char *name) {
    for (int i = 0; i < ix->globalCount; i++) {
        const AST *d = ix->globalDecls[i];
        const AST *v = d->type == AST_CONST_DECL ? d : (d->child_count > 0 ? d->children[0] : NULL);
        if (v && v->token && v->token->value && strcmp(v->token->value, name) == 0)
            return v->token->line;
    }
    return 0;
}

static void bareFieldCheckRefs(const BareFieldIndex *ix, const BareFieldType *t,
                               const CaseNameSet *locals, CaseNameSet *reported, const AST *n) {
    if (!n) return;
    switch (n->type) {
        case AST_VAR_DECL:
            /* the declared names are not references; the initializer is */
            bareFieldCheckRefs(ix, t, locals, reported, n->left);
            return;
        case AST_FIELD_ACCESS:
            bareFieldCheckRefs(ix, t, locals, reported, n->left); /* `x.v`: v is a field name */
            return;
        case AST_ASSIGN:
            /* A record literal's `v: value` (`new T { v: self.v + 1 }`) names
             * the field on its left: only the value is a reference. */
            if (n->token && n->token->value && strcmp(n->token->value, ":") == 0) {
                bareFieldCheckRefs(ix, t, locals, reported, n->right);
                return;
            }
            break;
        case AST_VARIABLE: {
            const char *name = n->token ? n->token->value : NULL;
            const char *unused = NULL;
            if (!name || caseNameIsInternal(name)) return;
            size_t len = strlen(name);
            if (caseLookup(&t->fields, name, len, &unused) != 1 ||
                caseLookup(locals, name, len, &unused) == 1 ||
                caseLookup(reported, name, len, &unused) == 1)
                return;
            caseSetAdd(reported, name);
            int line = n->token->line > 0 ? n->token->line : 1;
            char detail[320], hint[200];
            if (caseLookup(&ix->globals, name, len, &unused) == 1) {
                snprintf(detail, sizeof(detail),
                         "'%s' is both a field of %s and a top-level name (line %d), and "
                         "inside a method a bare '%s' cannot reach the top-level one.",
                         name, t->name, bareFieldGlobalLine(ix, name), name);
                snprintf(hint, sizeof(hint),
                         "write `self.%s` for the field; to use the top-level '%s' in a "
                         "method, rename the field or the top-level name.", name, name);
            } else {
                snprintf(detail, sizeof(detail),
                         "identifier '%s' not in scope: '%s' is a field of %s, and members are "
                         "not implicit inside a method.", name, name, t->name);
                snprintf(hint, sizeof(hint), "write `self.%s`.", name);
            }
            fprintf(stderr, "%s:%d: [SCOPE-001] Aether scope error: %s\n",
                    g_aether_source_path ? g_aether_source_path : "<aether>", line, detail);
            fprintf(stderr, "hint: %s\n", hint);
            aetherReportGuideHelp("SCOPE-001");
            pascal_semantic_error_count++;
            return;
        }
        default:
            break;
    }
    bareFieldCheckRefs(ix, t, locals, reported, n->left);
    bareFieldCheckRefs(ix, t, locals, reported, n->right);
    bareFieldCheckRefs(ix, t, locals, reported, n->extra);
    for (int i = 0; i < n->child_count; i++)
        bareFieldCheckRefs(ix, t, locals, reported, n->children[i]);
}

static void bareFieldCheckMethods(const BareFieldIndex *ix, const AST *n) {
    if (!n) return;
    if ((n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) && n->token &&
        n->token->value) {
        const char *dot = strrchr(n->token->value, '.');
        if (dot) {
            size_t tlen = (size_t)(dot - n->token->value);
            for (int i = 0; i < ix->count; i++) {
                const BareFieldType *t = &ix->types[i];
                if (t->fields.count == 0 || strlen(t->name) != tlen ||
                    strncmp(t->name, n->token->value, tlen) != 0)
                    continue;
                CaseNameSet locals = {0}, reported = {0};
                bareFieldLocals(&locals, n);
                bareFieldCheckRefs(ix, t, &locals, &reported,
                                   n->type == AST_FUNCTION_DECL ? n->extra : n->right);
                free(locals.names);
                free(reported.names);
                break;
            }
        }
        return; /* methods are top-level; nothing nests inside one */
    }
    bareFieldCheckMethods(ix, n->left);
    bareFieldCheckMethods(ix, n->right);
    bareFieldCheckMethods(ix, n->extra);
    for (int i = 0; i < n->child_count; i++) bareFieldCheckMethods(ix, n->children[i]);
}

/* --------------------------------------------------------------------------
 * D86: a `loop x in xs` body may not change xs (LOOP-001).
 *
 * A variable, field or index path is walked in place: the loop re-reads its
 * length and element on every pass. So an append in the body kept the loop
 * going for ever, a shrink stopped it early, `xs = [7, 8, 9]` switched it to
 * the new array mid-walk, and changing `r` in `loop x in rows[r]` switched
 * rows. Python reads the same text differently by case, Swift and Go never
 * see the change, and no program in the tree relied on any of it. Rejecting
 * the write keeps both meanings open for later. A write is an assignment or
 * an append rooted at the collection (or a path above or below it), an
 * assignment to a variable in its index, or a call to a nested function that
 * writes the collection (D64: its by-reference capture parameter). A method
 * call that changes a field through the record is not seen here.
 * -------------------------------------------------------------------------- */

#define FE_MAX_STEPS 16

typedef struct {
    const char *root;                /* `xs`, `myself`, `rows` */
    const char *steps[FE_MAX_STEPS]; /* a field name, or NULL for an index */
    int stepCount;
} FePath;

static bool fePath(const AST *n, FePath *out) {
    const char *rev[FE_MAX_STEPS];
    int count = 0;
    while (n && (n->type == AST_FIELD_ACCESS || n->type == AST_ARRAY_ACCESS)) {
        if (n->type == AST_FIELD_ACCESS) {
            if (count >= FE_MAX_STEPS || !n->token || !n->token->value) return false;
            rev[count++] = n->token->value;
        } else {
            for (int i = 0; i < n->child_count; i++) {
                if (count >= FE_MAX_STEPS) return false;
                rev[count++] = NULL;
            }
        }
        n = n->left;
    }
    if (!n || n->type != AST_VARIABLE || !n->token || !n->token->value) return false;
    out->root = n->token->value;
    out->stepCount = count;
    for (int i = 0; i < count; i++) out->steps[i] = rev[count - 1 - i];
    return true;
}

/* One path is a prefix of the other, an index matching any index. */
static bool fePathsOverlap(const FePath *a, const FePath *b) {
    if (strcmp(a->root, b->root) != 0) return false;
    int n = a->stepCount < b->stepCount ? a->stepCount : b->stepCount;
    for (int i = 0; i < n; i++) {
        if (!a->steps[i] != !b->steps[i]) return false;
        if (a->steps[i] && strcmp(a->steps[i], b->steps[i]) != 0) return false;
    }
    return true;
}

static void feIndexVars(const AST *n, CaseNameSet *out, bool inIndex) {
    if (!n) return;
    if (n->type == AST_VARIABLE && inIndex && n->token && n->token->value)
        caseSetAdd(out, n->token->value);
    if (n->type == AST_ARRAY_ACCESS) {
        feIndexVars(n->left, out, inIndex);
        for (int i = 0; i < n->child_count; i++) feIndexVars(n->children[i], out, true);
        return;
    }
    feIndexVars(n->left, out, inIndex);
    feIndexVars(n->right, out, inIndex);
    feIndexVars(n->extra, out, inIndex);
    for (int i = 0; i < n->child_count; i++) feIndexVars(n->children[i], out, inIndex);
}

/* The collection as the user wrote it: `self.items`, `rows[r]`. */
static void feRender(const AST *n, char *buf, size_t cap) {
    if (!n || cap == 0) return;
    size_t len = strlen(buf);
    if (n->type == AST_VARIABLE && n->token && n->token->value) {
        const char *name = strcmp(n->token->value, "myself") == 0 ? "self" : n->token->value;
        snprintf(buf + len, cap - len, "%s", name);
    } else if (n->type == AST_FIELD_ACCESS && n->token && n->token->value) {
        feRender(n->left, buf, cap);
        len = strlen(buf);
        snprintf(buf + len, cap - len, ".%s", n->token->value);
    } else if (n->type == AST_ARRAY_ACCESS) {
        feRender(n->left, buf, cap);
        for (int i = 0; i < n->child_count; i++) {
            const AST *ix = n->children[i];
            len = strlen(buf);
            const char *text = (ix && (ix->type == AST_VARIABLE || ix->type == AST_NUMBER) &&
                                ix->token && ix->token->value) ? ix->token->value : "i";
            snprintf(buf + len, cap - len, "[%s]", text);
        }
    }
}

typedef enum { FE_WRITE, FE_INDEX, FE_NESTED } FeWriteKind;

typedef struct {
    const AetherForeachSite *site;
    FePath coll;
    CaseNameSet indexVars;
    const AST **fns;
    int fnCount;
    bool reported;
} FeCtx;

static const AST *feFindFn(const FeCtx *c, const char *name) {
    for (int i = 0; i < c->fnCount; i++)
        if (c->fns[i]->token && c->fns[i]->token->value && strcmp(c->fns[i]->token->value, name) == 0)
            return c->fns[i];
    return NULL;
}

static void feReport(FeCtx *c, const AST *at, FeWriteKind kind, const char *what) {
    const AetherForeachSite *s = c->site;
    char coll[160] = "";
    feRender(s->coll, coll, sizeof(coll));
    const char *x = s->loopVar ? s->loopVar : "x";
    char detail[480], hint[640];
    if (kind == FE_INDEX) {
        snprintf(detail, sizeof(detail),
                 "`loop %s in %s` walks `%s`, and its body changes `%s` here, which picks the "
                 "collection: a loop may not change the collection it walks.",
                 x, coll, coll, what);
        snprintf(hint, sizeof(hint),
                 "copy it first, `let snapshot: %s = %s; loop %s in snapshot { ... }`, and the "
                 "body may then change `%s`.",
                 s->collType ? s->collType : "T[]", coll, x, what);
    } else {
        if (kind == FE_NESTED) {
            snprintf(detail, sizeof(detail),
                     "`loop %s in %s` walks `%s`, and its body calls `%s` here, a nested function "
                     "that changes it: a loop may not change the collection it walks.",
                     x, coll, coll, what);
        } else {
            snprintf(detail, sizeof(detail),
                     "`loop %s in %s` walks `%s`, and its body changes it here: a loop may not "
                     "change the collection it walks.",
                     x, coll, coll);
        }
        bool toon = s->collType && strcmp(s->collType, "ToonNode") == 0;
        char lenExpr[200], elemExpr[200];
        snprintf(lenExpr, sizeof(lenExpr), toon ? "toon_len(%s)" : "len(%s)", coll);
        if (toon) snprintf(elemExpr, sizeof(elemExpr), "toon_at(%s, i)", coll);
        else snprintf(elemExpr, sizeof(elemExpr), "%s[i]", coll);
        snprintf(hint, sizeof(hint),
                 "to walk it as the body changes it (a worklist, or updating elements), index "
                 "it: `let i: Int = 0; "
                 "loop i < %s { let %s: %s = %s; ... i = i + 1; }`; to go over the contents as "
                 "they were, copy them first: `let snapshot: %s = %s; loop %s in snapshot { ... }`.",
                 lenExpr, x, s->elemType ? s->elemType : "T", elemExpr,
                 s->collType ? s->collType : "T[]", coll, x);
    }
    int line = aetherAstNodeLine(at);
    fprintf(stderr, "%s:%d: [LOOP-001] Aether loop error: %s\n",
            g_aether_source_path ? g_aether_source_path : "<aether>", line > 0 ? line : s->line,
            detail);
    fprintf(stderr, "hint: %s\n", hint);
    aetherReportGuideHelp("LOOP-001");
    pascal_semantic_error_count++;
    c->reported = true;
}

static bool feTouches(const FeCtx *c, const AST *target) {
    FePath t;
    return fePath(target, &t) && fePathsOverlap(&t, &c->coll);
}

static const char *feIndexVarWritten(const FeCtx *c, const AST *target) {
    const char *unused = NULL;
    if (!target || target->type != AST_VARIABLE || !target->token || !target->token->value) return NULL;
    const char *name = target->token->value;
    return caseLookup(&c->indexVars, name, strlen(name), &unused) == 1 ? name : NULL;
}

static void feScan(FeCtx *c, const AST *n) {
    if (!n || c->reported) return;
    /* A record literal's `items: value` names a field, not a variable. */
    if (n->type == AST_ASSIGN && n->token && n->token->value && strcmp(n->token->value, ":") == 0) {
        feScan(c, n->right);
        return;
    }
    if (n->type == AST_ASSIGN && n->left) {
        const char *ix = NULL;
        if (feTouches(c, n->left)) { feReport(c, n, FE_WRITE, NULL); return; }
        if ((ix = feIndexVarWritten(c, n->left)) != NULL) { feReport(c, n, FE_INDEX, ix); return; }
    } else if (n->type == AST_PROCEDURE_CALL && n->token && n->token->value) {
        const char *name = n->token->value;
        if (strcasecmp(name, "setlength") == 0 && n->child_count > 0 && feTouches(c, n->children[0])) {
            feReport(c, n, FE_WRITE, NULL);
            return;
        }
        const AST *fn = strncmp(name, "__aether_", 9) == 0 ? feFindFn(c, name) : NULL;
        int arg = 0;
        for (int i = 0; fn && i < fn->child_count; i++) {
            const AST *pd = fn->children[i];
            if (!pd || pd->type != AST_VAR_DECL) continue;
            for (int k = 0; k < pd->child_count; k++, arg++) {
                if (!pd->by_ref || arg >= n->child_count) continue;
                const AST *a = n->children[arg];
                if (feTouches(c, a) || feIndexVarWritten(c, a)) {
                    feReport(c, n, FE_NESTED, aetherAstFunctionDisplayName(name));
                    return;
                }
            }
        }
    }
    feScan(c, n->left);
    feScan(c, n->right);
    feScan(c, n->extra);
    for (int i = 0; i < n->child_count; i++) feScan(c, n->children[i]);
}

static void feCollectFns(const AST *n, const AST ***fns, int *count, int *cap) {
    if (!n) return;
    if ((n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) && n->token &&
        n->token->value && strncmp(n->token->value, "__aether_", 9) == 0) {
        if (*count == *cap) {
            int ncap = *cap ? *cap * 2 : 16;
            const AST **g = (const AST **)realloc((void *)*fns, (size_t)ncap * sizeof(*g));
            if (!g) return;
            *fns = g;
            *cap = ncap;
        }
        (*fns)[(*count)++] = n;
    }
    feCollectFns(n->left, fns, count, cap);
    feCollectFns(n->right, fns, count, cap);
    feCollectFns(n->extra, fns, count, cap);
    for (int i = 0; i < n->child_count; i++) feCollectFns(n->children[i], fns, count, cap);
}

static void feWalkProgram(const AST *n, const AST **fns, int fnCount) {
    if (!n) return;
    const AetherForeachSite *site = aetherAstForeachSiteFor(n);
    if (site && site->coll) {
        FeCtx c;
        memset(&c, 0, sizeof(c));
        c.site = site;
        c.fns = fns;
        c.fnCount = fnCount;
        if (fePath(site->coll, &c.coll)) {
            feIndexVars(site->coll, &c.indexVars, false);
            feScan(&c, n);
        }
        free(c.indexVars.names);
    }
    feWalkProgram(n->left, fns, fnCount);
    feWalkProgram(n->right, fns, fnCount);
    feWalkProgram(n->extra, fns, fnCount);
    for (int i = 0; i < n->child_count; i++) feWalkProgram(n->children[i], fns, fnCount);
}

static void aetherValidateForeachWrites(const AST *root) {
    const AST **fns = NULL;
    int fnCount = 0, fnCap = 0;
    feCollectFns(root, &fns, &fnCount, &fnCap);
    feWalkProgram(root, fns, fnCount);
    free((void *)fns);
}

static void aetherValidateBareFieldNames(const AST *root) {
    BareFieldIndex ix;
    memset(&ix, 0, sizeof(ix));
    bareFieldCollect(&ix, root, false);
    if (ix.count > 0) bareFieldCheckMethods(&ix, root);
    for (int i = 0; i < ix.count; i++) free(ix.types[i].fields.names);
    free(ix.types);
    free(ix.globals.names);
}

/* Two functions (or two methods of a type) with one name, or names that
 * differ only by case. The engine keeps one routine per name, ignoring case,
 * so the later body silently replaced the earlier one for every call: with
 * `fn foo` and `fn Foo` both calls ran Foo. A prototype the forward scan
 * emits has no body and is not a declaration here. */
typedef struct {
    const char *name;
    int line;
} FnDeclSeen;

static void dupFnCollect(const AST *n, FnDeclSeen **seen, int *count, int *cap) {
    if (!n) return;
    if ((n->type == AST_FUNCTION_DECL || n->type == AST_PROCEDURE_DECL) && n->token &&
        n->token->value) {
        const AST *body = n->type == AST_FUNCTION_DECL ? n->extra : n->right;
        if (body) {
            const char *name = n->token->value;
            int line = n->token->line > 0 ? n->token->line : 1;
            for (int i = 0; i < *count; i++) {
                if (strcasecmp((*seen)[i].name, name) != 0) continue;
                const char *dot = strrchr(name, '.');
                const char *kind = dot ? "method" : "function";
                const char *shown = dot ? dot + 1 : name;
                const char *first = strrchr((*seen)[i].name, '.');
                first = first ? first + 1 : (*seen)[i].name;
                char detail[320];
                if (strcmp((*seen)[i].name, name) == 0) {
                    snprintf(detail, sizeof(detail),
                             "%s '%s' is declared twice (first on line %d): every call would "
                             "reach only the last one.",
                             kind, shown, (*seen)[i].line);
                } else {
                    snprintf(detail, sizeof(detail),
                             "%s '%s' and '%s' (line %d) differ only by case: two names may "
                             "not differ only by case, and every call would reach only one.",
                             kind, shown, first, (*seen)[i].line);
                }
                reportNameCollision(line, detail, "rename one of them.");
                break;
            }
            if (*count == *cap) {
                int ncap = *cap ? *cap * 2 : 32;
                FnDeclSeen *grown = (FnDeclSeen *)realloc(*seen, (size_t)ncap * sizeof(**seen));
                if (!grown) return;
                *seen = grown;
                *cap = ncap;
            }
            (*seen)[*count].name = name;
            (*seen)[*count].line = line;
            (*count)++;
        }
    }
    dupFnCollect(n->left, seen, count, cap);
    dupFnCollect(n->right, seen, count, cap);
    dupFnCollect(n->extra, seen, count, cap);
    for (int i = 0; i < n->child_count; i++) dupFnCollect(n->children[i], seen, count, cap);
}

static void aetherValidateDuplicateFunctions(const AST *root) {
    FnDeclSeen *seen = NULL;
    int count = 0, cap = 0;
    dupFnCollect(root, &seen, &count, &cap);
    free(seen);
}

static void aetherWalkLoopExits(const AST *n, int loopDepth) {
    if (!n) return;
    switch (n->type) {
        case AST_FUNCTION_DECL:
        case AST_PROCEDURE_DECL:
            loopDepth = 0;
            break;
        case AST_WHILE:
        case AST_REPEAT:
        case AST_FOR_TO:
        case AST_FOR_DOWNTO:
            aetherWalkLoopExits(n->left, loopDepth);
            aetherWalkLoopExits(n->right, loopDepth + 1);
            aetherWalkLoopExits(n->extra, loopDepth + 1);
            for (int i = 0; i < n->child_count; i++) aetherWalkLoopExits(n->children[i], loopDepth + 1);
            return;
        case AST_BREAK:
        case AST_CONTINUE:
            if (loopDepth == 0) {
                const char *word = n->type == AST_BREAK ? "break" : "continue";
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "`%s` is outside any loop: it only works inside `loop { }` (or "
                         "`while`/`for`). To leave a function early, use `ret`.",
                         word);
                aetherSemanticReportCoded("SYN-001", "parser",
                                          n->token && n->token->line > 0 ? n->token->line : 1,
                                          detail, 1);
            }
            return;
        default:
            break;
    }
    aetherWalkLoopExits(n->left, loopDepth);
    aetherWalkLoopExits(n->right, loopDepth);
    aetherWalkLoopExits(n->extra, loopDepth);
    for (int i = 0; i < n->child_count; i++) aetherWalkLoopExits(n->children[i], loopDepth);
}

static void aetherValidateParRaces(const AST *root) {
    ParIndex ix;
    memset(&ix, 0, sizeof(ix));
    parIndexProgram(&ix, root, true);
    ParAliases al;
    memset(&al, 0, sizeof(al));
    parWalk(&ix, &al, root);
    parSetFree(&ix.globals);
    free(ix.fns);
}

/* --------------------------------------------------------------------------
 * TYPE-002: a type name in a declaration that never resolved.
 *
 * buildTypeNode() emits a bare AST_TYPE_REFERENCE at TYPE_UNKNOWN for any type
 * name that is neither a builtin nor an already-registered `type`. That
 * fallback is load bearing: a signature may legitimately name a record
 * declared further down the file, or one exported by a module that has not
 * been loaded yet. What was missing is anything that later went back and asked
 * whether the name ever did resolve.
 *
 * An unresolved annotation is not inert. It lowers to a slot built by
 * makeValueForType(TYPE_UNKNOWN), which accepts whatever value it is handed,
 * so the declared type does no checking at all -- `fn f() -> Strng { ret 1; }`
 * compiles, runs, and returns an Int. A misspelled or foreign type name
 * therefore produced a working program and not one diagnostic, which is the
 * one failure shape the coded-error loop cannot help a reader recover from.
 *
 * Runs after reaPerformSemanticAnalysis so every imported module's types are
 * registered: a name still unresolved at this point is unresolvable, not
 * merely early. It still runs before the bytecode compiler -- main() re-checks
 * the error count in between -- which is also what stops the backend's
 * "makeValueForType called with unhandled type 0" note from reaching the user
 * on this path, since the program no longer gets that far.
 * -------------------------------------------------------------------------- */

/* The Aether spelling to suggest for a type name imported wholesale from
 * another language. NULL when there is no single obvious replacement. */
static const char *aetherSuggestedTypeName(const char *name) {
    static const struct { const char *wrong; const char *right; } table[] = {
        { "Str",       "Text" },
        { "Character", "Text" },
        { "Char",      "Text" },
        { "Double",    "Real" },
        { "Float64",   "Real" },
        { "Number",    "Real" },
        { "Num",       "Real" },
        { "Single",    "Real" },
        { "Integer",   "Int" },
        { "Int32",     "Int" },
        { "Int64",     "Int" },
        { "Long",      "Int" },
        { "Boolean",   "Bool" },
        { "Unit",      "Void" },
        { "None",      "Void" },
        { "Nothing",   "Void" },
        /* Rea/Pascal scalar keywords. rea's own type resolver accepts these
         * case-insensitively, which is exactly why they need naming here:
         * without a TYPE-002 they used to slip through to codegen. */
        { "Word",      "Int" },
        { "Byte",      "Int" },
        { "Short",     "Int" },
        { "Int8",      "Int" },
        { "Int16",     "Int" },
        { "UInt8",     "Int" },
        { "UInt16",    "Int" },
        { "UInt32",    "Int" },
        { "UInt64",    "Int" },
        { "Float32",   "Real" },
        /* Lowercase spellings of Aether's own names (the canonical, capitalized
         * spelling never reaches this table). */
        { "int",       "Int" },
        { "real",      "Real" },
        { "float",     "Real" },
        { "text",      "Text" },
        { "string",    "Text" },
        { "bool",      "Bool" },
        { "void",      "Void" },
        { "mstream",   "MStream" },
        { "toondoc",   "ToonDoc" },
        { "toonnode",  "ToonNode" },
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcasecmp(name, table[i].wrong) == 0) {
            return table[i].right;
        }
    }
    return NULL;
}

/* Container type names have no one-word replacement: an array is spelled `T[]`
 * and there is no map/dict/set type at all, so the fix is a different shape
 * rather than a different word. NULL when the name is not a container. */
static const char *aetherCollectionTypeAdvice(const char *name) {
    static const char *arrayNames[] = { "List", "Array", "Vec", "Vector", "Seq", "Slice" };
    static const char *mapNames[]   = { "Map", "Dict", "Dictionary", "HashMap", "Set" };
    size_t i;
    for (i = 0; i < sizeof(arrayNames) / sizeof(arrayNames[0]); i++) {
        if (strcasecmp(name, arrayNames[i]) == 0) {
            return "an array is spelled with the element type, e.g. `Int[]` or `Text[]`.";
        }
    }
    /* D55/D60: the File handle type is gone with assign/reset/readln(f). */
    if (strcasecmp(name, "File") == 0 || strcasecmp(name, "Text_File") == 0 ||
        strcasecmp(name, "TextFile") == 0) {
        return "Aether has no file handles: read a whole file with `read_file(path)` (Text) "
               "or `read_lines(path)` (Text[]); write with `write_file(path, text)` or "
               "`append_file(path, text)`.";
    }
    for (i = 0; i < sizeof(mapNames) / sizeof(mapNames[0]); i++) {
        if (strcasecmp(name, mapNames[i]) == 0) {
            return "a map is spelled `Map[Text, T]` (Text keys), e.g. `Map[Text, Int]`; "
                   "for a set of Text, use `Map[Text, Bool]`.";
        }
    }
    return NULL;
}

/* Every function declaration reaches this pass twice -- the parser emits a
 * bodiless prototype alongside the definition, and both carry the same
 * signature nodes -- so a bad parameter or return type would otherwise be
 * reported twice for one mistake. Keyed on line + name + context rather than
 * on the AST node so the two copies collapse while genuinely distinct sites
 * survive: two parameters of the same bad type differ by context, and a
 * hand-written forward declaration differs by line and is worth flagging
 * separately, since both spellings do need fixing. */
typedef struct AetherReportedTypeTable {
    char **keys;
    size_t count;
    size_t cap;
} AetherReportedTypeTable;

static AetherReportedTypeTable g_aether_reported_types = {0};

static int aetherMarkTypeReported(const char *key) {
    size_t i;
    char *copy;
    for (i = 0; i < g_aether_reported_types.count; i++) {
        if (strcmp(g_aether_reported_types.keys[i], key) == 0) {
            return 0;
        }
    }
    if (g_aether_reported_types.count == g_aether_reported_types.cap) {
        size_t cap = g_aether_reported_types.cap ? g_aether_reported_types.cap * 2 : 8;
        char **grown = realloc(g_aether_reported_types.keys, cap * sizeof(char *));
        if (!grown) {
            return 1;  /* out of memory: report rather than swallow the error */
        }
        g_aether_reported_types.keys = grown;
        g_aether_reported_types.cap = cap;
    }
    copy = strdup(key);
    if (!copy) {
        return 1;
    }
    g_aether_reported_types.keys[g_aether_reported_types.count++] = copy;
    return 1;
}

static void aetherFreeReportedTypeTable(void) {
    size_t i;
    for (i = 0; i < g_aether_reported_types.count; i++) {
        free(g_aether_reported_types.keys[i]);
    }
    free(g_aether_reported_types.keys);
    g_aether_reported_types.keys = NULL;
    g_aether_reported_types.count = 0;
    g_aether_reported_types.cap = 0;
}

static void aetherReportUnknownType(const AST *node, const char *ctx) {
    const char *name = (node && node->token) ? node->token->value : NULL;
    const char *suggestion;
    const char *advice;
    char detail[384];
    char key[448];
    if (!name) {
        return;
    }
    snprintf(key, sizeof(key), "%d|%s|%s", aetherAstNodeLine(node), name, ctx ? ctx : "");
    if (!aetherMarkTypeReported(key)) {
        return;
    }
    suggestion = aetherSuggestedTypeName(name);
    advice = suggestion ? NULL : aetherCollectionTypeAdvice(name);
    if (suggestion) {
        snprintf(detail, sizeof(detail),
                 "unknown type '%s' in %s; did you mean '%s'?", name, ctx, suggestion);
    } else if (advice) {
        snprintf(detail, sizeof(detail),
                 "unknown type '%s' in %s; %s", name, ctx, advice);
    } else {
        snprintf(detail, sizeof(detail),
                 "unknown type '%s' in %s; it is neither a builtin type (Int, Real, "
                 "Text, Bool, Void) nor a `type` declared in this program or an "
                 "imported module.", name, ctx);
    }
    reportAetherErrorCoded("TYPE-002", "unknown-type", aetherAstNodeLine(node), detail);
}

/* Descends one declaration's type slot -- through the AST_ARRAY_TYPE and
 * AST_POINTER_TYPE wrappers that `T[]` and record types build -- reporting
 * every name in it that never resolved. Deliberately refuses to walk anything
 * that is not itself a type node, so an array type's bounds expression cannot
 * drag the scan into ordinary code. */
/* How a declaration's type name resolves:
 *   0 -- unknown: not an Aether builtin, and nothing registered under it (yet)
 *   1 -- a real type: an Aether builtin spelling, or a `type` registered by this
 *        program or an imported module
 *   2 -- one of rea's scalar keywords (`char`, `byte`, `word`, `str`, ...): the
 *        shared resolver accepts these case-insensitively by handing back a
 *        transient stub instead of NULL, which is how `Char` used to slip past
 *        this check and reach codegen as an untyped slot. Never a valid Aether
 *        type name, and decidable before module loading.
 * lookupType is rea's resolver here; its stub is a token-less AST_VARIABLE
 * allocated for the caller, so it is freed on the spot. A registered type is
 * table-owned and must not be. */
static int aetherTypeNameKind(const char *name) {
    AST *resolved;
    if (aetherAstIsBuiltinTypeName(name)) {
        return 1;
    }
    resolved = lookupType(name);
    if (!resolved) {
        return 0;
    }
    if (resolved->type == AST_VARIABLE && !resolved->token) {
        freeAST(resolved);
        return 2;
    }
    return 1;
}

/* When set, aetherCheckTypeSlot reports only what is decidable before module
 * loading: the rea-scalar stubs (kind 2) and case variants of Aether's own
 * type names (`string`, `INT`, `text`, ...), which no module could sensibly
 * export. Any other unknown name may still be an import that has not been
 * registered yet and waits for the second stage. */
static int g_aether_type_check_stubs_only = 0;

static int aetherIsCaseVariantOfBuiltinType(const char *name) {
    static const char *const builtins[] = {
        "Int", "Real", "Text", "Bool", "Void", "Float", "String", "TOON",
        "ToonDoc", "ToonNode", "MStream", "File"
    };
    size_t i;
    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcasecmp(name, builtins[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void aetherCheckTypeSlot(const AST *node, const char *ctx) {
    int i;
    if (!node) {
        return;
    }
    if (node->type == AST_TYPE_REFERENCE) {
        if (node->token && node->token->value) {
            int kind = aetherTypeNameKind(node->token->value);
            if (kind == 2 ||
                (kind == 0 && (!g_aether_type_check_stubs_only ||
                               aetherIsCaseVariantOfBuiltinType(node->token->value)))) {
                aetherReportUnknownType(node, ctx);
            }
        }
        return;
    }
    if (node->type != AST_ARRAY_TYPE && node->type != AST_POINTER_TYPE) {
        return;
    }
    aetherCheckTypeSlot(node->left, ctx);
    aetherCheckTypeSlot(node->right, ctx);
    aetherCheckTypeSlot(node->extra, ctx);
    for (i = 0; i < node->child_count; i++) {
        aetherCheckTypeSlot(node->children[i], ctx);
    }
}

#define AETHER_TYPE002_CTX_MAX 192

/* `fnName` is the enclosing function, carried down purely so a bad type on a
 * local binding can say which function it is in. */
static void aetherWalkUnknownTypes(const AST *node, const char *fnName) {
    char ctx[AETHER_TYPE002_CTX_MAX];
    int i;
    if (!node) {
        return;
    }

    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        /* Signature slots differ by kind: a value-returning function keeps its
         * return type on `right` and its body on `extra`, a Void one has no
         * return type and puts the body on `right`. Parameters are AST_VAR_DECL
         * children in both, each with the parameter name as children[0] and its
         * type on `right`. */
        const char *name = (node->token && node->token->value) ? node->token->value : NULL;
        for (i = 0; i < node->child_count; i++) {
            const AST *param = node->children[i];
            const char *pname = NULL;
            if (!param || param->type != AST_VAR_DECL) {
                continue;
            }
            if (param->child_count > 0 && param->children[0] && param->children[0]->token) {
                pname = param->children[0]->token->value;
            }
            if (pname && name) {
                snprintf(ctx, sizeof(ctx), "parameter '%s' of '%s'", pname, name);
            } else if (pname) {
                snprintf(ctx, sizeof(ctx), "parameter '%s'", pname);
            } else {
                snprintf(ctx, sizeof(ctx), "a parameter");
            }
            aetherCheckTypeSlot(param->right, ctx);
        }
        if (node->type == AST_FUNCTION_DECL) {
            if (name) {
                snprintf(ctx, sizeof(ctx), "the return type of '%s'", name);
            } else {
                snprintf(ctx, sizeof(ctx), "a return type");
            }
            aetherCheckTypeSlot(node->right, ctx);
            aetherWalkUnknownTypes(node->extra, name);
        } else {
            aetherWalkUnknownTypes(node->right, name);
        }
        aetherWalkUnknownTypes(node->left, name);
        return;
    }

    if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
        /* A `let`/`const` binding or a record field. AST_CONST_DECL carries its
         * type on `left`; everything else on `right`. */
        const AST *typeSlot = (node->type == AST_CONST_DECL) ? node->left : node->right;
        const char *vname = NULL;
        if (node->child_count > 0 && node->children[0] && node->children[0]->token) {
            vname = node->children[0]->token->value;
        } else if (node->token) {
            vname = node->token->value;
        }
        if (vname && fnName) {
            snprintf(ctx, sizeof(ctx), "the declaration of '%s' in '%s'", vname, fnName);
        } else if (vname) {
            snprintf(ctx, sizeof(ctx), "the declaration of '%s'", vname);
        } else {
            snprintf(ctx, sizeof(ctx), "a declaration");
        }
        aetherCheckTypeSlot(typeSlot, ctx);
        /* Falls through to the generic recursion below for the initializer;
         * re-reaching the type slot there is harmless, since only
         * aetherCheckTypeSlot reports. */
    }

    aetherWalkUnknownTypes(node->left, fnName);
    aetherWalkUnknownTypes(node->right, fnName);
    aetherWalkUnknownTypes(node->extra, fnName);
    for (i = 0; i < node->child_count; i++) {
        aetherWalkUnknownTypes(node->children[i], fnName);
    }
}

static void aetherValidateTypeNames(const AST *root) {
    int moduleCount;
    int i;
    aetherWalkUnknownTypes(root, NULL);
    aetherFreeReportedTypeTable();
    /* An imported module is a separate AST the main walk never reaches, so a
     * typo'd annotation inside `mod M { export type T { x: Bogus[]; } }` would
     * otherwise stay silent -- the same hole this check exists to close, one
     * file over. Reachable here only because this stage runs after
     * reaPerformSemanticAnalysis, which is what loads them. Each module is
     * attributed to its own path so the diagnostic points at the module file,
     * not the importer, and the duplicate table is per file so the same
     * (line, name) in two files cannot mask one another. The count covers
     * transitive imports (loadModuleRecursive). */
    moduleCount = aetherGetLoadedModuleCount();
    for (i = 0; i < moduleCount; i++) {
        const char *savedPath = g_aether_source_path;
        const char *path = aetherGetModulePath(i);
        if (path) {
            g_aether_source_path = path;
        }
        aetherWalkUnknownTypes(aetherGetModuleAST(i), NULL);
        g_aether_source_path = savedPath;
        aetherFreeReportedTypeTable();
    }
}

/* TYPE-002, first stage: the names rea's resolver would otherwise accept as
 * scalars (`Char`, `Byte`, `Word`, `Str`, `int`, ...). These are never valid
 * Aether types, so they are reported BEFORE reaPerformSemanticAnalysis runs --
 * that pass would emit its own uncoded "identifier 'Char' not in scope" (or,
 * for a parameter, let the declaration reach codegen and print an internal
 * makeValueForType warning), and the coded diagnostic must be the one the
 * reader sees. Everything else waits for the second stage, after modules are
 * loaded, where an unresolved name is known to be wrong rather than early. */
static void aetherValidateReaScalarTypeNames(const AST *root) {
    g_aether_type_check_stubs_only = 1;
    aetherWalkUnknownTypes(root, NULL);
    g_aether_type_check_stubs_only = 0;
    aetherFreeReportedTypeTable();
}

void aetherPerformSemanticAnalysis(AST *root) {
    const char *source = aetherGetLastSource();
    int errorCountBefore = pascal_semantic_error_count;

    if (source) {
        char *sanitized = sanitizeAetherSourceForSemanticScan(source);
        const char *scanSource = sanitized ? sanitized : source;
        AetherOpaqueBindingTable opaqueBindings = {0};
        AetherScalarBindingTable scalarBindings = {0};
        size_t lineCount = 0;
        int fnCount = 0;
        int *lineFnIds;
        validateContractAnnotations(scanSource);
        /* Function-scoped type flow: collect + validate the top level first
         * (its const/let bindings are visible program-wide), then re-run per
         * function against a fresh copy of the top-level tables plus only
         * that function's own locals -- so one function's `let v: Int` can
         * neither type another function's `v` nor clobber a global it merely
         * shadows. On allocation failure lineFnIds is NULL and the passes
         * degrade to the old whole-source flat scan. */
        lineFnIds = computeLineFunctionIds(scanSource, &lineCount, &fnCount);
        collectOpaqueBindings(scanSource, &opaqueBindings, lineFnIds, lineCount, 0);
        collectScalarBindings(scanSource, &scalarBindings, lineFnIds, lineCount, 0);
        validateAetherSource(scanSource, &opaqueBindings, &scalarBindings,
                             lineFnIds, lineCount, 0);
        if (lineFnIds) {
            int fid;
            for (fid = 1; fid <= fnCount; fid++) {
                AetherOpaqueBindingTable fnOpaque = {0};
                AetherScalarBindingTable fnScalar = {0};
                if (copyOpaqueBindingTable(&fnOpaque, &opaqueBindings) &&
                    copyScalarBindingTable(&fnScalar, &scalarBindings)) {
                    collectOpaqueBindings(scanSource, &fnOpaque, lineFnIds, lineCount, fid);
                    collectScalarBindings(scanSource, &fnScalar, lineFnIds, lineCount, fid);
                    validateAetherSource(scanSource, &fnOpaque, &fnScalar,
                                         lineFnIds, lineCount, fid);
                }
                freeScalarBindingTable(&fnScalar);
                freeOpaqueBindingTable(&fnOpaque);
            }
            free(lineFnIds);
        }
        validateNoDuplicateLocals(scanSource);
        freeScalarBindingTable(&scalarBindings);
        freeOpaqueBindingTable(&opaqueBindings);
        free(sanitized);
    }
    /* Effect fence + purity run on the AST (not the source text), so they see
     * the program the compiler will actually build. */
    aetherValidateEffectsAndPurity(root);
    /* D70: a reference spelled differently from its declaration. */
    aetherValidateNameCase(root);
    aetherValidateBareFieldNames(root);
    aetherValidateForeachWrites(root);
    aetherValidateDuplicateFunctions(root);
    if (pascal_semantic_error_count > errorCountBefore) {
        return;
    }
    /* TYPE-002, stage 1: rea's scalar keywords used as Aether type names. Must
     * precede reaPerformSemanticAnalysis (see aetherValidateReaScalarTypeNames)
     * and gates it, so the coded diagnostic is the only one printed. */
    aetherValidateReaScalarTypeNames(root);
    if (pascal_semantic_error_count > errorCountBefore) {
        return;
    }
    aetherRunExperimentsBeforeRea(root);
    reaPerformSemanticAnalysis(root);
    /* TYPE-002, stage 2, runs here and not with the source-text passes above:
     * imported modules are loaded by the pass that just ran, so this is the
     * first point at which any other unresolved type name is known to be wrong
     * rather than early. */
    aetherValidateTypeNames(root);
    /* D69: imported modules are in now; check their effects and the
     * importer's @pure calls into them. */
    aetherValidateModuleEffects(root);
    /* NARROW-001 is likewise a warning and never touches the error counter.
     * It must run AFTER reaPerformSemanticAnalysis: a bare AST_VARIABLE
     * reference and a call to a user function both still carry TYPE_UNKNOWN
     * until that pass resolves them, so running earlier saw every assignment
     * target as untyped and fired only on `let` declarations, whose type comes
     * straight off the parsed declaration. */
    aetherWalkNarrowing(root);
    /* AETHER_EXPERIMENT arms (experiment.c); a no-op with the variable unset. */
    aetherRunExperiments(root);
    /* Coded errors hosted on the type oracle (types.c aetherTypedRules), on a
     * program the passes above accepted. */
    if (pascal_semantic_error_count == errorCountBefore) {
        aetherValidateArrayParamWrites(root);
        /* D69: an imported module's functions follow ARR-001 too, reported
         * under the module's own path. */
        int moduleCount = aetherGetLoadedModuleCount();
        for (int i = 0; i < moduleCount; i++) {
            AST *m = aetherGetModuleAST(i);
            if (!m || m == root) continue;
            const char *savedPath = g_aether_source_path;
            const char *path = aetherGetModulePath(i);
            if (path) g_aether_source_path = path;
            aetherValidateArrayParamWrites(m);
            g_aether_source_path = savedPath;
        }
    }
    if (pascal_semantic_error_count == errorCountBefore) aetherValidateParamCursor(root);
    if (pascal_semantic_error_count == errorCountBefore) aetherWalkLoopExits(root, 0);
    if (pascal_semantic_error_count == errorCountBefore) aetherValidateParRaces(root);
    if (pascal_semantic_error_count == errorCountBefore) aetherTypedRules(root);
    /* The type oracle's pass (types.c). It runs only when AETHER_DUMP_TYPES is
     * set and reports no diagnostics: W7-24a ships the oracle with no rules. */
    aetherTypedPass(root);
}

void aetherSemanticReportCoded(const char *code, const char *kind, int line,
                               const char *detail, int isError) {
    if (isError) reportAetherErrorCoded(code, kind, line, detail);
    else reportAetherWarningCoded(code, kind, line, detail);
}

void aetherSemanticSetSourcePath(const char *path) {
    g_aether_source_path = path;
    reaSemanticSetSourcePath(path);
}

const char *aetherSemanticGetSourcePath(void) {
    return g_aether_source_path;
}

const char *aetherSemanticSwapReportPath(const char *path) {
    const char *previous = g_aether_source_path;
    g_aether_source_path = path;
    return previous;
}

int aetherGetLoadedModuleCount(void) {
    return reaGetLoadedModuleCount();
}

AST *aetherGetModuleAST(int index) {
    return reaGetModuleAST(index);
}

const char *aetherGetModulePath(int index) {
    return reaGetModulePath(index);
}

const char *aetherGetModuleName(int index) {
    return reaGetModuleName(index);
}

char *aetherResolveImportPath(const char *path) {
    return reaResolveImportPath(path);
}

void aetherSemanticResetState(void) {
    g_aether_source_path = NULL;
    free(g_declaredValues.names);
    memset(&g_declaredValues, 0, sizeof(g_declaredValues));
    reaSemanticResetState();
}
