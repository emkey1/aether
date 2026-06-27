#include "aether/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether/semantic.h"
#include "aether/translate.h"
#include "rea/parser.h"

static char *g_aether_last_source = NULL;

static void aetherRememberSource(const char *source) {
    size_t len;
    char *copy;

    free(g_aether_last_source);
    g_aether_last_source = NULL;
    if (!source) {
        return;
    }
    len = strlen(source);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, source, len + 1);
    g_aether_last_source = copy;
}

AST *parseAether(const char *source) {
    char *rewritten;
    AST *ast;
    const char *sourcePath;

    if (!source) {
        return NULL;
    }

    aetherRememberSource(source);

    /* Experimental AST frontend (Milestone 1): when AETHER_PARSER=ast, parse
     * Aether straight to the shared pscal AST. The default path below (the text
     * rewriter -> parseRea) is unchanged. */
    {
        const char *parserMode = getenv("AETHER_PARSER");
        if (parserMode && strcmp(parserMode, "ast") == 0) {
            return parseAetherAst(source);
        }
    }

    sourcePath = aetherSemanticGetSourcePath();
    rewritten = aetherRewriteSource(source, sourcePath);
    if (!rewritten) {
        return NULL;
    }

    if (getenv("AETHER_DUMP_REWRITE")) {
        fprintf(stderr, "=== AETHER REWRITE BEGIN ===\n%s\n=== AETHER REWRITE END ===\n", rewritten);
    }

    ast = parseRea(rewritten);
    free(rewritten);
    return ast;
}

void aetherSetStrictMode(int enable) {
    reaSetStrictMode(enable);
}

const char *aetherGetLastSource(void) {
    return g_aether_last_source;
}

void aetherClearLastSource(void) {
    free(g_aether_last_source);
    g_aether_last_source = NULL;
}
