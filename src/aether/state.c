#include "aether/state.h"

#include "aether/parser.h"
#include "aether/diagnostics.h"
#include "aether/ast_prepasses.h"
#include "aether/semantic.h"
#include "rea/state.h"
#include "rea/frontend_hooks.h"
#include "backend_ast/builtin.h"
#include "common/frontend_kind.h"

static int g_aether_verbose_compatibility_diagnostics = 0;

void aetherResetSymbolState(void) {
    reaResetSymbolState();
}

void aetherInvalidateGlobalState(void) {
    aetherClearLastSource();
    /* The fx-block/@pure/call-alias registries accumulate across parseAether
     * calls (main program + imported modules) because semantic analysis runs
     * after all of them; this is the one place they are torn down. */
    aetherAstClearSemanticRegistries();
    aetherAstPrepassClearReportedArity();
    reaInvalidateGlobalState();
}

void aetherSetVerboseCompatibilityDiagnostics(int enable) {
    g_aether_verbose_compatibility_diagnostics = enable ? 1 : 0;
}

int aetherGetVerboseCompatibilityDiagnostics(void) {
    return g_aether_verbose_compatibility_diagnostics;
}

/* Install the Aether implementations of the shared engine's frontend hooks.
 * Called explicitly from the engine's main() via PSCAL_FRONTEND_INSTALL_HOOKS, so
 * the call site forces this translation unit to link even inside a static library
 * (a constructor could be dropped). The plain Rea front end omits this unit and
 * uses the engine's built-in defaults. */
void aetherInstallFrontendHooks(void) {
    static const ReaFrontendHooks hooks = {
        .parseSource = parseAether,
        .setStrictMode = aetherSetStrictMode,
        .resetSymbolState = aetherResetSymbolState,
        .invalidateGlobalState = aetherInvalidateGlobalState,
        .semanticSetSourcePath = aetherSemanticSetSourcePath,
        .performSemanticAnalysis = aetherPerformSemanticAnalysis,
        .getLoadedModuleCount = aetherGetLoadedModuleCount,
        .getModuleAST = aetherGetModuleAST,
        .getModulePath = aetherGetModulePath,
        .getModuleName = aetherGetModuleName,
        .resolveImportPath = aetherResolveImportPath,
        .inferDiagnosticCode = aetherInferDiagnosticCode,
        .setVerboseCompatibilityDiagnostics = aetherSetVerboseCompatibilityDiagnostics,
        .getVerboseCompatibilityDiagnostics = aetherGetVerboseCompatibilityDiagnostics,
        .unknownIdentifierHint = aetherUnknownIdentifierHint,
        .swapReportPath = aetherSemanticSwapReportPath,
    };
    reaSetFrontendHooks(&hooks);
    /* Builtin names given at run time (task_spawn("dns_lookup", ...),
     * has_builtin("core", "file_exists")) take the Aether spellings. */
    registerBuiltinNameAlias(FRONTEND_KIND_AETHER, aetherRuntimeBuiltinAlias);
    /* The builtin listings name only what a program can call. */
    registerBuiltinNameRejected(FRONTEND_KIND_AETHER, aetherBuiltinNameRemoved);
}
