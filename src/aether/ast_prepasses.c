/*
 * src/aether/ast_prepasses.c
 *
 * Self-contained source pre-passes for the Aether AST frontend.
 *
 * The AST parser (ast_parser.c) needs four source-level transforms before it
 * lexes a program:
 *   - TOON `toon:` blocks lowered to escaped string literals
 *   - stdlib / TOON / capability builtin call spellings lowered to the canonical
 *     pscal builtins (toon_* -> Yyjson*, has_toon/string_eq/print/len/... )
 *   - the context-free `string_eq(a, b)` inline alias lowered to `(a == b)`
 *   - imported-module exported binding/return types collected for `use`/`mod`
 *
 * These exact behaviors also exist in the text rewriter (translate.c). To keep
 * the AST path free of any dependency on the rewriter (so the rewriter's
 * line-based fragility cannot leak into AST parsing), the logic is reproduced
 * here as standalone code: byte-for-byte the same observable transforms, but in
 * a translation unit the AST parser owns. Nothing here calls into translate.c,
 * and translate.c is left untouched. The public entry points are the
 * aetherAstPrepass* / aetherAstCollectImportedTypes functions declared in
 * ast_prepasses.h.
 */

#include "aether/ast_prepasses.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether/diagnostics.h"
#include "aether/parser.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Source line the builtin-alias pre-pass is currently rewriting, so an alias
 * replacement can be recorded (line, canonical -> surface) for diagnostics.
 * 0 = no line context (e.g. contract-expression segment re-aliasing); such
 * rewrites are not recorded. Set/cleared by aetherAstPrepassBuiltins. */
static int g_aetherPrepassAliasLine = 0;


typedef struct Buffer {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

/* Append the canonical spelling for an aliased builtin call and, when line
 * context exists, record the surface spelling the user wrote so semantic
 * diagnostics (FX-001/ANN-001) can quote it. */
static int aetherAliasAppendRecorded(Buffer *out,
                                     const char *surfaceStart,
                                     size_t surfaceLen,
                                     const char *canonical);

typedef struct JsonAliasState {
    int needed;
    int alreadyImported;
} JsonAliasState;

typedef struct AetherBinding {
    char *name;
    char *typeName;
} AetherBinding;

typedef struct AetherBindingTable {
    AetherBinding *items;
    size_t count;
    size_t cap;
} AetherBindingTable;

typedef struct AetherFunctionSig {
    char *name;
    char *returnType;
} AetherFunctionSig;

typedef struct AetherFunctionTable {
    AetherFunctionSig *items;
    size_t count;
    size_t cap;
} AetherFunctionTable;

typedef struct AetherFieldSig {
    char *typeName;
    char *fieldName;
    char *fieldType;
} AetherFieldSig;

typedef struct AetherFieldTable {
    AetherFieldSig *items;
    size_t count;
    size_t cap;
} AetherFieldTable;

typedef struct ToonLiteralBinding {
    char *name;
    char *literal;
} ToonLiteralBinding;

typedef struct ToonLiteralTable {
    ToonLiteralBinding *items;
    size_t count;
    size_t cap;
} ToonLiteralTable;

/* ---- forward declarations ---- */
static int bufferEnsure(Buffer *buf, size_t extra);
static int bufferAppendN(Buffer *buf, const char *text, size_t len);
static int bufferAppend(Buffer *buf, const char *text);
static char *dupRange(const char *start, const char *end);
static char *dupCString(const char *text);
static char *readTextFile(const char *path);
static char *trimmedCopy(const char *start, const char *end);
static const char *skipSpaces(const char *p);
static const char *skipSpacesInRange(const char *p, const char *end);
static char *resolveRelativePath(const char *sourcePath, const char *importPath);
static const char *findCharInRange(const char *start, const char *end, char target);
static const char *findMatchingCloseParen(const char *open, const char *end);
static const char *findSubstringInRange(const char *start, const char *end, const char *needle);
static void reportAetherRewriteError(const char *path,
                                     int line,
                                     const char *kind,
                                     const char *detail,
                                     const char *hint);
static int isIdentifierChar(unsigned char ch);
static int bufferAppendEscapedStringLiteral(Buffer *buf, const char *text, size_t len);
static const char *findToonMarker(const char *lineStart, const char *lineEnd);
static const char *findLineCommentStartInRange(const char *start, const char *end);
static size_t leadingIndentWidth(const char *lineStart, const char *lineEnd);
static char *preprocessToonBlocks(const char *source, const char *path);
static void freeAetherBindingTable(AetherBindingTable *table);
static void freeAetherFunctionTable(AetherFunctionTable *table);
static void freeAetherFieldTable(AetherFieldTable *table);
static int ensureAetherBindingTable(AetherBindingTable *table, size_t extra);
static int ensureAetherFunctionTable(AetherFunctionTable *table, size_t extra);
static int setAetherBindingType(AetherBindingTable *table, const char *name, const char *typeName);
static int setAetherFunctionReturnType(AetherFunctionTable *table,
                                       const char *name,
                                       const char *returnType);
static const char *findAetherBindingType(const AetherBindingTable *table, const char *name, size_t len);
static const char *findAetherFunctionReturnType(const AetherFunctionTable *table,
                                                const char *name,
                                                size_t len);
static const char *findAetherFieldType(const AetherFieldTable *table,
                                       const char *typeName,
                                       const char *fieldName,
                                       size_t fieldLen);
static void freeToonLiteralTable(ToonLiteralTable *table);
static int ensureToonLiteralTable(ToonLiteralTable *table, size_t extra);
static int setToonLiteralBinding(ToonLiteralTable *table, const char *name, const char *literal);
static const char *findToonLiteralBinding(const ToonLiteralTable *table, const char *name, size_t len);
static void clearToonLiteralBinding(ToonLiteralTable *table, const char *name, size_t len);
static int isSupportedToonBindingType(const char *typeStart, const char *typeEnd);
static int isAetherBoolLiteral(const char *start, const char *end);
static int inferNumericLiteralType(const char *start, const char *end, const char **outTypeName);
static const char *inferHelperReturnTypeName(const char *nameStart, size_t nameLen);
static const char *inferObjectInitTypeName(const char *start, const char *end);
static char *inferNewObjectTypeName(const char *start, const char *end);
static char *composeQualifiedLookup(const char *left,
                                    size_t leftLen,
                                    const char *right,
                                    size_t rightLen);
static int isAetherNumericTypeName(const char *typeName);
static int isLikelyUnaryOperatorSite(const char *start, const char *op);
static int isWrappedInOuterParens(const char *start, const char *end);
static const char *findTopLevelArithmeticOp(const char *start,
                                            const char *end,
                                            const char *ops);
static char *inferAetherBindingTypeName(const char *exprStart,
                                        const char *exprEnd,
                                        const AetherBindingTable *bindings,
                                        const AetherFunctionTable *functions,
                                        const AetherFieldTable *fields);
static void maybeRecordAetherBindingType(AetherBindingTable *table,
                                         const char *body,
                                         const char *lineEnd,
                                         const AetherFunctionTable *functions,
                                         const AetherFieldTable *fields);
static char *extractBindingName(const char *body, const char *lineEnd);
static char *extractModuleName(const char *source);
static void maybeRecordAetherFunctionReturnType(AetherFunctionTable *table,
                                                const char *body,
                                                const char *lineEnd,
                                                const char *moduleName,
                                                const char *typeName);
static char *extractSelfReceiverTypeName(const char *paramsStart, const char *paramsEnd);
static char *extractUsePathLiteral(const char *body, const char *lineEnd);
static int copyNamedBindingType(AetherBindingTable *dst,
                                const AetherBindingTable *src,
                                const char *name);
static int collectImportedAetherBindings(AetherBindingTable *out,
                                         AetherFunctionTable *functions,
                                         const char *source,
                                         const char *modulePath);
static void maybeRecordToonLiteralBinding(ToonLiteralTable *table, const char *body, const char *lineEnd);
static int appendJsonAliasReplacement(Buffer *out,
                                      const char *nameStart,
                                      size_t nameLen,
                                      JsonAliasState *jsonState);
static int appendAetherBuiltinAlias(Buffer *out, const char *nameStart, size_t nameLen);
static int appendAetherCapabilityAlias(Buffer *out,
                                       const char *nameStart,
                                       size_t nameLen,
                                       const char *openParen,
                                       const char **outCursor);
static int appendAetherInlineCallAlias(Buffer *out,
                                       const char *nameStart,
                                       size_t nameLen,
                                       const char *openParen,
                                       const char *lineEnd,
                                       const char **outCursor);
static const char *toonScalarGetterForName(const char *nameStart, size_t nameLen);
static int isToonScalarDefaultHelper(const char *nameStart, size_t nameLen);
static const char *toonTypePredicateExpected(const char *nameStart, size_t nameLen);
static int appendAliasedTrimmedRange(Buffer *out,
                                     const char *start,
                                     const char *end,
                                     JsonAliasState *jsonState,
                                     const ToonLiteralTable *toonTable);
static int isSingleCharDoubleQuotedLiteral(const char *start, const char *end);
static int appendToonTextKeyArg(Buffer *out,
                                const char *start,
                                const char *end,
                                JsonAliasState *jsonState,
                                const ToonLiteralTable *toonTable);
static int appendToonScalarAlias(Buffer *out,
                                 const char *nameStart,
                                 size_t nameLen,
                                 const char *openParen,
                                 const char **outCursor,
                                 JsonAliasState *jsonState,
                                 const ToonLiteralTable *toonTable);
static int appendToonNullAlias(Buffer *out,
                               const char *nameStart,
                               size_t nameLen,
                               const char *openParen,
                               const char **outCursor,
                               JsonAliasState *jsonState);
static int appendToonQueryAlias(Buffer *out,
                                const char *nameStart,
                                size_t nameLen,
                                const char *openParen,
                                const char **outCursor,
                                JsonAliasState *jsonState,
                                const ToonLiteralTable *toonTable);
static int appendToonInspectAlias(Buffer *out,
                                  const char *nameStart,
                                  size_t nameLen,
                                  const char *openParen,
                                  const char **outCursor,
                                  JsonAliasState *jsonState,
                                  const ToonLiteralTable *toonTable);
static char *applyJsonAliasesToLine(const char *line,
                                    JsonAliasState *jsonState,
                                    const ToonLiteralTable *toonTable);
static int startsWithWord(const char *body, const char *lineEnd, const char *word);
static int braceDeltaForLine(const char *line);
static char *rewriteAetherBuiltinAliases(const char *start, const char *end);

/* ---- implementations ---- */

static int bufferEnsure(Buffer *buf, size_t extra) {
    if (!buf) {
        return 0;
    }
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return 1;
    }
    size_t newCap = buf->cap ? buf->cap : 256;
    while (newCap < need) {
        newCap *= 2;
    }
    char *resized = (char *)realloc(buf->data, newCap);
    if (!resized) {
        return 0;
    }
    buf->data = resized;
    buf->cap = newCap;
    return 1;
}

static int bufferAppendN(Buffer *buf, const char *text, size_t len) {
    if (!buf || (!text && len > 0)) {
        return 0;
    }
    if (!bufferEnsure(buf, len)) {
        return 0;
    }
    if (len > 0) {
        memcpy(buf->data + buf->len, text, len);
        buf->len += len;
    }
    buf->data[buf->len] = '\0';
    return 1;
}

static int bufferAppend(Buffer *buf, const char *text) {
    return bufferAppendN(buf, text, text ? strlen(text) : 0);
}

