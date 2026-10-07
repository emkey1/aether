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

# Default to an optimised build. With no build type CMake compiles at -O0, and the
# benchmark ran on that: a 10^7-step loop took 26 s, against 11 s for Release. The
# default is a cache value, so -DCMAKE_BUILD_TYPE=Debug (or any other) still wins.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default Release)" FORCE)
endif()
message(STATUS "Aether build type: ${CMAKE_BUILD_TYPE}")

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
# libcurl networking: aether opts pscal-core into its HTTP/socket builtins.
# ON by default -- `http*`/`socket*` are documented language surface in all three
# guides, and tests/run.sh exercises them, so a build without them is neither what
# the docs describe nor able to pass the suite. Configure with
# -DAETHER_ENABLE_CURL=OFF to build a network-less binary.
option(AETHER_ENABLE_CURL "Build aether with the pscal-core libcurl networking backend" ON)
if(AETHER_ENABLE_CURL)
    set(PSCAL_CURL ON CACHE BOOL "Enable libcurl networking in pscal-core (forced by aether)" FORCE)
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
# file(STRINGS) reads VERSION at configure time and bakes it into PROGRAM_VERSION
# below, so a plain incremental build after a VERSION bump would keep reporting
# the stale version. Register VERSION as a configure dependency so any change to
# it forces a reconfigure (and thus a recompile of the version string).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT AETHER_LANG_VERSION)
    set(AETHER_LANG_VERSION "unknown")
endif()
message(STATUS "Aether language version: ${AETHER_LANG_VERSION}")

# Which published image this binary was built for, e.g.
# pscal-rootfs-2026.09.19.1. Passed in by the rootfs build; a developer build
# leaves it "untagged". Passed rather than discovered with `git describe` on
# purpose: the rootfs build clones --depth 1, and a shallow clone carries no
# tags to describe. The matching git tag is pushed to this repo after the image
# is published, so the string names a commit you can actually check out.
set(PSCAL_ROOTFS_TAG "untagged" CACHE STRING
    "Name of the published rootfs build this binary belongs to")

