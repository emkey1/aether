/*
 * ast_parser.c -- Aether recursive-descent parser (MILESTONE 1).
 *
 * Experimental alternative to the line-based text rewriter in translate.c.
 * Enabled only when the environment variable AETHER_PARSER is set to "ast";
 * otherwise parser.c keeps using aetherRewriteSource()+parseRea() unchanged.
 *
 * The parser tokenizes Aether source with Rea's lexer and emits the *shared
 * pscal AST* directly -- the same node shapes Rea's own parser builds -- so
 * semantic analysis, codegen and the VM are untouched. Every node carries the
 * true source line (taken straight from the lexer token), which is the whole
 * point of this rewrite: diagnostics report the real line, not a rewrite-offset
 * line.
 *
 * Node shapes are mirrored from rea's src/rea/parser.c (the canonical AST
 * producer). Aether's surface syntax differs from Rea's, but the AST it lowers
 * to is identical, so we reproduce rea's node construction verbatim and only
 * change how the tokens are recognized:
 *
 *   - Aether keywords (fn, ret, fx, loop, let, const) are NOT Rea keywords, so
 *     the lexer hands them back as REA_TOKEN_IDENTIFIER; we match them by text.
 *   - Aether type names (Int/Real/Text/Bool/Void) arrive as identifiers too;
 *     we map them to the Rea keyword-name + VarType the rewriter would have
 *     produced (Int->"int"/INT64, Real->"float"/DOUBLE, Text->"str"/
 *     UNICODE_STRING, Bool->"bool"/BOOLEAN, Void->VOID) so the AST -- and thus
 *     program output -- matches the rewriter path byte-for-byte.
 *
 * MILESTONE 1 grammar (a slice of roadmap P2, enough to prove the pipeline):
 *   program   := { fnDecl }
 *   fnDecl    := 'fn' NAME '(' [params] ')' [ '->' Type ] block
 *   params    := param { ',' param }
 *   param     := NAME ':' Type
 *   block     := '{' { statement } '}'
 *   statement := letDecl | constDecl | ret | fxBlock | ifStmt | loopStmt
 *              | assignment | exprStmt
 *   letDecl   := ('let'|'const') NAME ':' Type [ '=' expr ] ';'
 *   ret       := 'ret' [ expr ] ';'
 *   fxBlock   := 'fx' block          (effect wrapper erased -> inner block)
 *   expr      := assignment ladder (||, &&, ==, <, +, *, unary, primary)
 */

#include "aether/parser.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ast/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/globals.h"
#include "symbol/symbol.h"
#include "rea/lexer.h"
#include "aether/semantic.h"

/* Provided by core/utils.c */
Token *newToken(TokenType type, const char *value, int line, int column);

/* ------------------------------------------------------------------ */
/* Parser state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    ReaLexer lexer;
    ReaToken current;
    VarType currentFunctionType;
    int functionDepth;
    bool hadError;
} AetherParser;

static void aetherAdvance(AetherParser *p) { p->current = reaNextToken(&p->lexer); }

/* True if the current token's text equals `kw` exactly. Aether keywords come
 * through the Rea lexer as identifiers, so we compare the lexeme span. */
static bool tokTextIs(const ReaToken *t, const char *kw) {
    size_t klen = strlen(kw);
    return t->length == klen && strncmp(t->start, kw, klen) == 0;
}

static bool isAetherKeyword(const ReaToken *t, const char *kw) {
    return t->type == REA_TOKEN_IDENTIFIER && tokTextIs(t, kw);
}

/* Map an Aether stdlib builtin name to its canonical Rea/pscal builtin, exactly
 * as translate.c appendAetherBuiltinAlias() does textually. Returns the canonical
 * name, or `name` unchanged if there is no alias. Keeping this in sync with the
 * rewriter is what lets call output match byte-for-byte (e.g. println -> writeln
 * -> AST_WRITELN). */
static const char *aliasBuiltinName(const char *name) {
    static const struct { const char *from; const char *to; } aliases[] = {
        { "task_spawn",      "thread_spawn_named" },
        { "task_queue",      "thread_pool_submit" },
        { "task_set_name",   "thread_set_name" },
        { "task_pause",      "thread_pause" },
        { "task_resume",     "thread_resume" },
        { "task_cancel",     "thread_cancel" },
        { "task_lookup",     "thread_lookup" },
        { "task_wait",       "WaitForThread" },
        { "task_status",     "thread_get_status" },
        { "task_result",     "thread_get_result" },
        { "task_stats",      "thread_stats" },
        { "task_stats_json", "ThreadStatsJson" },
        { "ai_chat",         "openaichatcompletions" },
        { "builtins_json",   "aetherbuiltinsjson" },
        { "builtin_info",    "aetherbuiltininfo" },
        { "println",         "writeln" },
        { "int_to_text",     "IntToStr" },
        { "sleep",           "delay" },
        { "print",           "write" },
        { "string_len",      "length" },
        { "len",             "length" },
    };
    if (!name) return name;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(name, aliases[i].from) == 0) {
            return aliases[i].to;
        }
    }
    return name;
}

/* Copy the current token's lexeme into a freshly allocated pscal identifier
 * Token (mirrors rea copyCurrentTokenAsIdentifier). */
static Token *currentAsIdentifier(AetherParser *p) {
    size_t len = (size_t)p->current.length;
    char *lex = (char *)malloc(len + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, len);
    lex[len] = '\0';
    Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    return tok;
}

/* ------------------------------------------------------------------ */
/* Type-name mapping                                                   */
/* ------------------------------------------------------------------ */

