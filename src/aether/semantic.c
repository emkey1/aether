#include "aether/semantic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/globals.h"
#include "backend_ast/builtin.h"
#include "aether/diagnostics.h"
#include "aether/parser.h"
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
    /* Memory-stream consumers: first argument must be an MStream handle. */
    if ((len == 13 && strncasecmp(name, "mstreambuffer", len) == 0) ||
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
    if ((len == 13 && strncasecmp(name, "mstreamcreate", len) == 0) ||
        (len == 17 && strncasecmp(name, "mstreamfromstring", len) == 0) ||
        (len == 13 && strncasecmp(name, "socketreceive", len) == 0)) {
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
    if (!expectedType) {
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
        if (strcmp(lhsBinding->typeName, rhsExprType) == 0) {
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
        if (strcmp(lhsBinding->typeName, rhsExprType) == 0) {
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
    if (strcmp(lhsBinding->typeName, rhsBinding->typeName) == 0) {
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

                if (findOpaqueBinding(opaqueBindings, start, nameLen) != NULL) {
                    const char *prevSig = previousSignificantChar(lineStart, start);
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

typedef struct {
    char *name;
    int depth;
} AetherLocalDecl;

// Conservative NAME-001 pre-flight: flag a `let`/`const` local redeclared in the
// SAME lexical scope. Sound by construction — shadowing in a nested scope and
// name reuse across sibling scopes are allowed, and tuple/loop/parameter names
// are not tracked — so anything it misses still trips the bytecode compiler's
// own "duplicate variable" backstop. Reports via reportAetherError (kind
// "redeclaration" -> NAME-001), which increments the semantic error count so the
// compile aborts before codegen: one message, with a code and a guide pointer.
// `source` is the sanitized scan source (comments already neutralized); string
// literals are still present, so we skip them ourselves to keep brace depth
// honest.
static void validateNoDuplicateLocals(const char *source) {
    const char *cursor = source;
    int line = 1;
    int depth = 0;
    int atStmtStart = 1;
    AetherLocalDecl *decls = NULL;
    size_t count = 0;
    size_t cap = 0;

    if (!source) {
        return;
    }

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
            while (count > 0 && decls[count - 1].depth > depth) {
                free(decls[count - 1].name);
                count--;
            }
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
            int isConst;

            while (*lineEnd && *lineEnd != '\n') {
                lineEnd++;
            }
            isConst = startsWithWord(cursor, lineEnd, "const");
            if (isConst || startsWithWord(cursor, lineEnd, "let")) {
                const char *scan = skipInlineSpaces(cursor + (isConst ? 5 : 3), lineEnd);
                const char *nameStart;

                if (startsWithWord(scan, lineEnd, "mut")) {
                    scan = skipInlineSpaces(scan + 3, lineEnd);
                }
                nameStart = scan;
                while (scan < lineEnd &&
                       (isalnum((unsigned char)*scan) || *scan == '_')) {
                    scan++;
                }
                if (scan > nameStart) {
                    char *name = dupRange(nameStart, scan);
                    if (name) {
                        int dup = 0;
                        size_t k;

                        for (k = 0; k < count; k++) {
                            if (decls[k].depth == depth &&
                                strcmp(decls[k].name, name) == 0) {
                                dup = 1;
                                break;
                            }
                        }
                        if (dup) {
                            char detail[256];
                            snprintf(detail, sizeof(detail),
                                     "local '%s' is already declared in this scope.",
                                     name);
                            reportAetherError("redeclaration", line, detail);
                            free(name);
                        } else {
                            if (count == cap) {
                                size_t newCap = cap ? cap * 2 : 16;
                                AetherLocalDecl *grown = (AetherLocalDecl *)realloc(
                                    decls, newCap * sizeof(*grown));
                                if (!grown) {
                                    free(name);
                                    break;
                                }
                                decls = grown;
                                cap = newCap;
                            }
                            decls[count].name = name;
                            decls[count].depth = depth;
                            count++;
                        }
                    }
                }
                cursor = scan;
                atStmtStart = 0;
                continue;
            }
        }

        atStmtStart = 0;
        cursor++;
    }

    {
        size_t k;
        for (k = 0; k < count; k++) {
            free(decls[k].name);
        }
    }
    free(decls);
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
    const char *dot = name ? strrchr(name, '.') : NULL;
    return (dot && dot[1]) ? dot + 1 : name;
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
        display = canonical;
    }
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
            if (aetherAstLookupFunctionPurity(canonical, &calleePure) && !calleePure) {
                snprintf(detail,
                         sizeof(detail),
                         "pure function '%s' cannot call non-pure function '%s'.",
                         pureFnName,
                         display);
                reportAetherError("purity", line, detail);
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
    aetherWalkEffectsAndPurity(root, 0, NULL);
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
    if (pascal_semantic_error_count > errorCountBefore) {
        return;
    }
    reaPerformSemanticAnalysis(root);
}

void aetherSemanticSetSourcePath(const char *path) {
    g_aether_source_path = path;
    reaSemanticSetSourcePath(path);
}

const char *aetherSemanticGetSourcePath(void) {
    return g_aether_source_path;
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
    reaSemanticResetState();
}