static char *dupRange(const char *start, const char *end) {
    if (!start || !end || end < start) {
        return NULL;
    }
    size_t len = (size_t)(end - start);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static char *dupCString(const char *text) {
    return text ? dupRange(text, text + strlen(text)) : NULL;
}

static char *readTextFile(const char *path) {
    FILE *fp;
    long size;
    char *buffer;

    if (!path) {
        return NULL;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
        free(buffer);
        fclose(fp);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(fp);
    return buffer;
}

static char *trimmedCopy(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    return dupRange(start, end);
}

static const char *skipSpaces(const char *p) {
    while (p && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *skipSpacesInRange(const char *p, const char *end) {
    while (p && p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static char *resolveRelativePath(const char *sourcePath, const char *importPath) {
    char combined[PATH_MAX];
    char resolved[PATH_MAX];
    const char *slash;
    size_t dirLen;

    if (!importPath || !*importPath) {
        return NULL;
    }
    if (importPath[0] == '/') {
        if (realpath(importPath, resolved)) {
            return dupCString(resolved);
        }
        return dupCString(importPath);
    }
    if (!sourcePath || !*sourcePath) {
        return dupCString(importPath);
    }
    slash = strrchr(sourcePath, '/');
    if (!slash) {
        return dupCString(importPath);
    }
    dirLen = (size_t)(slash - sourcePath);
    if (dirLen + 1 + strlen(importPath) + 1 > sizeof(combined)) {
        return NULL;
    }
    memcpy(combined, sourcePath, dirLen);
    combined[dirLen] = '/';
    strcpy(combined + dirLen + 1, importPath);
    if (realpath(combined, resolved)) {
        return dupCString(resolved);
    }
    return dupCString(combined);
}

static const char *findCharInRange(const char *start, const char *end, char target) {
    const char *cursor = start;
    while (cursor < end) {
        if (*cursor == target) {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static const char *findMatchingCloseParen(const char *open, const char *end) {
    const char *cursor;
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!open || !end || open >= end || *open != '(') {
        return NULL;
    }
    cursor = open;
    while (cursor < end) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0) {
                return cursor;
            }
        }
        cursor++;
    }
    return NULL;
}

static const char *findSubstringInRange(const char *start, const char *end, const char *needle) {
    size_t needleLen = needle ? strlen(needle) : 0;
    const char *cursor = start;
    if (needleLen == 0 || !needle) {
        return NULL;
    }
    while (cursor + needleLen <= end) {
        if (strncmp(cursor, needle, needleLen) == 0) {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static void reportAetherRewriteError(const char *path,
                                     int line,
                                     const char *kind,
                                     const char *detail,
                                     const char *hint) {
    const char *code = aetherInferDiagnosticCode(kind, detail);
    const char *label = (kind && strcmp(kind, "parser") != 0) ? kind : NULL;

    if (code) {
        if (label) {
            fprintf(stderr,
                    "%s:%d: [%s] Aether %s parser error: %s\n",
                    path ? path : "<aether>",
                    line > 0 ? line : 1,
                    code,
                    label,
                    detail ? detail : "unknown parser error.");
        } else {
            fprintf(stderr,
                    "%s:%d: [%s] Aether parser error: %s\n",
                    path ? path : "<aether>",
                    line > 0 ? line : 1,
                    code,
                    detail ? detail : "unknown parser error.");
        }
    } else {
        if (label) {
            fprintf(stderr,
                    "%s:%d: Aether %s parser error: %s\n",
                    path ? path : "<aether>",
                    line > 0 ? line : 1,
                    label,
                    detail ? detail : "unknown parser error.");
        } else {
            fprintf(stderr,
                    "%s:%d: Aether parser error: %s\n",
                    path ? path : "<aether>",
                    line > 0 ? line : 1,
                    detail ? detail : "unknown parser error.");
        }
    }
    if (hint && *hint) {
        fprintf(stderr, "hint: %s\n", hint);
    }
    aetherReportGuideHelp(code);
}

static int isIdentifierChar(unsigned char ch) {
    return isalnum(ch) || ch == '_';
}

static int bufferAppendEscapedStringLiteral(Buffer *buf, const char *text, size_t len) {
    size_t i;

    if (!buf || (!text && len > 0)) {
        return 0;
    }
    if (!bufferAppend(buf, "\"")) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];

        if (ch == '\\') {
            if (!bufferAppend(buf, "\\\\")) {
                return 0;
            }
        } else if (ch == '"') {
            if (!bufferAppend(buf, "\\\"")) {
                return 0;
            }
        } else if (ch == '\n') {
            if (!bufferAppend(buf, "\\n")) {
                return 0;
            }
        } else if (ch == '\r') {
            if (!bufferAppend(buf, "\\r")) {
                return 0;
            }
        } else if (ch == '\t') {
            if (!bufferAppend(buf, "\\t")) {
                return 0;
            }
        } else if (ch < 0x20) {
            char escaped[7];

            snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            if (!bufferAppend(buf, escaped)) {
                return 0;
            }
        } else {
            if (!bufferAppendN(buf, (const char *)&text[i], 1)) {
                return 0;
            }
        }
    }
    return bufferAppend(buf, "\"");
}

static const char *findToonMarker(const char *lineStart, const char *lineEnd) {
    const char *cursor = lineStart;
    int inString = 0;
    char quote = '\0';

    while (cursor + 5 <= lineEnd) {
        if (!inString && cursor[0] == '/' && cursor + 1 < lineEnd && cursor[1] == '/') {
            break;
        }
        if (inString) {
            if (*cursor == '\\' && cursor + 1 < lineEnd) {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (strncmp(cursor, "toon:", 5) == 0 &&
            (cursor == lineStart || !isIdentifierChar((unsigned char)cursor[-1])) &&
            (cursor + 5 == lineEnd || isspace((unsigned char)cursor[5]) ||
             cursor[5] == '/' || cursor[5] == ';')) {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static const char *findLineCommentStartInRange(const char *start, const char *end) {
    const char *cursor = start;
    int inString = 0;
    char quote = '\0';

    while (cursor + 1 < end) {
        if (inString) {
            if (*cursor == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (cursor[0] == '/' && cursor[1] == '/') {
            return cursor;
        }
        cursor++;
    }
    return NULL;
}

static size_t leadingIndentWidth(const char *lineStart, const char *lineEnd) {
    const char *cursor = lineStart;

    while (cursor < lineEnd && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    return (size_t)(cursor - lineStart);
}

static char *preprocessToonBlocks(const char *source, const char *path) {
    const char *cursor = source;
    Buffer out = {0};
    int line = 1;

    if (!source) {
        return NULL;
    }

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *toonMarker;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }

        toonMarker = findToonMarker(lineStart, lineEnd);
        if (!toonMarker) {
            if (!bufferAppendN(&out, lineStart, (size_t)(lineEnd - lineStart))) {
                free(out.data);
                return NULL;
            }
            if (*lineEnd == '\n' && !bufferAppendN(&out, "\n", 1)) {
                free(out.data);
                return NULL;
            }
            cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
            continue;
        }

        {
            const char *afterMarker = toonMarker + 5;
            const char *scan = afterMarker;
            const char *blockStart;
            const char *blockCursor;
            const char *blockEnd;
            const char *lastContentEnd = NULL;
            size_t markerIndent = leadingIndentWidth(lineStart, lineEnd);
            size_t baseIndent = 0;
            int sawContent = 0;
            Buffer toonText = {0};

            while (scan < lineEnd) {
                if (*scan == '/' && scan + 1 < lineEnd && scan[1] == '/') {
                    break;
                }
                if (!isspace((unsigned char)*scan)) {
                    reportAetherRewriteError(path,
                                             line,
                                             "TOON",
                                             "only whitespace or comments may follow 'toon:'.",
                                             NULL);
                    free(out.data);
                    return NULL;
                }
                scan++;
            }

            blockStart = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
            blockCursor = blockStart;

            while (*blockCursor) {
                const char *blockLineStart = blockCursor;
                const char *blockLineEnd = blockCursor;
                size_t blockIndent;
                const char *trimmed;

                while (*blockLineEnd && *blockLineEnd != '\n') {
                    blockLineEnd++;
                }
                blockIndent = leadingIndentWidth(blockLineStart, blockLineEnd);
                trimmed = blockLineStart + blockIndent;

                if (trimmed >= blockLineEnd) {
                    blockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
                    continue;
                }
                if (blockIndent <= markerIndent) {
                    break;
                }
                if (!sawContent || blockIndent < baseIndent) {
                    baseIndent = blockIndent;
                }
                sawContent = 1;
                lastContentEnd = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
                blockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
            }

            if (!sawContent) {
                reportAetherRewriteError(path,
                                         line,
                                         "TOON",
                                         "'toon:' must be followed by an indented TOON block.",
                                         NULL);
                free(out.data);
                return NULL;
            }

            blockEnd = lastContentEnd ? lastContentEnd : blockCursor;
            blockCursor = blockStart;
            while (blockCursor < blockEnd) {
                const char *blockLineStart = blockCursor;
                const char *blockLineEnd = blockCursor;
                const char *nextBlockCursor;
                size_t blockIndent;
                const char *contentStart;

                while (*blockLineEnd && *blockLineEnd != '\n') {
                    blockLineEnd++;
                }
                nextBlockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
                blockIndent = leadingIndentWidth(blockLineStart, blockLineEnd);
                contentStart = blockLineStart;
                if (blockIndent >= baseIndent) {
                    contentStart = blockLineStart + baseIndent;
                }
                if (!bufferAppendN(&toonText, contentStart, (size_t)(blockLineEnd - contentStart))) {
                    free(toonText.data);
                    free(out.data);
                    return NULL;
                }
                if (nextBlockCursor < blockEnd && *blockLineEnd == '\n' &&
                    !bufferAppendN(&toonText, "\n", 1)) {
                    free(toonText.data);
                    free(out.data);
                    return NULL;
                }
                blockCursor = nextBlockCursor;
            }

            if (!bufferAppendN(&out, lineStart, (size_t)(toonMarker - lineStart)) ||
                !bufferAppendEscapedStringLiteral(&out, toonText.data, toonText.len) ||
                !bufferAppend(&out, ";")) {
                free(toonText.data);
                free(out.data);
                return NULL;
            }
            free(toonText.data);

            if (*lineEnd == '\n' && !bufferAppendN(&out, "\n", 1)) {
                free(out.data);
                return NULL;
            }

            blockCursor = blockStart;
            while (blockCursor < blockEnd) {
                const char *blockLineEnd = blockCursor;

                while (*blockLineEnd && *blockLineEnd != '\n') {
                    blockLineEnd++;
                }
                if (*blockLineEnd == '\n' && !bufferAppendN(&out, "\n", 1)) {
                    free(out.data);
                    return NULL;
                }
                blockCursor = *blockLineEnd == '\n' ? blockLineEnd + 1 : blockLineEnd;
            }

            while (cursor < blockEnd) {
                if (*cursor == '\n') {
                    line++;
                }
                cursor++;
            }
            continue;
        }

        if (*lineEnd == '\n') {
            line++;
        }
    }

    return out.data;
}

char *aetherAstPrepassToonBlocks(const char *source, const char *path) {
    return preprocessToonBlocks(source, path);
}

static void freeAetherBindingTable(AetherBindingTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].typeName);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static void freeAetherFunctionTable(AetherFunctionTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].returnType);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static void freeAetherFieldTable(AetherFieldTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].typeName);
        free(table->items[i].fieldName);
        free(table->items[i].fieldType);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureAetherBindingTable(AetherBindingTable *table, size_t extra) {
    AetherBinding *resized;
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
    resized = (AetherBinding *)realloc(table->items, newCap * sizeof(AetherBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int ensureAetherFunctionTable(AetherFunctionTable *table, size_t extra) {
    AetherFunctionSig *resized;
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
    resized = (AetherFunctionSig *)realloc(table->items, newCap * sizeof(AetherFunctionSig));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int setAetherBindingType(AetherBindingTable *table, const char *name, const char *typeName) {
    size_t i;
    char *nameCopy;
    char *typeCopy;

    if (!table || !name || !typeName) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            typeCopy = dupRange(typeName, typeName + strlen(typeName));
            if (!typeCopy) {
                return 0;
            }
            free(table->items[i].typeName);
            table->items[i].typeName = typeCopy;
            return 1;
        }
    }
    if (!ensureAetherBindingTable(table, 1)) {
        return 0;
    }
    nameCopy = dupRange(name, name + strlen(name));
    typeCopy = dupRange(typeName, typeName + strlen(typeName));
    if (!nameCopy || !typeCopy) {
        free(nameCopy);
        free(typeCopy);
        return 0;
    }
    table->items[table->count].name = nameCopy;
    table->items[table->count].typeName = typeCopy;
    table->count++;
    return 1;
}

static int setAetherFunctionReturnType(AetherFunctionTable *table,
                                       const char *name,
                                       const char *returnType) {
    size_t i;
    char *nameCopy;
    char *typeCopy;

    if (!table || !name || !returnType) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            typeCopy = dupRange(returnType, returnType + strlen(returnType));
            if (!typeCopy) {
                return 0;
            }
            free(table->items[i].returnType);
            table->items[i].returnType = typeCopy;
            return 1;
        }
    }
    if (!ensureAetherFunctionTable(table, 1)) {
        return 0;
    }
    nameCopy = dupRange(name, name + strlen(name));
    typeCopy = dupRange(returnType, returnType + strlen(returnType));
    if (!nameCopy || !typeCopy) {
        free(nameCopy);
        free(typeCopy);
        return 0;
    }
    table->items[table->count].name = nameCopy;
    table->items[table->count].returnType = typeCopy;
    table->count++;
    return 1;
}

static const char *findAetherBindingType(const AetherBindingTable *table, const char *name, size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return table->items[i].typeName;
        }
    }
    return NULL;
}

static const char *findAetherFunctionReturnType(const AetherFunctionTable *table,
                                                const char *name,
                                                size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return table->items[i].returnType;
        }
    }
    return NULL;
}

static const char *findAetherFieldType(const AetherFieldTable *table,
                                       const char *typeName,
                                       const char *fieldName,
                                       size_t fieldLen) {
    size_t i;

    if (!table || !typeName || !fieldName) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].typeName, typeName) == 0 &&
            strlen(table->items[i].fieldName) == fieldLen &&
            strncmp(table->items[i].fieldName, fieldName, fieldLen) == 0) {
            return table->items[i].fieldType;
        }
    }
    return NULL;
}

static void freeToonLiteralTable(ToonLiteralTable *table) {
    size_t i;

    if (!table) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        free(table->items[i].name);
        free(table->items[i].literal);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

static int ensureToonLiteralTable(ToonLiteralTable *table, size_t extra) {
    ToonLiteralBinding *resized;
    size_t need;
    size_t newCap;

    if (!table) {
        return 0;
    }
    need = table->count + extra;
    if (need <= table->cap) {
        return 1;
    }
    newCap = table->cap ? table->cap * 2 : 8;
    while (newCap < need) {
        newCap *= 2;
    }
    resized = (ToonLiteralBinding *)realloc(table->items, newCap * sizeof(ToonLiteralBinding));
    if (!resized) {
        return 0;
    }
    table->items = resized;
    table->cap = newCap;
    return 1;
}

static int setToonLiteralBinding(ToonLiteralTable *table, const char *name, const char *literal) {
    size_t i;
    char *nameCopy;
    char *literalCopy;

    if (!table || !name || !literal) {
        return 0;
    }
    for (i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].name, name) == 0) {
            literalCopy = dupRange(literal, literal + strlen(literal));
            if (!literalCopy) {
                return 0;
            }
            free(table->items[i].literal);
            table->items[i].literal = literalCopy;
            return 1;
        }
    }
    if (!ensureToonLiteralTable(table, 1)) {
        return 0;
    }
    nameCopy = dupRange(name, name + strlen(name));
    literalCopy = dupRange(literal, literal + strlen(literal));
    if (!nameCopy || !literalCopy) {
        free(nameCopy);
        free(literalCopy);
        return 0;
    }
    table->items[table->count].name = nameCopy;
    table->items[table->count].literal = literalCopy;
    table->count++;
    return 1;
}