/* Map an Aether type name (the lexeme span) to the Rea keyword-name the
 * rewriter would emit, plus the resulting VarType. Returns false if the name
 * is not a known builtin Aether type. This reproduces translate.c mapTypeName()
 * followed by rea mapType(), which is the byte-for-byte contract: the type node
 * token value and var_type must equal what the rewriter+parseRea produce. */
static bool mapAetherType(const char *name, size_t len,
                          const char **outReaName, VarType *outType) {
    struct { const char *aether; const char *rea; VarType vt; } table[] = {
        { "Int",   "int",   TYPE_INT64 },
        { "Real",  "float", TYPE_DOUBLE },
        { "Float", "float", TYPE_DOUBLE },
        { "Text",  "str",   TYPE_UNICODE_STRING },
        { "Bool",  "bool",  TYPE_BOOLEAN },
        { "Void",  "void",  TYPE_VOID },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t alen = strlen(table[i].aether);
        if (len == alen && strncmp(name, table[i].aether, alen) == 0) {
            *outReaName = table[i].rea;
            *outType = table[i].vt;
            return true;
        }
    }
    return false;
}

/* Build the type node for a value-bearing type (non-Void), mirroring how rea's
 * parseVarDecl builds AST_TYPE_IDENTIFIER for a builtin keyword. For unknown
 * (user-defined) type names we fall back to an AST_TYPE_REFERENCE with
 * TYPE_UNKNOWN, matching rea's handling of an unresolved type identifier. */
static AST *buildTypeNode(const char *name, size_t len, int line, VarType *outType) {
    const char *reaName = NULL;
    VarType vt = TYPE_VOID;
    if (mapAetherType(name, len, &reaName, &vt)) {
        Token *tok = newToken(TOKEN_IDENTIFIER, reaName, line, 0);
        AST *node = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(node, vt);
        *outType = vt;
        return node;
    }
    /* Unknown / user-defined type name: reference node, unresolved type. */
    char *lex = (char *)malloc(len + 1);
    if (!lex) return NULL;
    memcpy(lex, name, len);
    lex[len] = '\0';
    Token *tok = newToken(TOKEN_IDENTIFIER, lex, line, 0);
    free(lex);
    AST *node = newASTNode(AST_TYPE_REFERENCE, tok);
    setTypeAST(node, TYPE_UNKNOWN);
    *outType = TYPE_UNKNOWN;
    return node;
}

/* ------------------------------------------------------------------ */
/* String unescaping (verbatim from rea parser.c reaUnescapeString)    */
/* ------------------------------------------------------------------ */

static char *aetherUnescapeString(const char *src, size_t len, size_t *out_len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            char n = src[++i];
            switch (n) {
                case 'n': buf[j++] = '\n'; break;
                case 'r': buf[j++] = '\r'; break;
                case 't': buf[j++] = '\t'; break;
                case '\\': buf[j++] = '\\'; break;
                case 0x27: buf[j++] = 0x27; break;
                case '"': buf[j++] = '"'; break;
                case 'x':
                case 'X': {
                    int val = 0;
                    size_t digits = 0;
                    while (i + 1 < len && digits < 2) {
                        char h = src[i + 1];
                        if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')) {
                            i++;
                            digits++;
                            val = val * 16 + (h >= '0' && h <= '9' ? h - '0' : (h & 0x5f) - 'A' + 10);
                        } else {
                            break;
                        }
                    }
                    if (digits > 0) {
                        buf[j++] = (char)val;
                    } else {
                        buf[j++] = '\\';
                        buf[j++] = n;
                    }
                    break;
                }
                default:
                    if (n >= '0' && n <= '7') {
                        int val = n - '0';
                        size_t digits = 1;
                        while (i + 1 < len && digits < 3 && src[i + 1] >= '0' && src[i + 1] <= '7') {
                            val = (val << 3) + (src[++i] - '0');
                            digits++;
                        }
                        buf[j++] = (char)val;
                    } else {
                        buf[j++] = '\\';
                        buf[j++] = n;
                    }
                    break;
            }
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    if (out_len) *out_len = j;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Type promotion helpers (verbatim from rea parser.c)                 */
/* ------------------------------------------------------------------ */

