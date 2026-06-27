# aether — a contract / effect-typed front end for the PSCAL VM, built on Rea.
#
# Aether lowers to Rea: it reuses the shared Rea front-end engine (the `rea` repo)
# and installs its own parser, semantic analysis and diagnostics over the engine's
# hook seam (rea's src/rea/frontend_hooks.h), producing the shared PSCAL AST that
# pscal-core's VM runs.
#
# FetchContent pulls in `rea`, which transitively pulls in `pscal-core`, so building
# this repo on its own fetches and builds the whole stack automatically.
cmake_minimum_required(VERSION 3.18)
project(aether C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall)
if(UNIX AND NOT APPLE)
    add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options(-Wno-nullability-completeness)
endif()

find_package(Threads REQUIRED)

include(FetchContent)
# Backend engine deps are vendored as git submodules under external/, so a normal
# `git clone --recurse-submodules` / `git pull --recurse-submodules` brings them and a
# plain `git pull` carries backend (rea/pscal-core) fixes. pscal-core is declared FIRST
# so this declaration wins (FetchContent honours the first declaration of a name),
# overriding rea's transitive pull; both then resolve to the pinned submodule checkouts.
# The PBuild umbrella overrides these via FETCHCONTENT_SOURCE_DIR_{PSCAL_CORE,REA} to
# reuse its shared sibling checkouts instead.
# Optional SDL backend: aether opts pscal-core into its SDL graphics/audio build.
# Forced into the cache BEFORE the subbuild so pscal_core's option() picks it up.
option(AETHER_ENABLE_SDL "Build aether with the pscal-core SDL backend" OFF)
if(AETHER_ENABLE_SDL)
    set(PSCAL_SDL ON CACHE BOOL "Enable SDL in pscal-core (forced by aether)" FORCE)
endif()
FetchContent_Declare(pscal_core SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/pscal-core)
FetchContent_MakeAvailable(pscal_core)
FetchContent_Declare(rea SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/rea)
# Brings in the Rea engine sources (pscal-core already populated above, so rea reuses it).
FetchContent_MakeAvailable(rea)

include_directories(
    ${pscal_core_SOURCE_DIR}/src
    ${pscal_core_SOURCE_DIR}/src/third_party/yyjson
    ${rea_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Aether language version: the date of the last language-affecting change
# (YYYY-MM-DD-N), checked into VERSION so every node that builds this commit
# reports the same value. Bumped only on language changes via
# tools/bump_version.py, never per build, so the date means "semantics last
# changed" -- which makes node version-skew self-evident.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AETHER_LANG_VERSION LIMIT_COUNT 1)
if(NOT AETHER_LANG_VERSION)
    set(AETHER_LANG_VERSION "unknown")
endif()
message(STATUS "Aether language version: ${AETHER_LANG_VERSION}")

add_compile_definitions(
    PROGRAM_VERSION="${AETHER_LANG_VERSION}"
    PSCAL_GIT_TAG="untagged"
    ENABLE_EXT_BUILTIN_MATH ENABLE_EXT_BUILTIN_STRINGS ENABLE_EXT_BUILTIN_SYSTEM
    ENABLE_EXT_BUILTIN_USER ENABLE_EXT_BUILTIN_YYJSON ENABLE_EXT_BUILTIN_3D
    ENABLE_EXT_BUILTIN_GRAPHICS
)

# The shared Rea engine, consumed from the rea repo. Compiled here with Aether's
# front-end definitions so the engine's main() installs Aether's hooks.
set(ENGINE_SOURCES
    ${rea_SOURCE_DIR}/src/rea/main.c
    ${rea_SOURCE_DIR}/src/rea/lexer.c
    ${rea_SOURCE_DIR}/src/rea/parser.c
    ${rea_SOURCE_DIR}/src/rea/semantic.c
    ${rea_SOURCE_DIR}/src/rea/state.c
    ${rea_SOURCE_DIR}/src/rea/builtins/thread.c
    ${rea_SOURCE_DIR}/src/rea/type_stubs.c
    ${rea_SOURCE_DIR}/src/rea/frontend_hooks.c
)

# Aether's own overrides, installed over the engine hook seam at startup.
set(AETHER_SOURCES
    ${ENGINE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/diagnostics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/translate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_prepasses.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/semantic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/state.c
)

add_executable(aether ${AETHER_SOURCES})
target_compile_definitions(aether PRIVATE
    FRONTEND_REA
    PSCAL_FRONTEND_MAIN_NAME=aether_main
    PSCAL_FRONTEND_KIND=FRONTEND_KIND_AETHER
    PSCAL_FRONTEND_DISPLAY_NAME=\"Aether\"
    PSCAL_FRONTEND_USAGE_NAME=\"aether\"
    PSCAL_FRONTEND_COMPILER_ID=\"aether\"
    PSCAL_FRONTEND_HAS_REWRITE_DUMP=1
    PSCAL_FRONTEND_INSTALL_HOOKS=aetherInstallFrontendHooks
)
target_link_libraries(aether PRIVATE pscal_core_static Threads::Threads m)

# ---- Install ----------------------------------------------------------------
# `cmake --install build` puts `aether` on PATH and ships the examples and the
# language docs beside it. The FetchContent deps (rea, pscal-core) guard their
# own install rules to standalone builds, so only Aether's targets install here.
include(GNUInstallDirs)
install(TARGETS aether RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/aether/examples)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs/
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---- Test -------------------------------------------------------------------
# `ctest` runs the .aether corpus in tests/ against the binary just built.
enable_testing()
add_test(NAME aether_tests COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh)
set_tests_properties(aether_tests PROPERTIES
    ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>")