static const char *findToonLiteralBinding(const ToonLiteralTable *table, const char *name, size_t len) {
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            return table->items[i].literal;
        }
    }
    return NULL;
}

static void clearToonLiteralBinding(ToonLiteralTable *table, const char *name, size_t len) {
    size_t i;

    if (!table || !name) {
        return;
    }
    for (i = 0; i < table->count; i++) {
        if (strlen(table->items[i].name) == len &&
            strncmp(table->items[i].name, name, len) == 0) {
            free(table->items[i].name);
            free(table->items[i].literal);
            if (i + 1 < table->count) {
                memmove(&table->items[i],
                        &table->items[i + 1],
                        (table->count - i - 1) * sizeof(ToonLiteralBinding));
            }
            table->count--;
            return;
        }
    }
}

static int isSupportedToonBindingType(const char *typeStart, const char *typeEnd) {
    size_t len;

    if (!typeStart || !typeEnd || typeEnd < typeStart) {
        return 0;
    }
    while (typeStart < typeEnd && isspace((unsigned char)*typeStart)) {
        typeStart++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    len = (size_t)(typeEnd - typeStart);
    return (len == 4 && strncmp(typeStart, "TOON", len) == 0) ||
           (len == 4 && strncmp(typeStart, "Text", len) == 0);
}

static int isAetherBoolLiteral(const char *start, const char *end) {
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

static int inferNumericLiteralType(const char *start, const char *end, const char **outTypeName) {
    const char *cursor;
    int sawDigit = 0;
    int sawDot = 0;
    int sawExp = 0;

    if (!start || !end || !outTypeName) {
        return 0;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start >= end) {
        return 0;
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
        return 0;
    }
    if (!sawDigit) {
        return 0;
    }
    *outTypeName = sawDot ? "Real" : "Int";
    return 1;
}

static const char *inferHelperReturnTypeName(const char *nameStart, size_t nameLen) {
    if (!nameStart || nameLen == 0) {
        return NULL;
    }
    if ((nameLen == 10 && strncmp(nameStart, "toon_parse", 10) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_parse_file", 15) == 0)) {
        return "ToonDoc";
    }
    if ((nameLen == 9 && strncmp(nameStart, "toon_root", 9) == 0) ||
        (nameLen == 8 && strncmp(nameStart, "toon_key", 8) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_key_or", 11) == 0) ||
        (nameLen == 9 && strncmp(nameStart, "toon_null", 9) == 0) ||
        (nameLen == 7 && strncmp(nameStart, "toon_at", 7) == 0)) {
        return "ToonNode";
    }
    if ((nameLen == 13 && strncmp(nameStart, "mstreamcreate", 13) == 0) ||
        (nameLen == 17 && strncmp(nameStart, "mstreamfromstring", 17) == 0) ||
        (nameLen == 13 && strncmp(nameStart, "socketreceive", 13) == 0)) {
        return "MStream";
    }
    if (nameLen == 13 && strncmp(nameStart, "mstreambuffer", 13) == 0) {
        return "Text";
    }
    if ((nameLen == 9 && strncmp(nameStart, "toon_type", 9) == 0) ||
        (nameLen == 13 && strncmp(nameStart, "toon_get_text", 13) == 0) ||
        (nameLen == 16 && strncmp(nameStart, "toon_get_text_or", 16) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_text_value", 15) == 0) ||
        (nameLen == 7 && strncmp(nameStart, "ai_chat", 7) == 0) ||
        (nameLen == 13 && strncmp(nameStart, "builtins_json", 13) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "builtin_info", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "int_to_text", 11) == 0)) {
        return "Text";
    }
    if ((nameLen == 8 && strncmp(nameStart, "toon_len", 8) == 0) ||
        (nameLen == 10 && strncmp(nameStart, "string_len", 10) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_get_int", 12) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_get_int_or", 15) == 0) ||
        (nameLen == 14 && strncmp(nameStart, "toon_int_value", 14) == 0)) {
        return "Int";
    }
    if ((nameLen == 13 && strncmp(nameStart, "toon_get_real", 13) == 0) ||
        (nameLen == 16 && strncmp(nameStart, "toon_get_real_or", 16) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_real_value", 15) == 0)) {
        return "Real";
    }
    if ((nameLen == 13 && strncmp(nameStart, "toon_get_bool", 13) == 0) ||
        (nameLen == 16 && strncmp(nameStart, "toon_get_bool_or", 16) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_bool_value", 15) == 0) ||
        (nameLen == 15 && strncmp(nameStart, "toon_null_value", 15) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_text", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_is_int", 11) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_real", 12) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_bool", 12) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_is_null", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_is_arr", 11) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_is_obj", 11) == 0) ||
        (nameLen == 12 && strncmp(nameStart, "toon_has_key", 12) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "toon_has_at", 11) == 0) ||
        (nameLen == 8 && strncmp(nameStart, "has_toon", 8) == 0) ||
        (nameLen == 6 && strncmp(nameStart, "has_ai", 6) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "has_builtin", 11) == 0)) {
        return "Bool";
    }
    if ((nameLen == 8 && strncmp(nameStart, "has_toon", 8) == 0) ||
        (nameLen == 6 && strncmp(nameStart, "has_ai", 6) == 0) ||
        (nameLen == 11 && strncmp(nameStart, "has_builtin", 11) == 0)) {
        return "Bool";
    }
    return NULL;
}

static const char *inferObjectInitTypeName(const char *start, const char *end) {
    const char *nameStart;
    const char *nameEnd;
    const char *cursor;

    if (!start || !end || end <= start) {
        return NULL;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start >= end || !(isalpha((unsigned char)*start) || *start == '_')) {
        return NULL;
    }
    nameStart = start;
    nameEnd = start + 1;
    while (nameEnd < end && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    cursor = skipSpacesInRange(nameEnd, end);
    if (cursor < end && *cursor == '{' && end[-1] == '}') {
        return trimmedCopy(nameStart, nameEnd);
    }
    return NULL;
}

static char *inferNewObjectTypeName(const char *start, const char *end) {
    const char *nameStart;
    const char *nameEnd;
    const char *cursor;

    if (!start || !end || end <= start) {
        return NULL;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if ((size_t)(end - start) < 4 || strncmp(start, "new ", 4) != 0) {
        return NULL;
    }
    nameStart = skipSpacesInRange(start + 3, end);
    if (nameStart >= end || !(isalpha((unsigned char)*nameStart) || *nameStart == '_')) {
        return NULL;
    }
    nameEnd = nameStart + 1;
    while (nameEnd < end && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
        nameEnd++;
    }
    cursor = skipSpacesInRange(nameEnd, end);
    if (cursor < end && *cursor == '(' && end[-1] == ')') {
        return trimmedCopy(nameStart, nameEnd);
    }
    return NULL;
}

static char *composeQualifiedLookup(const char *left,
                                    size_t leftLen,
                                    const char *right,
                                    size_t rightLen) {
    char *qualified;

    if (!left || !right || leftLen == 0 || rightLen == 0) {
        return NULL;
    }
    qualified = (char *)malloc(leftLen + 1 + rightLen + 1);
    if (!qualified) {
        return NULL;
    }
    memcpy(qualified, left, leftLen);
    qualified[leftLen] = '.';
    memcpy(qualified + leftLen + 1, right, rightLen);
    qualified[leftLen + 1 + rightLen] = '\0';
    return qualified;
}

static int isAetherNumericTypeName(const char *typeName) {
    if (!typeName) {
        return 0;
    }
    return strcmp(typeName, "Int") == 0 || strcmp(typeName, "Real") == 0;
}

static int isLikelyUnaryOperatorSite(const char *start, const char *op) {
    const char *cursor;

    if (!start || !op || op <= start) {
        return 1;
    }
    cursor = op;
    while (cursor > start) {
        cursor--;
        if (isspace((unsigned char)*cursor)) {
            continue;
        }
        return *cursor == '(' || *cursor == '[' || *cursor == '{' ||
               *cursor == ',' || *cursor == ':' || *cursor == '=' ||
               *cursor == '+' || *cursor == '-' || *cursor == '*' ||
               *cursor == '/' || *cursor == '%' || *cursor == '!' ||
               *cursor == '<' || *cursor == '>' || *cursor == '&' ||
               *cursor == '|';
    }
    return 1;
}

static int isWrappedInOuterParens(const char *start, const char *end) {
    const char *cursor;
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!start || !end || end - start < 2 || *start != '(' || end[-1] != ')') {
        return 0;
    }
    cursor = start;
    while (cursor < end) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
            if (depth == 0 && cursor + 1 < end) {
                return 0;
            }
        }
        cursor++;
    }
    return depth == 0;
}