static VarType promoteRealBinaryType(VarType a, VarType b) {
    if (a == TYPE_LONG_DOUBLE || b == TYPE_LONG_DOUBLE) return TYPE_LONG_DOUBLE;
    if (a == TYPE_DOUBLE || b == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (a == TYPE_FLOAT || b == TYPE_FLOAT) return TYPE_FLOAT;
    return TYPE_DOUBLE;
}

static VarType promoteIntegralBinaryType(VarType a, VarType b) {
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

static VarType inferStringLiteralType(const char *text, size_t len) {
    if (!text) return TYPE_STRING;
    if (len == 1) return TYPE_CHAR;
    if (len == 0) return TYPE_STRING;
    uint32_t codepoint = 0;
    size_t advance = 0;
    if (decodeUtf8Codepoint(text, len, &codepoint, &advance) && advance == len) {
        return TYPE_WIDECHAR;
    }
    if (isValidUtf8Bytes(text, len) && utf8CodepointCount(text, len) < len) {
        return TYPE_UNICODE_STRING;
    }
    return TYPE_STRING;
}

/* ------------------------------------------------------------------ */
/* Procedure-table registration (mirrors rea parseFunctionDecl tail)   */
/* ------------------------------------------------------------------ */

static HashTable *aetherEnsureProcedureTable(void) {
    if (!procedure_table) {
        procedure_table = createHashTable();
    }
    if (!current_procedure_table) {
        current_procedure_table = procedure_table;
    }
    return current_procedure_table ? current_procedure_table : procedure_table;
}

/* Register a parsed function/procedure declaration so that calls resolve and
 * semantic analysis can find it -- exactly as rea's parseFunctionDecl does
 * (minus the class/module machinery, which Milestone 1 does not cover). */
static void registerFunctionSymbol(AST *func, const char *name, VarType vtype, bool hasBody) {
    char lower_name[MAX_SYMBOL_LENGTH];
    strncpy(lower_name, name, sizeof(lower_name) - 1);
    lower_name[sizeof(lower_name) - 1] = '\0';
    for (int i = 0; lower_name[i]; i++) {
        lower_name[i] = (char)tolower((unsigned char)lower_name[i]);
    }

    HashTable *target_table = current_procedure_table ? current_procedure_table : procedure_table;
    if (!target_table) {
        target_table = aetherEnsureProcedureTable();
    }

    Symbol *sym = target_table ? hashTableLookup(target_table, lower_name) : NULL;
    if (sym && sym->is_alias && sym->real_symbol) {
        sym = sym->real_symbol;
    }
    if (!sym) {
        sym = (Symbol *)calloc(1, sizeof(Symbol));
        if (sym) {
            sym->name = strdup(lower_name);
            if (target_table) {
                hashTableInsert(target_table, sym);
            }
        }
    }
    if (sym) {
        sym->type = vtype;
        if (sym->type_def) {
            freeAST(sym->type_def);
        }
        sym->type_def = copyAST(func);
        if (!hasBody) {
            sym->is_defined = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static AST *parseExpr(AetherParser *p);
static AST *parseStatement(AetherParser *p);
static AST *parseBlock(AetherParser *p);

/* ------------------------------------------------------------------ */
/* Primary / call expressions (mirrors rea parseFactor primary cases)  */
/* ------------------------------------------------------------------ */

static AST *parseStringLiteral(AetherParser *p) {
    int startLine = p->current.line;
    size_t totalLen = 0;
    size_t capacity = 0;
    char *buffer = NULL;
    bool haveSegment = false;
    bool charCandidate = false;

    while (p->current.type == REA_TOKEN_STRING) {
        size_t tokenLen = p->current.length;
        if (tokenLen < 2) { free(buffer); return NULL; }
        size_t innerLen = tokenLen - 2;
        size_t unescapedLen = 0;
        char *segment = aetherUnescapeString(p->current.start + 1, innerLen, &unescapedLen);
        if (!segment) { free(buffer); return NULL; }

        size_t required = totalLen + unescapedLen + 1;
        if (required > capacity) {
            size_t newCap = capacity ? capacity : 16;
            while (required > newCap) newCap *= 2;
            char *resized = (char *)realloc(buffer, newCap);
            if (!resized) { free(buffer); free(segment); return NULL; }
            buffer = resized;
            capacity = newCap;
        }
        if (unescapedLen > 0) memcpy(buffer + totalLen, segment, unescapedLen);
        totalLen += unescapedLen;
        free(segment);

        if (!haveSegment) {
            charCandidate = (p->current.start[0] == '\'' && unescapedLen == 1);
            haveSegment = true;
        } else {
            charCandidate = false;
        }
        aetherAdvance(p);
    }

    if (!haveSegment) { free(buffer); return NULL; }
    if (capacity == 0) {
        buffer = (char *)malloc(1);
        if (!buffer) return NULL;
        capacity = 1;
    }
    buffer[totalLen] = '\0';
    if (totalLen != 1) charCandidate = false;

    Token *tok = (Token *)malloc(sizeof(Token));
    if (!tok) { free(buffer); return NULL; }
    tok->type = TOKEN_STRING_CONST;
    tok->value = buffer;
    tok->length = totalLen;
    tok->line = startLine;
    tok->column = 0;
    tok->is_char_code = false;
    tok->char_code_value = 0;

    AST *node = newASTNode(AST_STRING, tok);
    node->i_val = (int)totalLen;
    setTypeAST(node, charCandidate ? TYPE_CHAR : inferStringLiteralType(buffer, totalLen));
    return node;
}

/* Parse an argument list assuming '(' is the current token. Returns an
 * AST_COMPOUND whose children are the argument expressions (caller moves them
 * onto the call node, matching rea). */
static AST *parseArgList(AetherParser *p) {
    aetherAdvance(p); /* consume '(' */
    AST *args = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        AST *arg = parseExpr(p);
        if (!arg) break;
        addChild(args, arg);
        if (p->current.type == REA_TOKEN_COMMA) {
            aetherAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    }
    return args;
}

static void moveArgsOntoCall(AST *call, AST *args) {
    if (args && args->child_count > 0) {
        call->children = args->children;
        call->child_count = args->child_count;
        call->child_capacity = args->child_capacity;
        for (int i = 0; i < call->child_count; i++) {
            if (call->children[i]) call->children[i]->parent = call;
        }
        args->children = NULL;
        args->child_count = 0;
        args->child_capacity = 0;
    }
    if (args) freeAST(args);
}

static AST *parsePrimary(AetherParser *p) {
    /* Unary minus */
    if (p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_MINUS, "-", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, right->var_type);
        return node;
    }
    /* Unary not */
    if (p->current.type == REA_TOKEN_BANG) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_NOT, "!", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, TYPE_BOOLEAN);
        return node;
    }
    /* Parenthesized expression */
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        aetherAdvance(p);
        AST *expr = parseExpr(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            aetherAdvance(p);
        }
        return expr;
    }
    /* Numeric literal */
    if (p->current.type == REA_TOKEN_NUMBER) {
        size_t len = p->current.length;
        const char *start = p->current.start;
        TokenType ttype = TOKEN_INTEGER_CONST;
        VarType vtype = TYPE_INT64;
        if (len > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
            start += 2;
            len -= 2;
            ttype = TOKEN_HEX_CONST;
        } else {
            for (size_t i = 0; i < len; i++) {
                if (start[i] == '.' || start[i] == 'e' || start[i] == 'E') {
                    ttype = TOKEN_REAL_CONST;
                    vtype = TYPE_DOUBLE;
                    break;
                }
            }
        }
        char *lex = (char *)malloc(len + 1);
        if (!lex) return NULL;
        memcpy(lex, start, len);
        lex[len] = '\0';
        Token *tok = newToken(ttype, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_NUMBER, tok);
        setTypeAST(node, vtype);
        aetherAdvance(p);
        return node;
    }
    /* String literal (with adjacent-literal concatenation, like rea) */
    if (p->current.type == REA_TOKEN_STRING) {
        return parseStringLiteral(p);
    }
    /* Boolean literals */
    if (p->current.type == REA_TOKEN_TRUE || p->current.type == REA_TOKEN_FALSE) {
        TokenType tt = (p->current.type == REA_TOKEN_TRUE) ? TOKEN_TRUE : TOKEN_FALSE;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(tt, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_BOOLEAN, tok);
        setTypeAST(node, TYPE_BOOLEAN);
        node->i_val = (tt == TOKEN_TRUE) ? 1 : 0;
        aetherAdvance(p);
        return node;
    }
    /* Identifier: bare variable or call f(args). */
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        Token *tok = currentAsIdentifier(p);
        if (!tok) return NULL;
        int idLine = p->current.line;
        aetherAdvance(p); /* consume identifier */

        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            /* Only names that are immediately called get the stdlib alias
             * treatment, matching the rewriter (it rewrites `name(` spans). */
            const char *raw = tok->value ? tok->value : "";
            const char *canonical = aliasBuiltinName(raw);
            if (canonical != raw && strcmp(canonical, raw) != 0) {
                freeToken(tok);
                tok = newToken(TOKEN_IDENTIFIER, canonical, idLine, 0);
                if (!tok) return NULL;
            }
            const char *tokValue = tok->value ? tok->value : "";
            AST *args = parseArgList(p);
            AST *call;
            if (strcasecmp(tokValue, "writeln") == 0) {
                call = newASTNode(AST_WRITELN, NULL);
                freeToken(tok);
            } else if (strcasecmp(tokValue, "write") == 0) {
                call = newASTNode(AST_WRITE, NULL);
                freeToken(tok);
            } else {
                call = newASTNode(AST_PROCEDURE_CALL, tok);
            }
            moveArgsOntoCall(call, args);
            setTypeAST(call, TYPE_UNKNOWN);
            return call;
        }
        AST *node = newASTNode(AST_VARIABLE, tok);
        setTypeAST(node, TYPE_UNKNOWN);
        return node;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Binary operator ladder (mirrors rea precedence climbing)            */
/* ------------------------------------------------------------------ */

static AST *parseMul(AetherParser *p) {
    AST *node = parsePrimary(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_STAR ||
           p->current.type == REA_TOKEN_SLASH ||
           p->current.type == REA_TOKEN_INT_DIV ||
           p->current.type == REA_TOKEN_PERCENT) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parsePrimary(p);
        if (!right) return NULL;
        VarType lt = node->var_type, rt = right->var_type;
        TokenType tt;
        const char *lex;
        switch (op.type) {
            case REA_TOKEN_STAR:    tt = TOKEN_MUL;     lex = "*";   break;
            case REA_TOKEN_SLASH:   tt = TOKEN_SLASH;   lex = "/";   break;
            case REA_TOKEN_INT_DIV: tt = TOKEN_INT_DIV; lex = "div"; break;
            default:                tt = TOKEN_MOD;     lex = "mod"; break;
        }
        Token *tok = newToken(tt, lex, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        bool leftReal = isRealType(lt), rightReal = isRealType(rt);
        bool integerOnlyOp = (tt == TOKEN_INT_DIV || tt == TOKEN_MOD);
        bool forceReal = (tt == TOKEN_SLASH) || (!integerOnlyOp && (leftReal || rightReal));
        VarType res = forceReal ? promoteRealBinaryType(lt, rt) : promoteIntegralBinaryType(lt, rt);
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseAdd(AetherParser *p) {
    AST *node = parseMul(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseMul(p);
        if (!right) return NULL;
        TokenType tt = (op.type == REA_TOKEN_PLUS) ? TOKEN_PLUS : TOKEN_MINUS;
        Token *tok = newToken(tt, (tt == TOKEN_PLUS) ? "+" : "-", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type, rt = right->var_type;
        VarType res;
        bool leftReal = isRealType(lt), rightReal = isRealType(rt);
        if (tt == TOKEN_PLUS && (isPascalStringType(lt) || isPascalStringType(rt) ||
                                 isPascalCharType(lt) || isPascalCharType(rt))) {
            res = inferBinaryOpType(lt, rt);
        } else if (leftReal || rightReal) {
            res = promoteRealBinaryType(lt, rt);
        } else {
            res = promoteIntegralBinaryType(lt, rt);
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseComparison(AetherParser *p) {
    AST *node = parseAdd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL ||
           p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseAdd(p);
        if (!right) return NULL;
        TokenType tt;
        const char *lex;
        switch (op.type) {
            case REA_TOKEN_GREATER:       tt = TOKEN_GREATER;       lex = ">";  break;
            case REA_TOKEN_GREATER_EQUAL: tt = TOKEN_GREATER_EQUAL; lex = ">="; break;
            case REA_TOKEN_LESS:          tt = TOKEN_LESS;          lex = "<";  break;
            default:                      tt = TOKEN_LESS_EQUAL;    lex = "<="; break;
        }
        Token *tok = newToken(tt, lex, op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseEquality(AetherParser *p) {
    AST *node = parseComparison(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_EQUAL_EQUAL || p->current.type == REA_TOKEN_BANG_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseComparison(p);
        if (!right) return NULL;
        TokenType tt = (op.type == REA_TOKEN_EQUAL_EQUAL) ? TOKEN_EQUAL : TOKEN_NOT_EQUAL;
        Token *tok = newToken(tt, (tt == TOKEN_EQUAL) ? "==" : "!=", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseLogicalAnd(AetherParser *p) {
    AST *node = parseEquality(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND_AND) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseEquality(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_AND, "&&", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseLogicalOr(AetherParser *p) {
    AST *node = parseLogicalAnd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_OR_OR) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *right = parseLogicalAnd(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_OR, "||", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

/* Assignment is right-associative and only valid with an lvalue on the left
 * (mirrors rea parseAssignment). Produces AST_ASSIGN. */
static AST *parseExpr(AetherParser *p) {
    AST *left = parseLogicalOr(p);
    if (!left) return NULL;
    if ((left->type == AST_VARIABLE || left->type == AST_FIELD_ACCESS ||
         left->type == AST_ARRAY_ACCESS) &&
        p->current.type == REA_TOKEN_EQUAL) {
        ReaToken op = p->current;
        aetherAdvance(p);
        AST *value = parseExpr(p);
        if (!value) return NULL;
        Token *assignTok = newToken(TOKEN_ASSIGN, "=", op.line, 0);
        AST *node = newASTNode(AST_ASSIGN, assignTok);
        setLeft(node, left);
        setRight(node, value);
        setTypeAST(node, left->var_type);
        return node;
    }
    return left;
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

/* let/const NAME : Type [ = expr ] ;
 * Produces AST_VAR_DECL identical to rea's `Type name = init;` form:
 *   child[0] = AST_VARIABLE(name, var_type),
 *   left  = initializer expr (or NULL),
 *   right = type node,
 *   var_type = mapped type. */
static AST *parseLetDecl(AetherParser *p) {
    aetherAdvance(p); /* consume 'let' or 'const' */

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected name after 'let'.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    Token *nameTok = currentAsIdentifier(p);
    if (!nameTok) return NULL;
    int nameLine = p->current.line;
    aetherAdvance(p); /* consume name */

    AST *typeNode = NULL;
    VarType vtype = TYPE_UNKNOWN;
    if (p->current.type == REA_TOKEN_COLON) {
        aetherAdvance(p); /* consume ':' */
        /* The type name may be a Rea-keyword token (e.g. an Aether alias that
         * the lexer recognized) or an identifier; accept whatever lexeme is
         * here as the type span. */
        if (p->current.type == REA_TOKEN_EOF || p->current.type == REA_TOKEN_EQUAL ||
            p->current.type == REA_TOKEN_SEMICOLON) {
            fprintf(stderr, "L%d: expected type after ':'.\n", p->current.line);
            p->hadError = true;
            freeToken(nameTok);
            return NULL;
        }
        typeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &vtype);
        if (!typeNode) { freeToken(nameTok); return NULL; }
        aetherAdvance(p); /* consume type name */
    } else {
        /* Milestone 1 requires an explicit type; without one we cannot match
         * the rewriter's inferred-type behavior, so report and bail. */
        fprintf(stderr, "L%d: '%s' requires an explicit type ': T' in the AST parser.\n",
                nameLine, nameTok->value ? nameTok->value : "let");
        p->hadError = true;
        freeToken(nameTok);
        return NULL;
    }

    AST *init = NULL;
    if (p->current.type == REA_TOKEN_EQUAL) {
        aetherAdvance(p); /* consume '=' */
        init = parseExpr(p);
        if (!init) {
            freeToken(nameTok);
            if (typeNode) freeAST(typeNode);
            return NULL;
        }
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);
    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);
    return decl;
}

/* ret [expr] ;  ->  AST_RETURN (mirrors rea parseReturn). */
static AST *parseRet(AetherParser *p) {
    int line = p->current.line;
    aetherAdvance(p); /* consume 'ret' */
    AST *value = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON && p->current.type != REA_TOKEN_RIGHT_BRACE &&
        p->current.type != REA_TOKEN_EOF) {
        value = parseExpr(p);
    } else if (p->currentFunctionType != TYPE_VOID) {
        fprintf(stderr, "L%d: return requires a value.\n", line);
        p->hadError = true;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }
    if (p->hadError) return NULL;
    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *node = newASTNode(AST_RETURN, retTok);
    setLeft(node, value);
    setTypeAST(node, value ? value->var_type : TYPE_VOID);
    return node;
}

/* if cond { then } [else { else }]  ->  AST_IF (mirrors rea parseIf). The
 * effect wrapper rules don't apply here; this is the statement form. */
static AST *parseIfStmt(AetherParser *p) {
    aetherAdvance(p); /* consume 'if' */
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        aetherAdvance(p);
    }
    AST *condition = parseExpr(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    }
    AST *thenBranch = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        thenBranch = parseBlock(p);
    } else {
        thenBranch = parseStatement(p);
    }
    AST *elseBranch = NULL;
    if (p->current.type == REA_TOKEN_ELSE) {
        aetherAdvance(p);
        if (p->current.type == REA_TOKEN_IF) {
            elseBranch = parseIfStmt(p);
        } else if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            elseBranch = parseBlock(p);
        } else {
            elseBranch = parseStatement(p);
        }
    }
    AST *node = newASTNode(AST_IF, NULL);
    setLeft(node, condition);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);
    return node;
}

/* Parse a standalone Aether expression from a NUL-terminated text fragment
 * (used for loop-range operands). Returns the expression AST or NULL. */
static AST *parseExprFromText(const char *text) {
    if (!text) return NULL;
    AetherParser sub;
    reaInitLexer(&sub.lexer, text);
    sub.currentFunctionType = TYPE_VOID;
    sub.functionDepth = 0;
    sub.hadError = false;
    aetherAdvance(&sub);
    AST *expr = parseExpr(&sub);
    if (!expr || sub.hadError) {
        if (expr) freeAST(expr);
        return NULL;
    }
    return expr;
}

static char *dupTrimmedRange(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)end[-1])) end--;
    size_t len = (size_t)(end - start);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/* loop NAME in LOW..HIGH { body }
 *
 * The rewriter lowers this to a C-style half-open for loop
 *     for (int i = LOW; i < HIGH; i = i + 1) { body }
 * which rea's parseFor turns into:
 *     COMPOUND[ init-var-decl,
 *               WHILE(cond: i < HIGH,
 *                     body: COMPOUND[ body-block, post-expr-stmt ]) ]
 * We reproduce that exact structure so output matches byte-for-byte.
 *
 * NOTE: the shared Rea lexer cannot tokenize a numeric range -- it folds the
 * dots into adjacent numbers (`0..5` lexes as `0.` then `.5`, swallowing the
 * `..`). A proper fix is an Aether-specific lexer with a `..` token (roadmap
 * P1). For Milestone 1 we recover the range operands from the *raw source*
 * between `in` and the body `{`, splitting on the literal `..` at bracket
 * depth 0, then parse each side as a standalone expression. This handles both
 * numeric and identifier bounds correctly. */
static AST *parseLoopRange(AetherParser *p) {
    aetherAdvance(p); /* consume 'loop' */

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected loop variable name.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    /* Loop variable name; we reuse it several times so capture the lexeme. */
    size_t nlen = (size_t)p->current.length;
    char *nameBuf = (char *)malloc(nlen + 1);
    if (!nameBuf) return NULL;
    memcpy(nameBuf, p->current.start, nlen);
    nameBuf[nlen] = '\0';
    int idLine = p->current.line;
    aetherAdvance(p); /* consume loop var */

    if (!isAetherKeyword(&p->current, "in")) {
        fprintf(stderr, "L%d: expected 'in' in loop range.\n", p->current.line);
        p->hadError = true;
        free(nameBuf);
        return NULL;
    }

    /* Recover the raw range text from `in` to the body-opening `{`. p->current
     * still points at `in`; its lexeme span gives us a pointer into source. */
    const char *afterIn = p->current.start + p->current.length; /* just past "in" */
    const char *scan = afterIn;
    const char *rangeBrace = NULL;
    const char *rangeOp = NULL;
    int depth = 0;
    for (; *scan; scan++) {
        char c = *scan;
        if (c == '(' || c == '[' || c == '{') {
            if (c == '{' && depth == 0) { rangeBrace = scan; break; }
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
        } else if (c == '.' && scan[1] == '.' && depth == 0 && !rangeOp) {
            rangeOp = scan;
            scan++; /* skip the second dot */
        }
    }
    if (!rangeBrace || !rangeOp) {
        fprintf(stderr, "L%d: expected '<low>..<high> {' in loop range.\n", idLine);
        p->hadError = true;
        free(nameBuf);
        return NULL;
    }
    char *lowText = dupTrimmedRange(afterIn, rangeOp);
    char *highText = dupTrimmedRange(rangeOp + 2, rangeBrace);
    if (!lowText || !highText) {
        free(lowText); free(highText); free(nameBuf);
        return NULL;
    }
    AST *low = parseExprFromText(lowText);
    AST *high = parseExprFromText(highText);
    free(lowText);
    free(highText);
    if (!low || !high) {
        fprintf(stderr, "L%d: could not parse loop range bounds.\n", idLine);
        p->hadError = true;
        if (low) freeAST(low);
        if (high) freeAST(high);
        free(nameBuf);
        return NULL;
    }

    /* Resynchronize the main token stream: drive the lexer up to (and onto) the
     * body-opening brace, so parseBlock sees '{' as current. */
    while (p->current.type != REA_TOKEN_LEFT_BRACE && p->current.type != REA_TOKEN_EOF) {
        aetherAdvance(p);
    }

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        body = parseBlock(p);
    } else {
        fprintf(stderr, "L%d: expected '{' to open loop body.\n", idLine);
        p->hadError = true;
        freeAST(low);
        freeAST(high);
        free(nameBuf);
        return NULL;
    }

    /* Build: int i = LOW;  (AST_VAR_DECL, matching rea parseFor init). */
    Token *initNameTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *initVar = newASTNode(AST_VARIABLE, initNameTok);
    setTypeAST(initVar, TYPE_INT64);
    Token *intTypeTok = newToken(TOKEN_IDENTIFIER, "int", idLine, 0);
    AST *intTypeNode = newASTNode(AST_TYPE_IDENTIFIER, intTypeTok);
    setTypeAST(intTypeNode, TYPE_INT64);
    AST *initDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(initDecl, initVar);
    setLeft(initDecl, low);
    setRight(initDecl, intTypeNode);
    setTypeAST(initDecl, TYPE_INT64);

    /* Condition: i < HIGH  (AST_BINARY_OP, BOOLEAN). */
    Token *condVarTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *condVar = newASTNode(AST_VARIABLE, condVarTok);
    setTypeAST(condVar, TYPE_UNKNOWN);
    Token *ltTok = newToken(TOKEN_LESS, "<", idLine, 0);
    AST *cond = newASTNode(AST_BINARY_OP, ltTok);
    setLeft(cond, condVar);
    setRight(cond, high);
    setTypeAST(cond, TYPE_BOOLEAN);

    /* Post: i = i + 1  (AST_ASSIGN of an AST_BINARY_OP). */
    Token *postLhsTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *postLhs = newASTNode(AST_VARIABLE, postLhsTok);
    setTypeAST(postLhs, TYPE_UNKNOWN);
    Token *addLhsTok = newToken(TOKEN_IDENTIFIER, nameBuf, idLine, 0);
    AST *addLhs = newASTNode(AST_VARIABLE, addLhsTok);
    setTypeAST(addLhs, TYPE_UNKNOWN);
    Token *oneTok = newToken(TOKEN_INTEGER_CONST, "1", idLine, 0);
    AST *oneNode = newASTNode(AST_NUMBER, oneTok);
    setTypeAST(oneNode, TYPE_INT64);
    Token *plusTok = newToken(TOKEN_PLUS, "+", idLine, 0);
    AST *addExpr = newASTNode(AST_BINARY_OP, plusTok);
    setLeft(addExpr, addLhs);
    setRight(addExpr, oneNode);
    setTypeAST(addExpr, promoteIntegralBinaryType(TYPE_UNKNOWN, TYPE_INT64));
    Token *assignTok = newToken(TOKEN_ASSIGN, "=", idLine, 0);
    AST *postAssign = newASTNode(AST_ASSIGN, assignTok);
    setLeft(postAssign, postLhs);
    setRight(postAssign, addExpr);
    setTypeAST(postAssign, TYPE_UNKNOWN);
    AST *postStmt = newASTNode(AST_EXPR_STMT, postAssign->token);
    setLeft(postStmt, postAssign);

    /* while body = COMPOUND[ body, postStmt ]  (rea parseFor with post). */
    AST *whileBody = newASTNode(AST_COMPOUND, NULL);
    addChild(whileBody, body);
    addChild(whileBody, postStmt);
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);

    /* outer = COMPOUND[ initDecl, whileNode ]. */
    AST *outer = newASTNode(AST_COMPOUND, NULL);
    addChild(outer, initDecl);
    addChild(outer, whileNode);

    free(nameBuf);
    return outer;
}

static AST *parseStatement(AetherParser *p) {
    /* fx { ... } -- effect wrapper. The marker is erased; we return the inner
     * block directly (an AST_COMPOUND), matching the rewriter which strips the
     * `fx` token and leaves the brace block. */
    if (isAetherKeyword(&p->current, "fx")) {
        aetherAdvance(p); /* consume 'fx' */
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            return parseBlock(p);
        }
        /* `fx` with no following block: a no-op block. */
        return newASTNode(AST_COMPOUND, NULL);
    }
    if (isAetherKeyword(&p->current, "let") || isAetherKeyword(&p->current, "const")) {
        return parseLetDecl(p);
    }
    if (isAetherKeyword(&p->current, "ret")) {
        return parseRet(p);
    }
    if (isAetherKeyword(&p->current, "loop")) {
        /* Only the range form `loop i in a..b` is handled in Milestone 1. */
        return parseLoopRange(p);
    }
    if (p->current.type == REA_TOKEN_IF) {
        return parseIfStmt(p);
    }
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        return parseBlock(p);
    }
    /* Expression statement or assignment. */
    AST *expr = parseExpr(p);
    if (!expr) return NULL;
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        aetherAdvance(p);
    }
    if (expr->type == AST_ASSIGN) {
        return expr; /* assignments act as statements directly */
    }
    AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
    setLeft(stmt, expr);
    return stmt;
}

static AST *parseBlock(AetherParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    aetherAdvance(p); /* consume '{' */
    AST *block = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) break;
        addChild(block, stmt);
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        aetherAdvance(p);
    }
    return block;
}

