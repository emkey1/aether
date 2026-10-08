#ifndef PSCAL_AETHER_AST_PREPASSES_H
#define PSCAL_AETHER_AST_PREPASSES_H

/*
 * Source pre-passes for the Aether AST frontend, implemented in
 * ast_prepasses.c. They are Aether-owned: they started as ports of the retired
 * text rewriter's passes and have since diverged from it (string literals and
 * comments are no longer alias-rewritten); tests/ is their contract.
 */

/* TOON pre-pass: lower `toon:` blocks to escaped string literals before the
 * lexer runs. Returns a malloc'd copy the caller frees, or NULL on error (a
 * diagnostic is reported via `path`). Line numbers are preserved. */
char *aetherAstPrepassToonBlocks(const char *source, const char *path);

/* Builtin-alias pre-pass: lower stdlib/TOON/capability builtin call spellings
 * (toon_* -> Yyjson*, has_toon/string_eq/print/len/... -> canonical builtins)
 * as a pure text transform. Returns a malloc'd copy (caller frees) or NULL.
 * Line structure preserved. */
char *aetherAstPrepassBuiltins(const char *source);

/* Lower the context-free `string_eq(a, b)` inline call alias to `(a == b)` as a
 * pure text transform. Returns a malloc'd copy (caller frees) or NULL. */
char *aetherAstPrepassInlineEq(const char *source);

/* Sink for aetherAstCollectImportedTypes: called once per exported binding of an
 * imported module. `name` is the binding/function name, `aetherType` its Aether
 * type name (or function return type), and `isFunction` is 1 for a function
 * return type, 0 for a const/let binding. The strings are owned by the caller
 * and valid only for the duration of the call. */
typedef void (*AetherImportTypeSink)(void *ctx, const char *name,
                                     const char *aetherType, int isFunction);

/* Function-return-table key prefix that marks an extension method `fn f(self:
 * T)` as `<prefix>T.f`, so the call-site UFCS rewrite `f(recv)` -> `recv.f()`
 * fires for it and not for an ordinary method, whose key is plain `T.f`. */
#define AETHER_EXT_METHOD_PREFIX "ext:"

/* Scan `mainSource` for `use` imports, load each module file, and report its
 * exported const/let binding types and fn return types via `sink`. */
void aetherAstCollectImportedTypes(const char *mainSource, const char *path,
                                   AetherImportTypeSink sink, void *ctx);

#endif