static const char *findTopLevelArithmeticOp(const char *start,
                                            const char *end,
                                            const char *ops) {
    const char *cursor = start;
    const char *found = NULL;
    int parenDepth = 0;
    int braceDepth = 0;
    int bracketDepth = 0;
    int inString = 0;
    char quote = '\0';

    if (!start || !end || !ops) {
        return NULL;
    }
    while (cursor < end) {
        char ch = *cursor;

        if (inString) {
            if (ch == '\\' && cursor + 1 < end) {
                cursor += 2;
                continue;
            }
            if (ch == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            inString = 1;
            quote = ch;
            cursor++;
            continue;
        }
        if (ch == '(') {
            parenDepth++;
            cursor++;
            continue;
        }
        if (ch == ')') {
            parenDepth--;
            cursor++;
            continue;
        }
        if (ch == '{') {
            braceDepth++;
            cursor++;
            continue;
        }
        if (ch == '}') {
            braceDepth--;
            cursor++;
            continue;
        }
        if (ch == '[') {
            bracketDepth++;
            cursor++;
            continue;
        }
        if (ch == ']') {
            bracketDepth--;
            cursor++;
            continue;
        }
        if (parenDepth == 0 && braceDepth == 0 && bracketDepth == 0 &&
            strchr(ops, ch) != NULL &&
            !isLikelyUnaryOperatorSite(start, cursor)) {
            found = cursor;
        }
        cursor++;
    }
    return found;
}

static char *inferAetherBindingTypeName(const char *exprStart,
                                        const char *exprEnd,
                                        const AetherBindingTable *bindings,
                                        const AetherFunctionTable *functions,
                                        const AetherFieldTable *fields) {
    const char *trimmedStart = exprStart;
    const char *trimmedEnd = exprEnd;
    const char *nameEnd;
    const char *helperType;
    char *objectInitType;
    char *newObjectType;

    if (!exprStart || !exprEnd || exprEnd < exprStart) {
        return NULL;
    }
    {
        const char *commentStart = findLineCommentStartInRange(trimmedStart, trimmedEnd);
        if (commentStart) {
            trimmedEnd = commentStart;
        }
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
    while (isWrappedInOuterParens(trimmedStart, trimmedEnd)) {
        trimmedStart++;
        trimmedEnd--;
        while (trimmedStart < trimmedEnd && isspace((unsigned char)*trimmedStart)) {
            trimmedStart++;
        }
        while (trimmedEnd > trimmedStart && isspace((unsigned char)trimmedEnd[-1])) {
            trimmedEnd--;
        }
    }
    if (*trimmedStart == '"' && trimmedEnd[-1] == '"') {
        return dupCString("Text");
    }
    if (isAetherBoolLiteral(trimmedStart, trimmedEnd)) {
        return dupCString("Bool");
    }
    if (inferNumericLiteralType(trimmedStart, trimmedEnd, &helperType)) {
        return dupCString(helperType);
    }
    newObjectType = inferNewObjectTypeName(trimmedStart, trimmedEnd);
    if (newObjectType) {
        return newObjectType;
    }
    {
        const char *op = findTopLevelArithmeticOp(trimmedStart, trimmedEnd, "+-");
        if (!op) {
            op = findTopLevelArithmeticOp(trimmedStart, trimmedEnd, "*");
        }
        if (op) {
            char *leftType = inferAetherBindingTypeName(trimmedStart, op, bindings, functions, fields);
            char *rightType = inferAetherBindingTypeName(op + 1, trimmedEnd, bindings, functions, fields);
            char *resultType = NULL;

            if (leftType && rightType &&
                isAetherNumericTypeName(leftType) &&
                isAetherNumericTypeName(rightType)) {
                if (*op == '+' || *op == '-' || *op == '*') {
                    resultType = dupCString((strcmp(leftType, "Real") == 0 ||
                                             strcmp(rightType, "Real") == 0)
                                                ? "Real"
                                                : "Int");
                }
            }
            free(leftType);
            free(rightType);
            if (resultType) {
                return resultType;
            }
        }
    }
    nameEnd = trimmedStart;
    if (isalpha((unsigned char)*nameEnd) || *nameEnd == '_') {
        while (nameEnd < trimmedEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
            nameEnd++;
        }
        if (nameEnd == trimmedEnd && bindings) {
            const char *bindingType = findAetherBindingType(bindings,
                                                            trimmedStart,
                                                            (size_t)(trimmedEnd - trimmedStart));
            if (bindingType) {
                return dupCString(bindingType);
            }
        }
        if (nameEnd < trimmedEnd) {
            const char *callNameEnd = nameEnd;

            while (callNameEnd < trimmedEnd) {
                const char *dot = skipSpacesInRange(callNameEnd, trimmedEnd);

                if (dot >= trimmedEnd || *dot != '.') {
                    break;
                }
                callNameEnd = skipSpacesInRange(dot + 1, trimmedEnd);
                if (callNameEnd >= trimmedEnd ||
                    !(isalpha((unsigned char)*callNameEnd) || *callNameEnd == '_')) {
                    callNameEnd = nameEnd;
                    break;
                }
                while (callNameEnd < trimmedEnd &&
                       (isalnum((unsigned char)*callNameEnd) || *callNameEnd == '_')) {
                    callNameEnd++;
                }
            }
            if (functions && *skipSpacesInRange(callNameEnd, trimmedEnd) == '(') {
                const char *functionType = findAetherFunctionReturnType(functions,
                                                                        trimmedStart,
                                                                        (size_t)(callNameEnd - trimmedStart));
                if (functionType) {
                    return dupCString(functionType);
                }
                {
                    const char *dot = findCharInRange(trimmedStart, callNameEnd, '.');
                    if (dot && bindings) {
                        const char *receiverType = findAetherBindingType(bindings,
                                                                         trimmedStart,
                                                                         (size_t)(dot - trimmedStart));
                        if (receiverType) {
                            char *qualified = composeQualifiedLookup(receiverType,
                                                                     strlen(receiverType),
                                                                     dot + 1,
                                                                     (size_t)(callNameEnd - (dot + 1)));
                            if (qualified) {
                                functionType = findAetherFunctionReturnType(functions,
                                                                            qualified,
                                                                            strlen(qualified));
                                free(qualified);
                                if (functionType) {
                                    return dupCString(functionType);
                                }
                            }
                        }
                    }
                }
            }
            if (functions && bindings && fields) {
                const char *dot = skipSpacesInRange(nameEnd, trimmedEnd);
                if (dot < trimmedEnd && *dot == '.') {
                    const char *fieldStart = skipSpacesInRange(dot + 1, trimmedEnd);
                    const char *fieldEnd = fieldStart;
                    const char *fieldTail;

                    while (fieldEnd < trimmedEnd &&
                           (isalnum((unsigned char)*fieldEnd) || *fieldEnd == '_')) {
                        fieldEnd++;
                    }
                    fieldTail = skipSpacesInRange(fieldEnd, trimmedEnd);
                    if (fieldEnd > fieldStart && fieldTail == trimmedEnd) {
                        const char *receiverType = findAetherBindingType(bindings,
                                                                         trimmedStart,
                                                                         (size_t)(nameEnd - trimmedStart));
                        if (receiverType) {
                            if ((size_t)(fieldEnd - fieldStart) == 3 &&
                                strncmp(fieldStart, "len", 3) == 0) {
                                if (strcmp(receiverType, "Text") == 0 ||
                                    strstr(receiverType, "[]") != NULL) {
                                    return dupCString("Int");
                                }
                            }
                            helperType = findAetherFieldType(fields,
                                                             receiverType,
                                                             fieldStart,
                                                             (size_t)(fieldEnd - fieldStart));
                            if (helperType) {
                                return dupCString(helperType);
                            }
                        }
                    }
                }
            }
        }
        if (nameEnd < trimmedEnd && *skipSpacesInRange(nameEnd, trimmedEnd) == '(') {
            helperType = inferHelperReturnTypeName(trimmedStart, (size_t)(nameEnd - trimmedStart));
            if (helperType) {
                return dupCString(helperType);
            }
        }
    }
    objectInitType = (char *)inferObjectInitTypeName(trimmedStart, trimmedEnd);
    if (objectInitType) {
        return objectInitType;
    }
    return NULL;
}

static void maybeRecordAetherBindingType(AetherBindingTable *table,
                                         const char *body,
                                         const char *lineEnd,
                                         const AetherFunctionTable *functions,
                                         const AetherFieldTable *fields) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *equals;
    char *name = NULL;
    char *typeName = NULL;

    if (!table || !body || !lineEnd) {
        return;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    if (nameEnd == nameStart) {
        return;
    }
    name = trimmedCopy(nameStart, nameEnd);
    if (!name) {
        return;
    }

    colon = skipSpacesInRange(cursor, lineEnd);
    if (colon < lineEnd && *colon == ':') {
        const char *typeStart = skipSpacesInRange(colon + 1, lineEnd);
        const char *typeEnd = typeStart;

        while (typeEnd < lineEnd && *typeEnd != '=' && *typeEnd != ';') {
            typeEnd++;
        }
        while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
            typeEnd--;
        }
        typeName = trimmedCopy(typeStart, typeEnd);
    } else {
        equals = colon;
        while (equals < lineEnd && *equals != '=') {
            equals++;
        }
        if (equals < lineEnd && *equals == '=') {
            typeName = inferAetherBindingTypeName(skipSpacesInRange(equals + 1, lineEnd),
                                                  lineEnd,
                                                  table,
                                                  functions,
                                                  fields);
        }
    }

    if (typeName) {
        setAetherBindingType(table, name, typeName);
    }
    free(name);
    free(typeName);
}

static char *extractBindingName(const char *body, const char *lineEnd) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;

    if (!body || !lineEnd) {
        return NULL;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return NULL;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    if (nameEnd == nameStart) {
        return NULL;
    }
    return trimmedCopy(nameStart, nameEnd);
}

static char *extractModuleName(const char *source) {
    const char *cursor = source;

    if (!source) {
        return NULL;
    }
    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;
        const char *nameStart;
        const char *nameEnd;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipSpacesInRange(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "mod")) {
            nameStart = skipSpacesInRange(body + 3, lineEnd);
            nameEnd = nameStart;
            while (nameEnd < lineEnd &&
                   (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            if (nameEnd > nameStart) {
                return trimmedCopy(nameStart, nameEnd);
            }
        }
        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }
    return NULL;
}

static void maybeRecordAetherFunctionReturnType(AetherFunctionTable *table,
                                                const char *body,
                                                const char *lineEnd,
                                                const char *moduleName,
                                                const char *typeName) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *paramsOpen;
    const char *paramsClose;
    const char *arrow;
    const char *typeStart;
    const char *typeEnd;
    char qualifiedName[512];
    char *fnName = NULL;
    char *returnType = NULL;
    char *receiverType = NULL;

    if (!table || !body || !lineEnd) {
        return;
    }
    if (startsWithWord(body, lineEnd, "export")) {
        body = skipSpacesInRange(body + 6, lineEnd);
    }
    if (!startsWithWord(body, lineEnd, "fn")) {
        return;
    }
    cursor = skipSpacesInRange(body + 2, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    paramsOpen = findCharInRange(nameEnd, lineEnd, '(');
    paramsClose = paramsOpen ? findMatchingCloseParen(paramsOpen, lineEnd) : NULL;
    arrow = paramsClose ? findSubstringInRange(paramsClose, lineEnd, "->") : NULL;
    if (!paramsOpen || !paramsClose || !arrow || nameEnd == nameStart) {
        return;
    }
    typeStart = skipSpacesInRange(arrow + 2, lineEnd);
    typeEnd = typeStart;
    while (typeEnd < lineEnd && *typeEnd != '{') {
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    fnName = trimmedCopy(nameStart, nameEnd);
    returnType = trimmedCopy(typeStart, typeEnd);
    if (!fnName || !returnType) {
        free(fnName);
        free(returnType);
        return;
    }
    setAetherFunctionReturnType(table, fnName, returnType);
    if (moduleName && *moduleName &&
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", moduleName, fnName) < (int)sizeof(qualifiedName)) {
        setAetherFunctionReturnType(table, qualifiedName, returnType);
    }
    if (typeName && *typeName &&
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", typeName, fnName) < (int)sizeof(qualifiedName)) {
        setAetherFunctionReturnType(table, qualifiedName, returnType);
    }
    receiverType = extractSelfReceiverTypeName(paramsOpen + 1, paramsClose);
    if (receiverType &&
        snprintf(qualifiedName, sizeof(qualifiedName), "%s.%s", receiverType, fnName) < (int)sizeof(qualifiedName)) {
        setAetherFunctionReturnType(table, qualifiedName, returnType);
    }
    free(receiverType);
    free(fnName);
    free(returnType);
}

static char *extractSelfReceiverTypeName(const char *paramsStart, const char *paramsEnd) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *colon;
    const char *typeStart;
    const char *typeEnd;
    int depth = 0;

    if (!paramsStart || !paramsEnd || paramsEnd <= paramsStart) {
        return NULL;
    }
    cursor = skipSpacesInRange(paramsStart, paramsEnd);
    nameStart = cursor;
    while (cursor < paramsEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    cursor = skipSpacesInRange(cursor, paramsEnd);
    if (nameEnd == nameStart || cursor >= paramsEnd || *cursor != ':') {
        return NULL;
    }
    if (!(((size_t)(nameEnd - nameStart) == 4 && strncmp(nameStart, "self", 4) == 0) ||
          ((size_t)(nameEnd - nameStart) == 6 && strncmp(nameStart, "myself", 6) == 0) ||
          ((size_t)(nameEnd - nameStart) == 2 && strncmp(nameStart, "my", 2) == 0))) {
        return NULL;
    }
    colon = cursor;
    typeStart = skipSpacesInRange(colon + 1, paramsEnd);
    typeEnd = typeStart;
    while (typeEnd < paramsEnd) {
        char ch = *typeEnd;
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0) {
            depth--;
        } else if (ch == ',' && depth == 0) {
            break;
        }
        typeEnd++;
    }
    while (typeEnd > typeStart && isspace((unsigned char)typeEnd[-1])) {
        typeEnd--;
    }
    return trimmedCopy(typeStart, typeEnd);
}

static char *extractUsePathLiteral(const char *body, const char *lineEnd) {
    const char *cursor;
    const char *pathStart;
    const char *pathEnd;

    if (!body || !lineEnd || !startsWithWord(body, lineEnd, "use")) {
        return NULL;
    }
    cursor = skipSpacesInRange(body + 3, lineEnd);
    if (cursor >= lineEnd || *cursor != '"') {
        return NULL;
    }
    pathStart = cursor + 1;
    pathEnd = pathStart;
    while (pathEnd < lineEnd) {
        if (*pathEnd == '\\' && pathEnd + 1 < lineEnd) {
            pathEnd += 2;
            continue;
        }
        if (*pathEnd == '"') {
            break;
        }
        pathEnd++;
    }
    if (pathEnd >= lineEnd || *pathEnd != '"') {
        return NULL;
    }
    return dupRange(pathStart, pathEnd);
}

static int copyNamedBindingType(AetherBindingTable *dst,
                                const AetherBindingTable *src,
                                const char *name) {
    const char *typeName;

    if (!dst || !src || !name) {
        return 0;
    }
    typeName = findAetherBindingType(src, name, strlen(name));
    if (!typeName) {
        return 1;
    }
    return setAetherBindingType(dst, name, typeName);
}

static int collectImportedAetherBindings(AetherBindingTable *out,
                                         AetherFunctionTable *functions,
                                         const char *source,
                                         const char *modulePath) {
    AetherBindingTable local = {0};
    AetherFieldTable localFields = {0};
    char *moduleName = NULL;
    char *currentTypeName = NULL;
    const char *cursor = source;
    int braceDepth = 0;

    (void)modulePath;
    if (!out || !source) {
        return 0;
    }
    moduleName = extractModuleName(source);

    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        const char *body;

        while (*lineEnd && *lineEnd != '\n') {
            lineEnd++;
        }
        body = skipSpacesInRange(lineStart, lineEnd);

        if (startsWithWord(body, lineEnd, "type")) {
            const char *nameStart = skipSpacesInRange(body + 4, lineEnd);
            const char *nameEnd = nameStart;
            while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            free(currentTypeName);
            currentTypeName = (nameEnd > nameStart) ? trimmedCopy(nameStart, nameEnd) : NULL;
        }

        if (startsWithWord(body, lineEnd, "export")) {
            const char *rest = skipSpacesInRange(body + 6, lineEnd);

            maybeRecordAetherFunctionReturnType(functions, rest, lineEnd, moduleName, currentTypeName);
            maybeRecordAetherBindingType(&local, rest, lineEnd, functions, &localFields);
            if (startsWithWord(rest, lineEnd, "let") || startsWithWord(rest, lineEnd, "const")) {
                char *name = extractBindingName(rest, lineEnd);
                if (name) {
                    if (!copyNamedBindingType(out, &local, name)) {
                        free(name);
                        free(moduleName);
                        freeAetherBindingTable(&local);
                        return 0;
                    }
                    free(name);
                }
            }
        } else {
            maybeRecordAetherFunctionReturnType(functions, body, lineEnd, moduleName, currentTypeName);
            maybeRecordAetherBindingType(&local, body, lineEnd, functions, &localFields);
        }

        braceDepth += braceDeltaForLine(body);
        if (currentTypeName && braceDepth <= 0) {
            free(currentTypeName);
            currentTypeName = NULL;
        }

        cursor = *lineEnd == '\n' ? lineEnd + 1 : lineEnd;
    }

    free(currentTypeName);
    free(moduleName);
    freeAetherBindingTable(&local);
    freeAetherFieldTable(&localFields);
    return 1;
}