/* ------------------------------------------------------------------ */
/* Function declarations                                               */
/* ------------------------------------------------------------------ */

/* fn NAME ( [name: Type, ...] ) [ -> RetType ] { body }
 *
 * Mirrors rea parseFunctionDecl: a function with a non-void return type is
 * AST_FUNCTION_DECL with the return-type node on `right` and the body on
 * `extra`; a void function is AST_PROCEDURE_DECL with the body on `right` and
 * no return-type node. Params are AST_VAR_DECL nodes moved into the decl's
 * children[]. */
static AST *parseFnDecl(AetherParser *p) {
    aetherAdvance(p); /* consume 'fn' */

    if (p->current.type != REA_TOKEN_IDENTIFIER) {
        fprintf(stderr, "L%d: expected function name after 'fn'.\n", p->current.line);
        p->hadError = true;
        return NULL;
    }
    Token *nameTok = currentAsIdentifier(p);
    if (!nameTok) return NULL;
    aetherAdvance(p); /* consume function name */

    if (p->current.type != REA_TOKEN_LEFT_PAREN) {
        fprintf(stderr, "L%d: expected '(' after function name.\n", p->current.line);
        p->hadError = true;
        freeToken(nameTok);
        return NULL;
    }
    aetherAdvance(p); /* consume '(' */

    AST *params = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type != REA_TOKEN_IDENTIFIER) {
            fprintf(stderr, "L%d: expected parameter name.\n", p->current.line);
            p->hadError = true;
            break;
        }
        Token *paramNameTok = currentAsIdentifier(p);
        if (!paramNameTok) { p->hadError = true; break; }
        aetherAdvance(p); /* consume param name */

        if (p->current.type != REA_TOKEN_COLON) {
            fprintf(stderr, "L%d: expected ':' after parameter name.\n", p->current.line);
            p->hadError = true;
            freeToken(paramNameTok);
            break;
        }
        aetherAdvance(p); /* consume ':' */

        if (p->current.type == REA_TOKEN_RIGHT_PAREN || p->current.type == REA_TOKEN_COMMA ||
            p->current.type == REA_TOKEN_EOF) {
            fprintf(stderr, "L%d: expected parameter type.\n", p->current.line);
            p->hadError = true;
            freeToken(paramNameTok);
            break;
        }
        VarType pvtype = TYPE_UNKNOWN;
        AST *ptypeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &pvtype);
        if (!ptypeNode) { freeToken(paramNameTok); p->hadError = true; break; }
        aetherAdvance(p); /* consume param type */

        AST *paramVar = newASTNode(AST_VARIABLE, paramNameTok);
        setTypeAST(paramVar, pvtype);
        AST *paramDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(paramDecl, paramVar);
        setRight(paramDecl, ptypeNode);
        setTypeAST(paramDecl, pvtype);
        addChild(params, paramDecl);

        if (p->current.type == REA_TOKEN_COMMA) {
            aetherAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        aetherAdvance(p);
    }

    /* Optional '-> RetType'. Default return type is Void. */
    AST *returnTypeNode = NULL;
    VarType vtype = TYPE_VOID;
    if (p->current.type == REA_TOKEN_ARROW) {
        aetherAdvance(p); /* consume '->' */
        if (p->current.type == REA_TOKEN_LEFT_BRACE || p->current.type == REA_TOKEN_EOF) {
            fprintf(stderr, "L%d: expected return type after '->'.\n", p->current.line);
            p->hadError = true;
        } else {
            returnTypeNode = buildTypeNode(p->current.start, p->current.length, p->current.line, &vtype);
            aetherAdvance(p); /* consume return type */
        }
    }

    /* Body. */
    VarType prevType = p->currentFunctionType;
    int prevDepth = p->functionDepth;
    p->currentFunctionType = vtype;
    p->functionDepth++;

    AST *block = NULL;
    bool hasBody = false;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        block = parseBlock(p);
        hasBody = true;
    }

    p->currentFunctionType = prevType;
    p->functionDepth = prevDepth;

    if (p->hadError) {
        freeAST(params);
        if (returnTypeNode) freeAST(returnTypeNode);
        if (block) freeAST(block);
        freeToken(nameTok);
        return NULL;
    }

    /* If a non-void function declared no '->' return type but the body returns
     * a value, that is a user error; for Milestone 1 we follow the explicit
     * '->'. A Void function uses AST_PROCEDURE_DECL. */
    AST *func = (vtype == TYPE_VOID) ? newASTNode(AST_PROCEDURE_DECL, nameTok)
                                     : newASTNode(AST_FUNCTION_DECL, nameTok);

    /* Move params into the function node's children[] (rea does this move). */
    if (params->child_count > 0) {
        func->children = params->children;
        func->child_count = params->child_count;
        func->child_capacity = params->child_capacity;
        for (int i = 0; i < func->child_count; i++) {
            if (func->children[i]) func->children[i]->parent = func;
        }
        params->children = NULL;
        params->child_count = 0;
        params->child_capacity = 0;
    }
    freeAST(params);

    if (vtype == TYPE_VOID) {
        setRight(func, block);          /* procedure: body on right, no ret type */
        if (returnTypeNode) freeAST(returnTypeNode);
    } else {
        setRight(func, returnTypeNode); /* function: return type on right */
        setExtra(func, block);          /*           body on extra */
    }
    setTypeAST(func, vtype);

    registerFunctionSymbol(func, nameTok->value ? nameTok->value : "", vtype, hasBody);
    return func;
}

