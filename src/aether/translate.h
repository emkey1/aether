#ifndef PSCAL_AETHER_TRANSLATE_H
#define PSCAL_AETHER_TRANSLATE_H

#include "core/diagmap.h"  /* rewritten-line -> source-line map (shared core facility) */

char *aetherRewriteSource(const char *source, const char *path);

/* Exposed for the AST parser (AETHER_PARSER=ast): run the same TOON extraction
 * pre-pass the rewriter uses, lowering `toon:` blocks to escaped string literals
 * before the lexer sees the source. Returns a malloc'd copy the caller frees, or
 * NULL on error (a diagnostic is reported via `path`). Line numbers are
 * preserved (each consumed TOON block line becomes a blank line). */
char *aetherPreprocessToonBlocksForAst(const char *source, const char *path);

/* Exposed for the AST parser: lower stdlib/TOON/capability builtin call spellings
 * (toon_*->Yyjson*, has_toon/string_eq/print/len/... -> canonical builtins) as a
 * pure text pre-pass, reusing the rewriter's alias machinery. Returns a malloc'd
 * copy (caller frees) or NULL. Line structure preserved. */
char *aetherPreprocessBuiltinsForAst(const char *source);

/* Exposed for the AST parser: lower the context-free `string_eq(a, b)` inline
 * call alias to `(a == b)` as a pure text pre-pass (the rewriter does this in
 * translateLine via appendAetherInlineCallAlias). Returns a malloc'd copy (caller
 * frees) or NULL. Line structure preserved. */
char *aetherPreprocessInlineEqForAst(const char *source);

/* Sink for aetherCollectImportedTypesForAst: called once per exported binding of
 * an imported module. `name` is the binding/function name, `aetherType` its Aether
 * type name (or function return type), and `isFunction` is 1 for a function return
 * type, 0 for a const/let binding. The strings are owned by the caller and valid
 * only for the duration of the call. */
typedef void (*AetherImportTypeSink)(void *ctx, const char *name,
                                     const char *aetherType, int isFunction);

/* Exposed for the AST parser: scan `mainSource` for `use` imports, load each
 * module file, and report its exported const/let binding types and fn return
 * types via `sink`, so the AST path can infer `let x = ImportedConst;` /
 * `let y = importedFn();`. Mirrors the rewriter's maybeLoadImportedBindings. */
void aetherCollectImportedTypesForAst(const char *mainSource, const char *path,
                                      AetherImportTypeSink sink, void *ctx);

#endif