static void maybeRecordToonLiteralBinding(ToonLiteralTable *table, const char *body, const char *lineEnd) {
    const char *cursor;
    const char *nameStart;
    const char *nameEnd;
    const char *equals;
    const char *valueStart;
    const char *valueEnd;
    const char *aliasLiteral = NULL;
    char *name = NULL;
    char *literal = NULL;

    if (!table || !body || !lineEnd) {
        return;
    }
    if (!(startsWithWord(body, lineEnd, "let") || startsWithWord(body, lineEnd, "const"))) {
        return;
    }
    cursor = body + (startsWithWord(body, lineEnd, "const") ? 5 : 3);
    cursor = skipSpacesInRange(cursor, lineEnd);
    nameStart = cursor;
    while (cursor < lineEnd && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    nameEnd = cursor;
    cursor = skipSpacesInRange(cursor, lineEnd);
    if (cursor >= lineEnd || *cursor != ':') {
        return;
    }
    equals = cursor + 1;
    while (equals < lineEnd && *equals != '=') {
        equals++;
    }
    if (equals >= lineEnd || *equals != '=') {
        return;
    }
    if (!isSupportedToonBindingType(cursor + 1, equals)) {
        return;
    }
    valueStart = skipSpacesInRange(equals + 1, lineEnd);
    valueEnd = lineEnd;
    while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
        valueEnd--;
    }
    if (valueEnd > valueStart && valueEnd[-1] == ';') {
        valueEnd--;
    }
    while (valueEnd > valueStart && isspace((unsigned char)valueEnd[-1])) {
        valueEnd--;
    }
    name = trimmedCopy(nameStart, nameEnd);
    if (!name) {
        return;
    }
    if (valueEnd > valueStart && *valueStart == '"' && valueEnd[-1] == '"') {
        literal = dupRange(valueStart, valueEnd);
        if (!literal || !setToonLiteralBinding(table, name, literal)) {
            free(name);
            free(literal);
            return;
        }
        free(name);
        free(literal);
        return;
    }
    if (valueEnd > valueStart &&
        (isalpha((unsigned char)*valueStart) || *valueStart == '_')) {
        const char *aliasEnd = valueStart;

        while (aliasEnd < valueEnd && (isalnum((unsigned char)*aliasEnd) || *aliasEnd == '_')) {
            aliasEnd++;
        }
        if (aliasEnd == valueEnd) {
            aliasLiteral = findToonLiteralBinding(table,
                                                 valueStart,
                                                 (size_t)(valueEnd - valueStart));
        }
    }
    if (aliasLiteral && setToonLiteralBinding(table, name, aliasLiteral)) {
        free(name);
        return;
    }
    clearToonLiteralBinding(table, name, strlen(name));
    free(name);
}

