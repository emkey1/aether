#ifndef PSCAL_AETHER_PARSER_H
#define PSCAL_AETHER_PARSER_H

#include "ast/ast.h"

AST *parseAether(const char *source);

/* Experimental recursive-descent parser (Milestone 1). Builds the shared pscal
 * AST directly from Aether source instead of rewriting to Rea text first.
 * Selected at runtime by parseAether() when AETHER_PARSER=ast; the default
 * remains the text rewriter. */
AST *parseAetherAst(const char *source);

void aetherSetStrictMode(int enable);
const char *aetherGetLastSource(void);
void aetherClearLastSource(void);

#endif
