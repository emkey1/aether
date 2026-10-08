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
/* A coded diagnostic from outside semantic.c (the experiment arms). An error
 * increments the semantic error count; a warning does not. */
void aetherSemanticReportCoded(const char *code, const char *kind, int line,
                               const char *detail, int isError);

#endif