static int appendJsonAliasReplacement(Buffer *out,
                                      const char *nameStart,
                                      size_t nameLen,
                                      JsonAliasState *jsonState) {
    if (!out || !nameStart || nameLen == 0) {
        return 0;
    }

    if (nameLen == 15 && strncmp(nameStart, "toon_parse_file", nameLen) == 0) {
        jsonState->needed = 1;
        /* Recorded: YyjsonReadFile is the one effectful Yyjson lowering, and
         * FX-001/purity diagnostics must quote 'toon_parse_file'. */
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "YyjsonReadFile");
    }
    if (nameLen == 10 && strncmp(nameStart, "toon_parse", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonRead");
    }
    if (nameLen == 9 && strncmp(nameStart, "toon_root", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetRoot");
    }
    if (nameLen == 10 && strncmp(nameStart, "toon_close", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonDocFree");
    }
    if (nameLen == 8 && strncmp(nameStart, "toon_key", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetKey");
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_has_key", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonHasKey");
    }
    if (nameLen == 7 && strncmp(nameStart, "toon_at", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetIndex");
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_has_at", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonHasIndex");
    }
    if (nameLen == 8 && strncmp(nameStart, "toon_len", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetLength");
    }
    if (nameLen == 9 && strncmp(nameStart, "toon_free", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonFreeValue");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_text_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetString");
    }
    if (nameLen == 14 && strncmp(nameStart, "toon_int_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetInt");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_real_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetNumber");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_bool_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonGetBool");
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_null_value", nameLen) == 0) {
        jsonState->needed = 1;
        return bufferAppend(out, "YyjsonIsNull");
    }
    return 0;
}

static int aetherAliasAppendRecorded(Buffer *out,
                                     const char *surfaceStart,
                                     size_t surfaceLen,
                                     const char *canonical) {
    if (g_aetherPrepassAliasLine > 0 && surfaceStart && surfaceLen > 0 && surfaceLen < 64) {
        char surface[64];
        memcpy(surface, surfaceStart, surfaceLen);
        surface[surfaceLen] = '\0';
        aetherAstRegisterAliasAtLine(g_aetherPrepassAliasLine, canonical, surface);
    }
    return bufferAppend(out, canonical);
}

static int appendAetherBuiltinAlias(Buffer *out, const char *nameStart, size_t nameLen) {
    if (!out || !nameStart || nameLen == 0) {
        return 0;
    }
    if (nameLen == 10 && strncmp(nameStart, "task_spawn", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_spawn_named");
    }
    if (nameLen == 10 && strncmp(nameStart, "task_queue", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_pool_submit");
    }
    if (nameLen == 13 && strncmp(nameStart, "task_set_name", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_set_name");
    }
    if (nameLen == 10 && strncmp(nameStart, "task_pause", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_pause");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_resume", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_resume");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_cancel", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_cancel");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_lookup", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_lookup");
    }
    if (nameLen == 9 && strncmp(nameStart, "task_wait", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "WaitForThread");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_status", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_get_status");
    }
    if (nameLen == 11 && strncmp(nameStart, "task_result", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_get_result");
    }
    if (nameLen == 10 && strncmp(nameStart, "task_stats", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "thread_stats");
    }
    if (nameLen == 15 && strncmp(nameStart, "task_stats_json", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "ThreadStatsJson");
    }
    if (nameLen == 7 && strncmp(nameStart, "ai_chat", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "openaichatcompletions");
    }
    if (nameLen == 13 && strncmp(nameStart, "builtins_json", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "aetherbuiltinsjson");
    }
    if (nameLen == 12 && strncmp(nameStart, "builtin_info", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "aetherbuiltininfo");
    }
    if (nameLen == 7 && strncmp(nameStart, "println", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "writeln");
    }
    if (nameLen == 11 && strncmp(nameStart, "int_to_text", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "IntToStr");
    }
    if (nameLen == 5 && strncmp(nameStart, "sleep", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "delay");
    }
    if (nameLen == 5 && strncmp(nameStart, "print", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "write");
    }
    if (nameLen == 10 && strncmp(nameStart, "string_len", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "length");
    }
    if (nameLen == 3 && strncmp(nameStart, "len", nameLen) == 0) {
        return aetherAliasAppendRecorded(out, nameStart, nameLen, "length");
    }
    return 0;
}

static int appendAetherCapabilityAlias(Buffer *out,
                                       const char *nameStart,
                                       size_t nameLen,
                                       const char *openParen,
                                       const char **outCursor) {
    const char *closeParen;

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }
    closeParen = skipSpaces(openParen + 1);
    if (nameLen == 8 && strncmp(nameStart, "has_toon", nameLen) == 0) {
        if (*closeParen != ')') {
            return 0;
        }
        if (!bufferAppend(out, "hasextbuiltin(\"yyjson\", \"YyjsonRead\")")) {
            return 0;
        }
        *outCursor = closeParen + 1;
        return 1;
    }
    if (nameLen == 6 && strncmp(nameStart, "has_ai", nameLen) == 0) {
        if (*closeParen != ')') {
            return 0;
        }
        if (!bufferAppend(out, "hasextbuiltin(\"openai\", \"OpenAIChatCompletions\")")) {
            return 0;
        }
        *outCursor = closeParen + 1;
        return 1;
    }
    if (nameLen == 11 && strncmp(nameStart, "has_builtin", nameLen) == 0) {
        if (!bufferAppend(out, "hasextbuiltin")) {
            return 0;
        }
        *outCursor = openParen;
        return 1;
    }
    return 0;
}

static int appendAetherInlineCallAlias(Buffer *out,
                                       const char *nameStart,
                                       size_t nameLen,
                                       const char *openParen,
                                       const char *lineEnd,
                                       const char **outCursor) {
    const char *closeParen;
    const char *arg1Start;
    const char *arg1End;
    const char *arg2Start;
    const char *arg2End;
    const char *comma = NULL;
    const char *scan;
    int depth = 0;

    if (!out || !nameStart || !openParen || *openParen != '(' || !lineEnd || !outCursor) {
        return 0;
    }
    if (!(nameLen == 9 && strncmp(nameStart, "string_eq", nameLen) == 0)) {
        return 0;
    }
    closeParen = findMatchingCloseParen(openParen, lineEnd);
    if (!closeParen) {
        return 0;
    }
    arg1Start = skipSpacesInRange(openParen + 1, closeParen);
    if (arg1Start >= closeParen) {
        return 0;
    }
    scan = arg1Start;
    while (scan < closeParen) {
        char ch = *scan;

        if (ch == '"' || ch == '\'') {
            char quote = ch;
            scan++;
            while (scan < closeParen) {
                if (*scan == '\\' && scan + 1 < closeParen) {
                    scan += 2;
                    continue;
                }
                if (*scan == quote) {
                    scan++;
                    break;
                }
                scan++;
            }
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{' || ch == '<') {
            depth++;
        } else if ((ch == ')' || ch == ']' || ch == '}' || ch == '>') && depth > 0) {
            depth--;
        } else if (ch == ',' && depth == 0) {
            comma = scan;
            break;
        }
        scan++;
    }
    if (!comma) {
        return 0;
    }
    arg1End = comma;
    while (arg1End > arg1Start && isspace((unsigned char)arg1End[-1])) {
        arg1End--;
    }
    arg2Start = skipSpacesInRange(comma + 1, closeParen);
    arg2End = closeParen;
    while (arg2End > arg2Start && isspace((unsigned char)arg2End[-1])) {
        arg2End--;
    }
    if (arg1End <= arg1Start || arg2End <= arg2Start) {
        return 0;
    }
    if (!bufferAppend(out, "(") ||
        !bufferAppendN(out, arg1Start, (size_t)(arg1End - arg1Start)) ||
        !bufferAppend(out, " == ") ||
        !bufferAppendN(out, arg2Start, (size_t)(arg2End - arg2Start)) ||
        !bufferAppend(out, ")")) {
        return 0;
    }
    *outCursor = closeParen + 1;
    return 1;
}

static const char *toonScalarGetterForName(const char *nameStart, size_t nameLen) {
    if (nameLen == 13 && strncmp(nameStart, "toon_get_text", nameLen) == 0) {
        return "YyjsonGetString";
    }
    if (nameLen == 16 && strncmp(nameStart, "toon_get_text_or", nameLen) == 0) {
        return "YyjsonGetString";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_get_int", nameLen) == 0) {
        return "YyjsonGetInt";
    }
    if (nameLen == 15 && strncmp(nameStart, "toon_get_int_or", nameLen) == 0) {
        return "YyjsonGetInt";
    }
    if (nameLen == 13 && strncmp(nameStart, "toon_get_real", nameLen) == 0) {
        return "YyjsonGetNumber";
    }
    if (nameLen == 16 && strncmp(nameStart, "toon_get_real_or", nameLen) == 0) {
        return "YyjsonGetNumber";
    }
    if (nameLen == 13 && strncmp(nameStart, "toon_get_bool", nameLen) == 0) {
        return "YyjsonGetBool";
    }
    if (nameLen == 16 && strncmp(nameStart, "toon_get_bool_or", nameLen) == 0) {
        return "YyjsonGetBool";
    }
    return NULL;
}

static int isToonScalarDefaultHelper(const char *nameStart, size_t nameLen) {
    return (nameLen == 16 && strncmp(nameStart, "toon_get_text_or", nameLen) == 0) ||
           (nameLen == 15 && strncmp(nameStart, "toon_get_int_or", nameLen) == 0) ||
           (nameLen == 16 && strncmp(nameStart, "toon_get_real_or", nameLen) == 0) ||
           (nameLen == 16 && strncmp(nameStart, "toon_get_bool_or", nameLen) == 0);
}

static const char *toonTypePredicateExpected(const char *nameStart, size_t nameLen) {
    if (nameLen == 12 && strncmp(nameStart, "toon_is_text", nameLen) == 0) {
        return "string";
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_is_int", nameLen) == 0) {
        return "int";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_is_real", nameLen) == 0) {
        return "real";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_is_bool", nameLen) == 0) {
        return "bool";
    }
    if (nameLen == 12 && strncmp(nameStart, "toon_is_null", nameLen) == 0) {
        return "null";
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_is_arr", nameLen) == 0) {
        return "array";
    }
    if (nameLen == 11 && strncmp(nameStart, "toon_is_obj", nameLen) == 0) {
        return "object";
    }
    return NULL;
}

static int appendAliasedTrimmedRange(Buffer *out,
                                     const char *start,
                                     const char *end,
                                     JsonAliasState *jsonState,
                                     const ToonLiteralTable *toonTable) {
    char *segment;
    char *aliased;
    int ok;

    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    segment = dupRange(start, end);
    if (!segment) {
        return 0;
    }
    aliased = applyJsonAliasesToLine(segment, jsonState, toonTable);
    free(segment);
    if (!aliased) {
        return 0;
    }
    ok = bufferAppend(out, aliased);
    free(aliased);
    return ok;
}

static int isSingleCharDoubleQuotedLiteral(const char *start, const char *end) {
    size_t innerLen;

    if (!start || !end) {
        return 0;
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if ((size_t)(end - start) < 3 || *start != '"' || end[-1] != '"') {
        return 0;
    }
    innerLen = (size_t)(end - start - 2);
    if (innerLen == 1) {
        return 1;
    }
    if (innerLen == 2 && start[1] == '\\') {
        return 1;
    }
    return 0;
}

static int appendToonTextKeyArg(Buffer *out,
                                const char *start,
                                const char *end,
                                JsonAliasState *jsonState,
                                const ToonLiteralTable *toonTable) {
    if (isSingleCharDoubleQuotedLiteral(start, end)) {
        return bufferAppend(out, "(\"\" + ") &&
               appendAliasedTrimmedRange(out, start, end, jsonState, toonTable) &&
               bufferAppend(out, ")");
    }
    return appendAliasedTrimmedRange(out, start, end, jsonState, toonTable);
}

static int appendToonScalarAlias(Buffer *out,
                                 const char *nameStart,
                                 size_t nameLen,
                                 const char *openParen,
                                 const char **outCursor,
                                 JsonAliasState *jsonState,
                                 const ToonLiteralTable *toonTable) {
    const char *getter = toonScalarGetterForName(nameStart, nameLen);
    const char *cursor;
    const char *arg1Start;
    const char *arg1End = NULL;
    const char *arg2Start = NULL;
    const char *arg2End = NULL;
    const char *arg3Start = NULL;
    const char *arg3End = NULL;
    int depth = 0;
    int inString = 0;
    char quote = '\0';
    int defaultHelper = isToonScalarDefaultHelper(nameStart, nameLen);

    if (!getter || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }
    cursor = openParen + 1;
    arg1Start = cursor;
    while (*cursor) {
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            depth++;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            if (depth == 0) {
                if (!arg1End || !arg2Start) {
                    return 0;
                }
                if (arg3Start) {
                    arg3End = cursor;
                } else {
                    arg2End = cursor;
                }
                break;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0 && !arg1End) {
            arg1End = cursor;
            arg2Start = cursor + 1;
        } else if (*cursor == ',' && depth == 0 && !arg2End) {
            arg2End = cursor;
            arg3Start = cursor + 1;
        }
        cursor++;
    }
    if (!arg1End || !arg2Start || !arg2End) {
        return 0;
    }
    if (defaultHelper && (!arg3Start || !arg3End)) {
        return 0;
    }
    if (!defaultHelper && arg3Start) {
        return 0;
    }
    jsonState->needed = 1;
    if (defaultHelper) {
        if (!bufferAppend(out, "(YyjsonHasKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, ") ? ") ||
            !bufferAppend(out, getter) ||
            !bufferAppend(out, "(YyjsonGetKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, ")) : ") ||
            !appendAliasedTrimmedRange(out, arg3Start, arg3End, jsonState, toonTable) ||
            !bufferAppend(out, ")")) {
            return 0;
        }
    } else {
        if (!bufferAppend(out, getter) ||
            !bufferAppend(out, "(YyjsonGetKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, "))")) {
            return 0;
        }
    }
    *outCursor = cursor + 1;
    return 1;
}

static int appendToonNullAlias(Buffer *out,
                               const char *nameStart,
                               size_t nameLen,
                               const char *openParen,
                               const char **outCursor,
                               JsonAliasState *jsonState) {
    const char *closeParen;

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor ||
        !jsonState) {
        return 0;
    }
    if (!(nameLen == 9 && strncmp(nameStart, "toon_null", nameLen) == 0)) {
        return 0;
    }
    closeParen = skipSpaces(openParen + 1);
    if (*closeParen != ')') {
        return 0;
    }
    /* toon_null() yields a real JSON-null node: parse the literal "null" and
     * take its root. toon_is_null() is true for it and toon_len() degrades to 0,
     * so it is a valid stand-in default for the node-returning toon_key_or. */
    jsonState->needed = 1;
    if (!bufferAppend(out, "YyjsonGetRoot(YyjsonRead(\"null\"))")) {
        return 0;
    }
    *outCursor = closeParen + 1;
    return 1;
}

static int appendToonQueryAlias(Buffer *out,
                                const char *nameStart,
                                size_t nameLen,
                                const char *openParen,
                                const char **outCursor,
                                JsonAliasState *jsonState,
                                const ToonLiteralTable *toonTable) {
    const char *replacement = NULL;
    const char *cursor;
    const char *arg1Start;
    const char *arg1End = NULL;
    const char *arg2Start = NULL;
    const char *arg2End = NULL;
    const char *arg3Start = NULL;
    const char *arg3End = NULL;
    int depth = 0;
    int inString = 0;
    char quote = '\0';
    int keyHelper = 0;
    int keyOrHelper = 0;

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }
    if (nameLen == 8 && strncmp(nameStart, "toon_key", nameLen) == 0) {
        replacement = "YyjsonGetKey";
        keyHelper = 1;
    } else if (nameLen == 11 && strncmp(nameStart, "toon_key_or", nameLen) == 0) {
        /* toon_key_or(node, key, default) returns a sub-node like toon_key, but
         * falls back to the third argument (itself a ToonNode) when the key is
         * absent. Mirrors the defaulted scalar helpers (toon_get_text_or, ...),
         * just without the scalar value getter wrapping the node. */
        replacement = "YyjsonGetKey";
        keyHelper = 1;
        keyOrHelper = 1;
    } else if (nameLen == 12 && strncmp(nameStart, "toon_has_key", nameLen) == 0) {
        replacement = "YyjsonHasKey";
        keyHelper = 1;
    } else if (nameLen == 7 && strncmp(nameStart, "toon_at", nameLen) == 0) {
        replacement = "YyjsonGetIndex";
    } else if (nameLen == 11 && strncmp(nameStart, "toon_has_at", nameLen) == 0) {
        replacement = "YyjsonHasIndex";
    } else {
        return 0;
    }

    cursor = openParen + 1;
    arg1Start = cursor;
    while (*cursor) {
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            depth++;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            if (depth == 0) {
                if (arg3Start) {
                    arg3End = cursor;
                } else {
                    arg2End = cursor;
                }
                break;
            }
            depth--;
        } else if (*cursor == ',' && depth == 0 && !arg1End) {
            arg1End = cursor;
            arg2Start = cursor + 1;
        } else if (*cursor == ',' && depth == 0 && keyOrHelper && !arg2End) {
            arg2End = cursor;
            arg3Start = cursor + 1;
        }
        cursor++;
    }
    if (!arg1End || !arg2Start || !arg2End) {
        return 0;
    }
    if (keyOrHelper && (!arg3Start || !arg3End)) {
        return 0;
    }

    jsonState->needed = 1;
    if (keyOrHelper) {
        /* (YyjsonHasKey(node, key) ? YyjsonGetKey(node, key) : default) */
        if (!bufferAppend(out, "(YyjsonHasKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, ") ? YyjsonGetKey(") ||
            !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
            !bufferAppend(out, ", ") ||
            !appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable) ||
            !bufferAppend(out, ") : ") ||
            !appendAliasedTrimmedRange(out, arg3Start, arg3End, jsonState, toonTable) ||
            !bufferAppend(out, ")")) {
            return 0;
        }
        *outCursor = cursor + 1;
        return 1;
    }

    if (!bufferAppend(out, replacement) ||
        !bufferAppend(out, "(") ||
        !appendAliasedTrimmedRange(out, arg1Start, arg1End, jsonState, toonTable) ||
        !bufferAppend(out, ", ")) {
        return 0;
    }
    if (keyHelper) {
        if (!appendToonTextKeyArg(out, arg2Start, arg2End, jsonState, toonTable)) {
            return 0;
        }
    } else {
        if (!appendAliasedTrimmedRange(out, arg2Start, arg2End, jsonState, toonTable)) {
            return 0;
        }
    }
    if (!bufferAppend(out, ")")) {
        return 0;
    }
    *outCursor = cursor + 1;
    return 1;
}

static int appendToonInspectAlias(Buffer *out,
                                  const char *nameStart,
                                  size_t nameLen,
                                  const char *openParen,
                                  const char **outCursor,
                                  JsonAliasState *jsonState,
                                  const ToonLiteralTable *toonTable) {
    const char *expectedType = toonTypePredicateExpected(nameStart, nameLen);
    const char *cursor;
    const char *argStart;
    const char *argEnd;
    int depth = 0;
    int inString = 0;
    char quote = '\0';

    if (!out || !nameStart || !openParen || *openParen != '(' || !outCursor) {
        return 0;
    }

    if (nameLen == 9 && strncmp(nameStart, "toon_type", nameLen) == 0) {
        cursor = openParen + 1;
        argStart = cursor;
        while (*cursor) {
            if (inString) {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    cursor += 2;
                    continue;
                }
                if (*cursor == quote) {
                    inString = 0;
                    quote = '\0';
                }
                cursor++;
                continue;
            }
            if (*cursor == '"' || *cursor == '\'') {
                inString = 1;
                quote = *cursor;
                cursor++;
                continue;
            }
            if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
                depth++;
            } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
                if (depth == 0) {
                    argEnd = cursor;
                    jsonState->needed = 1;
                    if (!bufferAppend(out, "YyjsonGetType(") ||
                        !appendAliasedTrimmedRange(out, argStart, argEnd, jsonState, toonTable) ||
                        !bufferAppend(out, ")")) {
                        return 0;
                    }
                    *outCursor = cursor + 1;
                    return 1;
                }
                depth--;
            }
            cursor++;
        }
        return 0;
    }

    if (!expectedType) {
        return 0;
    }

    cursor = openParen + 1;
    argStart = cursor;
    while (*cursor) {
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '(' || *cursor == '[' || *cursor == '{') {
            depth++;
        } else if (*cursor == ')' || *cursor == ']' || *cursor == '}') {
            if (depth == 0) {
                argEnd = cursor;
                jsonState->needed = 1;
                if (!bufferAppend(out, "(YyjsonGetType(") ||
                    !appendAliasedTrimmedRange(out, argStart, argEnd, jsonState, toonTable) ||
                    !bufferAppend(out, ") == \"") ||
                    !bufferAppend(out, expectedType) ||
                    !bufferAppend(out, "\")")) {
                    return 0;
                }
                *outCursor = cursor + 1;
                return 1;
            }
            depth--;
        }
        cursor++;
    }
    return 0;
}

