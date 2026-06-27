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

#endif