/* ------------------------------------------------------------------ */
/* Program entry                                                       */
/* ------------------------------------------------------------------ */

AST *parseAetherAst(const char *source) {
    if (!source) return NULL;

    AetherParser p;
    reaInitLexer(&p.lexer, source);
    p.currentFunctionType = TYPE_VOID;
    p.functionDepth = 0;
    p.hadError = false;
    aetherAdvance(&p);

    /* Build the AST_PROGRAM root exactly like rea parseRea: a program whose
     * `right` is an AST_BLOCK holding two AST_COMPOUND children -- declarations
     * and statements. */
    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);
    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    while (p.current.type != REA_TOKEN_EOF && !p.hadError) {
        if (!isAetherKeyword(&p.current, "fn")) {
            const char *path = aetherSemanticGetSourcePath();
            if (path && *path) {
                fprintf(stderr, "%s:%d: ", path, p.current.line);
            }
            fprintf(stderr,
                    "Unexpected token '%.*s' at line %d (expected a top-level 'fn' declaration).\n",
                    (int)p.current.length, p.current.start, p.current.line);
            p.hadError = true;
            break;
        }
        AST *fn = parseFnDecl(&p);
        if (!fn) {
            p.hadError = true;
            break;
        }
        addChild(decls, fn);
    }

    if (p.hadError) {
        freeAST(program);
        return NULL;
    }

    /* If a routine named 'main' exists and there are no top-level statements,
     * inject an implicit `main()` call so the VM runs user code on start --
     * identical to rea parseRea's tail. */
    bool has_main = false;
    for (int i = 0; i < decls->child_count; i++) {
        AST *d = decls->children[i];
        if (!d) continue;
        if ((d->type == AST_FUNCTION_DECL || d->type == AST_PROCEDURE_DECL) &&
            d->token && d->token->value && strcasecmp(d->token->value, "main") == 0) {
            has_main = true;
        }
    }
    if (stmts->child_count == 0 && has_main) {
        Token *mainTok = newToken(TOKEN_IDENTIFIER, "main", 0, 0);
        AST *call = newASTNode(AST_PROCEDURE_CALL, mainTok);
        AST *stmt = newASTNode(AST_EXPR_STMT, call->token);
        setLeft(stmt, call);
        addChild(stmts, stmt);
    }

    return program;
}