add_compile_definitions(
    PROGRAM_VERSION="${AETHER_LANG_VERSION}"
    PSCAL_GIT_TAG="${PSCAL_ROOTFS_TAG}"
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_prepasses.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_lower.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_types.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aether/ast_checks.c
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
# Compile-check every shipped example (keep-going, capability-gated) so the
# examples tree cannot rot silently.
add_test(NAME aether_examples COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_examples.sh)
set_tests_properties(aether_examples PROPERTIES
    ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>")

# ---- Public hygiene -----------------------------------------------------------
# This repo is public: no host, fleet, credential or session data in tracked files.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME aether_public_hygiene
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_public_hygiene.py
                     --root ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(aether_public_hygiene PROPERTIES LABELS "lint" SKIP_RETURN_CODE 77)
endif()

# ---- Stress laps (off by default: -DAETHER_STRESS_TESTS=ON, then ctest -L stress)
# tests/stress_par.sh runs each par fixture 960 times while every core is kept
# busy, to catch output that depends on thread timing. It takes minutes and
# loads the whole machine, so it never gates and is not registered unless asked
# for: a plain `ctest` stays fast. RUN_SERIAL keeps `ctest -j` from running other
# tests beside it. STRESS_PAR_RUNS=<n> in the environment shortens or lengthens
# the lap.
option(AETHER_STRESS_TESTS "Register the stress laps (minutes, loads every core)" OFF)
if(AETHER_STRESS_TESTS)
add_test(NAME aether_stress_par COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/stress_par.sh)
set_tests_properties(aether_stress_par PROPERTIES
    LABELS stress
    RUN_SERIAL TRUE
    TIMEOUT 3600
    ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>")
endif()

# ---- Guide, doc and invariance gates ----------------------------------------
# The three LLM-facing guides are the product; these tests turn the invariants
# docs/aether_doc_maintenance.md states into gates. All are Python 3 scripts
# under tools/. The token gate needs tiktoken (tools/requirements-docs.txt) and
# reports Skipped, not Passed, without it: point AETHER_DOCS_PYTHON at an
# interpreter that has it, e.g. -DAETHER_DOCS_PYTHON=$HOME/venv/bin/python.
# Labels: `guide` on the doc gates (`report` on the two that only report until
# the first guide pass), `invariance` on the reformat gate; the developer run is
# `ctest -LE 'stress|metric'`.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(AETHER_DOCS_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
    "Python 3 used by the guide/doc gates (needs tiktoken for aether_guide_tokens)")
if(NOT AETHER_DOCS_PYTHON)
    message(STATUS "Guide/doc gates not registered: no Python 3 interpreter found")
else()
    set(AETHER_GUIDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/docs)
    set(AETHER_GUIDE_full ${AETHER_GUIDE_DIR}/aether_for_llms_and_others.md)
    set(AETHER_GUIDE_medium ${AETHER_GUIDE_DIR}/aether_for_llms_medium_contexts.md)
    set(AETHER_GUIDE_small ${AETHER_GUIDE_DIR}/aether_for_llms_with_small_contexts.md)

    # Every ```aether block in each guide compiles, or fails with exactly the
    # codes its EXPECT_FAIL entry names; a fragment that compiles only inside an
    # fx block it does not show fails too (tools/verify_guide_snippets.py).
    foreach(guide full medium small)
        add_test(NAME aether_guide_snippets_${guide}
                 COMMAND ${AETHER_DOCS_PYTHON}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_guide_snippets.py
                         ${AETHER_GUIDE_${guide}})
        set_tests_properties(aether_guide_snippets_${guide} PROPERTIES
            ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>"
            LABELS "guide")
    endforeach()
    # The EXPECT_FAIL keys are shared by all three guides, so whether each one
    # still matches a block is checked once, over all three.
    add_test(NAME aether_guide_snippet_keys
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_guide_snippets.py --keys-only
                     ${AETHER_GUIDE_full} ${AETHER_GUIDE_medium} ${AETHER_GUIDE_small})
    set_tests_properties(aether_guide_snippet_keys PROPERTIES LABELS "guide")

    # The prior-alignment card: every block compiles, and every complete
    # program runs to the output the ```text block after it states. Its
    # 2,000-token ceiling is checked by aether_guide_tokens, its stamp by
    # aether_guide_stamps.
    add_test(NAME aether_card_snippets
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_guide_snippets.py --run-outputs
                     ${AETHER_GUIDE_DIR}/aether_card.md)
    set_tests_properties(aether_card_snippets PROPERTIES
        ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>"
        LABELS "guide")

    # The full guide's generated builtin appendix matches what this build
    # registers. Skipped (77) on a non-canonical build such as SDL.
    add_test(NAME aether_builtin_appendix
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_builtin_appendix.py --check)
    set_tests_properties(aether_builtin_appendix PROPERTIES
        ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>"
        SKIP_RETURN_CODE 77
        LABELS "guide")

    # Medium <= 15,000 o200k tokens, whole document (staged per D6; small, full
    # and the Qwen3.5 count are reported only), and the card <= 2,000 under the
    # larger of o200k and Qwen3.5. Writes guide_sizes.md here.
    add_test(NAME aether_guide_tokens
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_guide_tokens.py
                     --sizes-out ${CMAKE_BINARY_DIR}/guide_sizes.md)
    set_tests_properties(aether_guide_tokens PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "guide")

    # Each guide's text is the text its stamp names (docs/guide_stamps.json),
    # every stamp has its changelog row, and VERSION matches CHANGELOG.md.
    add_test(NAME aether_guide_stamps
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_guide_stamps.py)
    set_tests_properties(aether_guide_stamps PROPERTIES LABELS "guide")

    # Every commit hash README.md, CHANGELOG.md and docs/**/*.md cite resolves
    # in aether, rea or pscal-core (or the umbrella, when PSCAL_UMBRELLA is set
    # in the environment), and no placeholder hash ships. Skipped (77) without
    # full git history.
    add_test(NAME aether_doc_refs
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_doc_refs.py)
    set_tests_properties(aether_doc_refs PROPERTIES
        ENVIRONMENT "AETHER_REA_DIR=${rea_SOURCE_DIR};AETHER_PSCAL_CORE_DIR=${pscal_core_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        LABELS "guide")

    # The "LLM-applicable content only" audit (provenance, build, history and
    # repo-meta terms in medium and small; the copied "yyjson unavailable"
    # literal and benchmark identifiers in all three). REPORT mode until the
    # first guide pass: prints every hit, exits 0. The pass adds --strict.
    add_test(NAME aether_guide_audit
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/audit_guides.py)
    set_tests_properties(aether_guide_audit PROPERTIES LABELS "guide;report")

    # The diagnostic-code map: codes the fixtures make the compiler print,
    # against code literals in the sources and each guide's repair rules.
    # REPORT mode until the first guide pass (prints findings, exits 0).
    add_test(NAME aether_diag_codes
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_diag_codes.py)
    set_tests_properties(aether_diag_codes PROPERTIES
        ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>;AETHER_REA_DIR=${rea_SOURCE_DIR};AETHER_PSCAL_CORE_DIR=${pscal_core_SOURCE_DIR}"
        LABELS "guide;report")

    # Layout-only rewrites (wrapped arguments, a `let` broken after `=`,
    # re-indentation, trailing comments, joined statements) of every pass
    # fixture and example must print the same stdout with the same exit status.
    # tests/reformat_invariance.known lists today's divergences and only
    # shrinks: a new divergence fails, and so does a listed pair that passes.
    add_test(NAME aether_reformat_invariance
             COMMAND ${AETHER_DOCS_PYTHON}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/reformat_invariance.py)
    set_tests_properties(aether_reformat_invariance PROPERTIES
        ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>"
        TIMEOUT 1200
        LABELS "invariance")
endif()

# ---- Backend conformance ------------------------------------------------------
# tests/backend_conformance/run.py: what Aether programs need from the shared
# engine (pscal-core, rea), each stdout fixture run uncached and then cold and
# warm on a private cache. Today's known engine defects are expected failures
# (manifest.txt `xfail:<item>`): they do not fail this test, but one that starts
# passing does, until its line flips to `pass` in the commit whose pin bump
# fixed it. A few seconds, so the default run includes it; tools/pin_gate.sh
# runs the same pack against candidate pins.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME aether_backend_conformance
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_conformance/run.py
                     --aether $<TARGET_FILE:aether>)
    set_tests_properties(aether_backend_conformance PROPERTIES
        TIMEOUT 300
        LABELS "conformance")
endif()

# ---- Backlog probe lap (W2-23) -------------------------------------------------
# tools/run_backlog.py: each tests/backlog/ probe pins TODAY's wrong behaviour
# of one open plan item, by outcome class (manifest.tsv). It fails when a
# probe's class changes, so the commit that fixes an item must also flip its
# probe, the ledger entry and the guide lines it names. About 2 s, so the
# default run includes it; `ctest -LE backlog` leaves it out during WIP.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME aether_backlog
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_backlog.py
                     --aether $<TARGET_FILE:aether>)
    set_tests_properties(aether_backlog PROPERTIES
        TIMEOUT 300
        LABELS "backlog")
endif()

# ---- Data-driven fixtures (W2-24) ----------------------------------------------
# tools/run_fixtures.py runs tests/fx/[<dir>/]<name>.aether against its sidecars
# (.out .unordered .err .codes .rc .in .flags .cap), keeps going past a failure
# and reports them all. Every new fixture goes here, not into tests/run.sh. One
# CTest per tests/fx subdirectory, labelled `fixtures` and the directory name
# (a new directory needs a re-configure); aether_fixtures_selftest mutates
# each sidecar class in a copy and fails when the runner misses one.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    file(GLOB _aether_fx_entries LIST_DIRECTORIES true CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx/*)
    file(GLOB _aether_fx_top CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/fx/*.aether)
    if(_aether_fx_top)
        add_test(NAME aether_fixtures
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_fixtures.py
                         --aether $<TARGET_FILE:aether> --dir .)
        set_tests_properties(aether_fixtures PROPERTIES TIMEOUT 600 LABELS "fixtures")
    endif()
    foreach(_fx_dir IN LISTS _aether_fx_entries)
        if(IS_DIRECTORY ${_fx_dir})
            get_filename_component(_fx_name ${_fx_dir} NAME)
            add_test(NAME aether_fixtures_${_fx_name}
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_fixtures.py
                             --aether $<TARGET_FILE:aether> --dir ${_fx_name})
            set_tests_properties(aether_fixtures_${_fx_name} PROPERTIES
                TIMEOUT 600 LABELS "fixtures;${_fx_name}")
        endif()
    endforeach()
    # tests/run.sh runs the fixtures too, for callers that exec it by path;
    # under CTest they already have their own tests.
    set_property(TEST aether_tests APPEND PROPERTY ENVIRONMENT "AETHER_RUN_SH_FIXTURES=0")
    add_test(NAME aether_fixtures_selftest
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/run_fixtures_selftest.sh)
    set_tests_properties(aether_fixtures_selftest PROPERTIES
        ENVIRONMENT "AETHER_BIN=$<TARGET_FILE:aether>"
        TIMEOUT 300
        LABELS "fixtures")
endif()
