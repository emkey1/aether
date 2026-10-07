/*
 * ast_checks.c -- front-end checks over the parsed Aether AST.
 *
 * Undefined `Type.method` calls (aetherCheckMemberCalls, SCOPE-001), the
 * FLOW-001 fallthrough helpers, the PAR-001 shared-record guard and the
 * ARR-002 rank check. Split out of ast_parser.c; see ast_internal.h.
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
 * (`self.rows[i][j]`) is not AST_VARIABLE, so neither is judged.
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
        if (declared && aetherTypeNameRank(declared) == 1) {
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