static char *applyJsonAliasesToLine(const char *line,
                                    JsonAliasState *jsonState,
                                    const ToonLiteralTable *toonTable) {
    const char *cursor = line;
    Buffer out = {0};

    if (!line || !jsonState) {
        return line ? dupRange(line, line + strlen(line)) : NULL;
    }
    if (*line == '\0') {
        return dupRange(line, line);
    }

    while (*cursor) {
        if (*cursor == '"') {
            /* Copy string literals verbatim: alias lowering must never rewrite
             * user-visible text (e.g. println("call sleep(5) now") must not
             * become "call delay(5) now"). Honors \" escapes. */
            const char *scan = cursor + 1;
            while (*scan) {
                if (*scan == '\\' && scan[1] != '\0') {
                    scan += 2;
                    continue;
                }
                if (*scan == '"') {
                    scan++;
                    break;
                }
                scan++;
            }
            if (!bufferAppendN(&out, cursor, (size_t)(scan - cursor))) {
                free(out.data);
                return NULL;
            }
            cursor = scan;
            continue;
        }
        if (cursor[0] == '/' && cursor[1] == '/') {
            /* Rest of the line is a comment: copy verbatim, no alias rewriting. */
            if (!bufferAppend(&out, cursor)) {
                free(out.data);
                return NULL;
            }
            break;
        }
        if ((cursor == line || !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            ((strncmp(cursor, "toon_parse", 10) == 0 &&
              !(isalnum((unsigned char)cursor[10]) || cursor[10] == '_')) ||
             (strncmp(cursor, "YyjsonRead", 10) == 0 &&
              !(isalnum((unsigned char)cursor[10]) || cursor[10] == '_')))) {
            const char *nameEnd = cursor + 10;
            const char *openParen = skipSpaces(nameEnd);

            if (*openParen == '(') {
                const char *argStart = skipSpaces(openParen + 1);
                const char *argEnd = argStart;
                const char *literal = NULL;
                const char *closeParen;

                while (*argEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
                    argEnd++;
                }
                literal = findToonLiteralBinding(toonTable, argStart, (size_t)(argEnd - argStart));
                closeParen = skipSpaces(argEnd);
                if (literal && *closeParen == ')') {
                    jsonState->needed = 1;
                    if (!bufferAppend(&out, "YyjsonRead(") ||
                        !bufferAppend(&out, literal) ||
                        !bufferAppend(&out, ")")) {
                        free(out.data);
                        return NULL;
                    }
                    cursor = closeParen + 1;
                    continue;
                }
            }
        }
        if ((cursor == line || !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            isalpha((unsigned char)*cursor)) {
            const char *nameStart = cursor;
            const char *nameEnd = cursor + 1;
            const char *afterName;
            const char *advancedCursor = NULL;

            while (*nameEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                nameEnd++;
            }
            afterName = skipSpaces(nameEnd);
            if (*afterName == '(' &&
                appendAetherCapabilityAlias(&out,
                                            nameStart,
                                            (size_t)(nameEnd - nameStart),
                                            afterName,
                                            &advancedCursor)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonNullAlias(&out,
                                    nameStart,
                                    (size_t)(nameEnd - nameStart),
                                    afterName,
                                    &advancedCursor,
                                    jsonState)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonScalarAlias(&out,
                                      nameStart,
                                      (size_t)(nameEnd - nameStart),
                                      afterName,
                                      &advancedCursor,
                                      jsonState,
                                      toonTable)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonQueryAlias(&out,
                                     nameStart,
                                     (size_t)(nameEnd - nameStart),
                                     afterName,
                                     &advancedCursor,
                                     jsonState,
                                     toonTable)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendToonInspectAlias(&out,
                                       nameStart,
                                       (size_t)(nameEnd - nameStart),
                                       afterName,
                                       &advancedCursor,
                                       jsonState,
                                       toonTable)) {
                cursor = advancedCursor;
                continue;
            }
            if (*afterName == '(' &&
                appendAetherBuiltinAlias(&out, nameStart, (size_t)(nameEnd - nameStart))) {
                cursor = nameEnd;
                continue;
            }
            if (*afterName == '(' &&
                appendJsonAliasReplacement(&out, nameStart, (size_t)(nameEnd - nameStart), jsonState)) {
                cursor = nameEnd;
                continue;
            }
        }
        if ((cursor == line || !(isalnum((unsigned char)cursor[-1]) || cursor[-1] == '_')) &&
            strncmp(cursor, "YyjsonRead(", 11) == 0) {
            const char *argStart = skipSpaces(cursor + 11);
            const char *argEnd = argStart;
            const char *literal = NULL;

            while (*argEnd && (isalnum((unsigned char)*argEnd) || *argEnd == '_')) {
                argEnd++;
            }
            literal = findToonLiteralBinding(toonTable, argStart, (size_t)(argEnd - argStart));
            if (literal) {
                const char *afterArg = skipSpaces(argEnd);
                if (*afterArg == ')') {
                    if (!bufferAppend(&out, "YyjsonRead(") ||
                        !bufferAppend(&out, literal) ||
                        !bufferAppend(&out, ")")) {
                        free(out.data);
                        return NULL;
                    }
                    cursor = afterArg + 1;
                    continue;
                }
            }
        }
        if (!bufferAppendN(&out, cursor, 1)) {
            free(out.data);
            return NULL;
        }
        cursor++;
    }

    return out.data;
}

static int startsWithWord(const char *body, const char *lineEnd, const char *word) {
    size_t wordLen = word ? strlen(word) : 0;
    if (!body || !lineEnd || !word || wordLen == 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) < wordLen) {
        return 0;
    }
    if (strncmp(body, word, wordLen) != 0) {
        return 0;
    }
    if ((size_t)(lineEnd - body) == wordLen) {
        return 1;
    }
    return isspace((unsigned char)body[wordLen]) || body[wordLen] == '{' || body[wordLen] == ';';
}

static int braceDeltaForLine(const char *line) {
    int delta = 0;
    int inString = 0;
    char quote = '\0';
    const char *cursor = line;

    if (!line) {
        return 0;
    }
    while (*cursor) {
        if (!inString && cursor[0] == '/' && cursor[1] == '/') {
            break;
        }
        if (inString) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                inString = 0;
                quote = '\0';
            }
            cursor++;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            inString = 1;
            quote = *cursor;
            cursor++;
            continue;
        }
        if (*cursor == '{') {
            delta++;
        } else if (*cursor == '}') {
            delta--;
        }
        cursor++;
    }
    return delta;
}

