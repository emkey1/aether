#ifndef PSCAL_AETHER_PARSER_H
#define PSCAL_AETHER_PARSER_H

#include "ast/ast.h"

AST *parseAether(const char *source);

/* The Aether recursive-descent parser. Builds the shared pscal AST directly
 * from Aether source; parseAether() is a thin wrapper over it. (The legacy
 * text rewriter this replaced was retired 2026-07-01; see docs/parser_roadmap.md.) */
AST *parseAetherAst(const char *source);

void aetherSetStrictMode(int enable);
const char *aetherGetLastSource(void);
void aetherClearLastSource(void);

/* --- Parser side-registries for AST-based semantic checks -------------------
 * The parser erases surface-only constructs while lowering to the shared pscal
 * AST (`fx` markers become plain blocks, `@pure` carries no codegen, builtin
 * aliases like `println` are canonicalized to `writeln`). These registries
 * preserve that information so semantic.c can run the effect-fence and purity
 * checks on the real AST instead of re-scanning source text.
 *
 * Lifetime: entries ACCUMULATE across parseAether calls (the main program and
 * each imported module are separate parses, and semantic analysis runs after
 * all of them; node pointers are unique). They are cleared only by
 * aetherAstClearSemanticRegistries(), invoked from aetherInvalidateGlobalState. */

/* Mark `block` (the AST_COMPOUND a `fx { ... }` statement lowered to) as an
 * effect region. `line` is the source line of the `fx` keyword, kept for
 * diagnostics (an empty fx block has no tokens to recover a line from). */
void aetherAstRegisterFxBlock(const AST *block, int line);
int aetherAstNodeIsFxBlock(const AST *node);
/* Line of the `fx` keyword for a registered fx block, or 0 if unregistered. */
int aetherAstFxBlockLine(const AST *node);

/* Record a function/method declaration and its @pure annotation. Methods are
 * registered under both the mangled `Type.method` name and the bare name. */
void aetherAstRegisterFunctionPurity(const char *name, int isPure);
/* Returns 1 if `name` is a declared Aether function; *isPure set when known. */
int aetherAstLookupFunctionPurity(const char *name, int *isPure);

/* Record a top-level (non-method, i.e. not declared inside a `type { ... }`
 * body) function declaration's bare name. Used to let a user's own function
 * shadow a same-named PSCAL vm_builtin for FX-001/ANN-001 purposes: a call
 * site should be judged against the user's declaration, not a name collision
 * with an unrelated builtin (see docs/ideas_and_todo.md, the `swap` entry). */
void aetherAstRegisterTopLevelFunction(const char *name);
/* Returns 1 if `name` names a top-level user-declared function. */
int aetherAstIsTopLevelUserFunction(const char *name);

/* Remember the surface spelling of an aliased builtin call (e.g. the user
 * wrote `println`, the AST node says `writeln`) so diagnostics can quote the
 * name the user actually typed. Returns NULL when the call was not aliased. */
void aetherAstRegisterCallSurfaceName(const AST *call, const char *surfaceName);
const char *aetherAstCallSurfaceName(const AST *call);

/* Mark a compiler-synthesized subtree (e.g. an injected @pre/@post guard,
 * whose failure path calls writeln/halt) so the AST effect/purity checks skip
 * it: those calls are the contract machinery, not user code. */
void aetherAstRegisterSynthesizedSubtree(const AST *node);
int aetherAstNodeIsSynthesizedSubtree(const AST *node);

/* Same purpose as the per-node surface-name registry, but for aliases the
 * TEXT pre-pass (aetherAstPrepassBuiltins) rewrites before the lexer ever
 * runs -- there is no AST node to key on at that point, so the record is
 * keyed (source line, canonical name) instead. `line` 0 (no line context,
 * e.g. contract-expression re-parses) is ignored. */
void aetherAstRegisterAliasAtLine(int line, const char *canonical, const char *surface);
const char *aetherAstAliasSurfaceAtLine(int line, const char *canonical);

void aetherAstClearSemanticRegistries(void);

#endif
