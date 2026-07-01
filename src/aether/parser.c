#include "aether/parser.h"

#include <stdlib.h>
#include <string.h>

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
    if (!source) {
        return NULL;
    }

    aetherRememberSource(source);

    /* The AST frontend is the only parser. The legacy line-oriented text
     * rewriter (translate.c, selected via AETHER_PARSER=rewriter) was retired
     * on 2026-07-01 after the P7 cutover proved out; see docs/parser_roadmap.md. */
    return parseAetherAst(source);
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
