#ifndef PSCAL_AETHER_SEMANTIC_H
#define PSCAL_AETHER_SEMANTIC_H

#include "ast/ast.h"

void aetherPerformSemanticAnalysis(AST *root);
void aetherSemanticSetSourcePath(const char *path);
const char *aetherSemanticGetSourcePath(void);
int aetherGetLoadedModuleCount(void);
AST *aetherGetModuleAST(int index);
const char *aetherGetModulePath(int index);
const char *aetherGetModuleName(int index);
char *aetherResolveImportPath(const char *path);
void aetherSemanticResetState(void);

/* A builtin verified to always return Real (the NARROW-001 table). */
int aetherIsAlwaysRealBuiltin(const char *name);
/* A TOON / MStream builtin the source-level checks type by name. */
int aetherIsNameTypedBuiltin(const char *name);
/* A coded diagnostic from outside semantic.c (the experiment arms). An error
 * increments the semantic error count; a warning does not. */
void aetherSemanticReportCoded(const char *code, const char *kind, int line,
                               const char *detail, int isError);
/* The file name diagnostics are reported under, swapped while a walk visits
 * an imported module (unlike aetherSemanticSetSourcePath, it leaves rea's
 * import state alone). Returns the previous one. */
const char *aetherSemanticSwapReportPath(const char *path);
/* The value names the program declares (lets, parameters, consts, functions),
 * as the D70 case pass collected them, for a close-spelling hint. */
/* A builtin's return type as builtins_json documents it, when an inferred
 * `let` can carry it (a scalar, a handle, an array of those); NULL otherwise.
 * `name` is the engine or the Aether name. */
const char *aetherBuiltinDocumentedReturnType(const char *name);
size_t aetherSemanticDeclaredValueCount(void);
const char *aetherSemanticDeclaredValueAt(size_t i);

#endif