static char *rewriteAetherBuiltinAliases(const char *start, const char *end) {
    static const struct { const char *from; size_t fromLen; const char *to; } kAliases[] = {
        {"parse_json",    10, "toon_parse"},
        {"root_node",      9, "toon_root"},
        {"close_doc",      9, "toon_close"},
        {"lookup_string", 13, "toon_get_text"},
        {"lookup_int",    10, "toon_get_int"},
    };
    const char *cursor = start;
    Buffer out = {0};

    if (!start || !end || end < start) {
        return NULL;
    }
    if (start == end) {
        return dupCString("");
    }

    while (cursor < end) {
        if (cursor + 1 < end && cursor[0] == '/' && cursor[1] == '/') {
            const char *nl = cursor;
            while (nl < end && *nl != '\n') {
                nl++;
            }
            if (!bufferAppendN(&out, cursor, (size_t)(nl - cursor))) {
                free(out.data);
                return NULL;
            }
            cursor = nl;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor;
            if (!bufferAppendN(&out, cursor, 1)) {
                free(out.data);
                return NULL;
            }
            cursor++;
            while (cursor < end) {
                if (!bufferAppendN(&out, cursor, 1)) {
                    free(out.data);
                    return NULL;
                }
                if (*cursor == '\\' && cursor + 1 < end) {
                    cursor++;
                    if (!bufferAppendN(&out, cursor, 1)) {
                        free(out.data);
                        return NULL;
                    }
                } else if (*cursor == quote) {
                    cursor++;
                    break;
                }
                cursor++;
            }
            continue;
        }
        if ((cursor == start || !isIdentifierChar((unsigned char)cursor[-1])) &&
            (isalpha((unsigned char)*cursor) || *cursor == '_')) {
            const char *idEnd = cursor + 1;
            size_t idLen;
            const char *afterId;
            int matched = 0;
            while (idEnd < end && isIdentifierChar((unsigned char)*idEnd)) {
                idEnd++;
            }
            idLen = (size_t)(idEnd - cursor);
            afterId = skipSpacesInRange(idEnd, end);
            if (afterId < end && *afterId == '(') {
                for (size_t i = 0; i < sizeof(kAliases) / sizeof(kAliases[0]); ++i) {
                    if (idLen == kAliases[i].fromLen &&
                        strncmp(cursor, kAliases[i].from, idLen) == 0) {
                        if (!bufferAppend(&out, kAliases[i].to)) {
                            free(out.data);
                            return NULL;
                        }
                        matched = 1;
                        break;
                    }
                }
            }
            if (!matched && !bufferAppendN(&out, cursor, idLen)) {
                free(out.data);
                return NULL;
            }
            cursor = idEnd;
            continue;
        }
        if (!bufferAppendN(&out, cursor, 1)) {
            free(out.data);
            return NULL;
        }
        cursor++;
    }
    return out.data ? out.data : dupCString("");
}

char *aetherAstPrepassBuiltins(const char *source) {
    if (!source) return NULL;
    char *aliasNormalized = rewriteAetherBuiltinAliases(source, source + strlen(source));
    if (!aliasNormalized) return NULL;

    JsonAliasState jsonState = {0};
    ToonLiteralTable toonTable = {0};
    Buffer out = {0};
    const char *cursor = aliasNormalized;
    int ok = 1;
    int line = 1; /* the pre-passes preserve line structure, so this matches lexer lines */
    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        const char *body = lineStart;
        while (body < lineEnd && (*body == ' ' || *body == '\t')) body++;
        /* Track TOON literal bindings exactly as the rewriter does before the
         * per-line alias pass so doc-handle args resolve to the literal. */
        maybeRecordToonLiteralBinding(&toonTable, body, lineEnd);
        char *lineCopy = dupRange(lineStart, lineEnd);
        if (!lineCopy) { ok = 0; break; }
        g_aetherPrepassAliasLine = line;
        char *aliased = applyJsonAliasesToLine(lineCopy, &jsonState, &toonTable);
        g_aetherPrepassAliasLine = 0;
        free(lineCopy);
        if (!aliased) { ok = 0; break; }
        if (!bufferAppend(&out, aliased)) { free(aliased); ok = 0; break; }
        free(aliased);
        if (*lineEnd == '\n') {
            if (!bufferAppendN(&out, "\n", 1)) { ok = 0; break; }
            cursor = lineEnd + 1;
            line++;
        } else {
            cursor = lineEnd;
        }
    }
    free(aliasNormalized);
    freeToonLiteralTable(&toonTable);
    if (!ok) { free(out.data); return NULL; }
    if (!out.data) { out.data = (char *)malloc(1); if (out.data) out.data[0] = '\0'; }
    return out.data;
}

char *aetherAstPrepassInlineEq(const char *source) {
    if (!source) return NULL;
    Buffer out = {0};
    const char *end = source + strlen(source);
    const char *cur = source;
    /* Process line by line so identifier-boundary checks (body[-1]) and the
     * lineEnd the alias helper needs are well defined. */
    while (cur < end) {
        const char *lineStart = cur;
        const char *lineEnd = cur;
        while (lineEnd < end && *lineEnd != '\n') lineEnd++;
        const char *body = lineStart;
        while (body < lineEnd) {
            char ch = *body;
            /* Skip over string/char literals verbatim. */
            if (ch == '"' || ch == '\'') {
                char quote = ch;
                if (!bufferAppendN(&out, body, 1)) { free(out.data); return NULL; }
                body++;
                while (body < lineEnd) {
                    if (*body == '\\' && body + 1 < lineEnd) {
                        if (!bufferAppendN(&out, body, 2)) { free(out.data); return NULL; }
                        body += 2;
                        continue;
                    }
                    char c = *body;
                    if (!bufferAppendN(&out, body, 1)) { free(out.data); return NULL; }
                    body++;
                    if (c == quote) break;
                }
                continue;
            }
            /* A `string_eq(` call at an identifier boundary -> inline `==`. */
            if ((body == lineStart || !(isalnum((unsigned char)body[-1]) || body[-1] == '_')) &&
                isalpha((unsigned char)ch)) {
                const char *nameStart = body;
                const char *nameEnd = body + 1;
                while (nameEnd < lineEnd && (isalnum((unsigned char)*nameEnd) || *nameEnd == '_')) {
                    nameEnd++;
                }
                const char *afterName = skipSpacesInRange(nameEnd, lineEnd);
                const char *advancedCursor = NULL;
                if (afterName < lineEnd && *afterName == '(' &&
                    appendAetherInlineCallAlias(&out, nameStart,
                                                (size_t)(nameEnd - nameStart),
                                                afterName, lineEnd, &advancedCursor)) {
                    body = advancedCursor;
                    continue;
                }
            }
            if (!bufferAppendN(&out, body, 1)) { free(out.data); return NULL; }
            body++;
        }
        if (lineEnd < end) {
            if (!bufferAppendN(&out, "\n", 1)) { free(out.data); return NULL; }
            cur = lineEnd + 1;
        } else {
            cur = lineEnd;
        }
    }
    if (!out.data) { out.data = (char *)malloc(1); if (out.data) out.data[0] = '\0'; }
    return out.data;
}

void aetherAstCollectImportedTypes(const char *mainSource, const char *path,
                                      AetherImportTypeSink sink, void *ctx) {
    if (!mainSource || !sink) {
        return;
    }
    const char *cursor = mainSource;
    while (*cursor) {
        const char *lineStart = cursor;
        const char *lineEnd = cursor;
        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        const char *body = skipSpacesInRange(lineStart, lineEnd);
        if (startsWithWord(body, lineEnd, "use")) {
            char *importPath = extractUsePathLiteral(body, lineEnd);
            if (importPath) {
                char *resolved = resolveRelativePath(path, importPath);
                if (resolved) {
                    char *moduleSource = readTextFile(resolved);
                    if (moduleSource) {
                        AetherBindingTable bt = {0};
                        AetherFunctionTable ft = {0};
                        if (collectImportedAetherBindings(&bt, &ft, moduleSource, resolved)) {
                            for (size_t i = 0; i < bt.count; i++) {
                                if (bt.items[i].name && bt.items[i].typeName) {
                                    sink(ctx, bt.items[i].name, bt.items[i].typeName, 0);
                                }
                            }
                            for (size_t i = 0; i < ft.count; i++) {
                                if (ft.items[i].name && ft.items[i].returnType) {
                                    sink(ctx, ft.items[i].name, ft.items[i].returnType, 1);
                                }
                            }
                        }
                        freeAetherBindingTable(&bt);
                        freeAetherFunctionTable(&ft);
                        free(moduleSource);
                    }
                    free(resolved);
                }
                free(importPath);
            }
        }
        cursor = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;
    }
}
