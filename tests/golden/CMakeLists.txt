# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# Golden-render character regression (docs/design/testing-strategy.md §4;
# task plan/backlog/026-golden-harness.md). cases.json is the single source of
# truth: this file parses it (CMake string(JSON)) and registers ONE CTest per
# case, each invoking mwstime-render --case <id> then golden_compare against the
# blessed render. Every case carries:
#   - CTest LABEL `golden`            (so `ctest -L golden` selects the suite),
#   - a test name `golden_<id>`       (the <model>_<case> ids already start with
#     the model, so the names read `golden_s1100_jungle_amen_300`, satisfying the
#     `ctest -R golden` name selector too — silent-pass rule, backlog/README.md).
#
# It also defines the gated `bless_goldens` target (tools/bless_goldens.sh) and a
# `goldeninputs` CTest that regenerates the committed input corpus into a scratch
# dir and asserts it is byte-identical (the inputs are reproducible).

set(GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(GOLDEN_INPUTS_DIR "${GOLDEN_DIR}/inputs")
set(GOLDEN_BLESSED_DIR "${GOLDEN_DIR}/blessed")
set(GOLDEN_CASES_JSON "${GOLDEN_DIR}/cases.json")
set(GOLDEN_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/golden_scratch")
file(MAKE_DIRECTORY "${GOLDEN_SCRATCH}")

# Reconfigure whenever cases.json changes (it drives the test list).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GOLDEN_CASES_JSON}")

# The render + compare binaries (built by tools/, link only mwstime_core).
set(RENDER_BIN "$<TARGET_FILE:mwstime_render>")
set(COMPARE_BIN "$<TARGET_FILE:golden_compare>")

# The per-case runner is a `cmake -P` script (cross-platform: no bash needed, so
# the golden suite runs identically on macOS/Linux/Windows-MSVC — task 050). The
# legacy tools/run_golden_case.sh wrapper is retained for manual shell use.
set(RUN_CASE_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/RunGoldenCase.cmake")
set(BLESS_SH "${CMAKE_SOURCE_DIR}/tools/bless_goldens.sh")

# --- parse cases.json -------------------------------------------------------
file(READ "${GOLDEN_CASES_JSON}" GOLDEN_CASES_TEXT)
string(JSON GOLDEN_NUM_CASES LENGTH "${GOLDEN_CASES_TEXT}" "cases")
if(GOLDEN_NUM_CASES EQUAL 0)
    message(FATAL_ERROR "tests/golden/cases.json has no cases")
endif()

# Accumulate "<id> <policy>" lines for the bless script's case list.
set(GOLDEN_CASE_LIST_LINES "")
math(EXPR GOLDEN_LAST_INDEX "${GOLDEN_NUM_CASES} - 1")

# --- platform-keyed tolerance (testing-strategy.md §4; task 049) ------------
# Goldens are blessed bit-exact on the REFERENCE platform (macOS arm64). The
# comparison policy is scoped per case AND per platform:
#   * CLASSIC stretch-only cases (policy "exact" in cases.json) are integer-only
#     by construction (dsp-engine.md §3.2) with FP discipline pinned on the core,
#     so they are bit-exact on EVERY platform — the tol arg below is ignored for
#     them (golden_compare --policy exact gates at max abs diff == 0).
#   * float-stage cases (policy "tolerance": filters/SRC/transpose/REVISED) are
#     bit-exact ONLY on the reference platform. There we keep the strict tol 0
#     (a tighter-than-1e-6 reference check that catches any drift early);
#     off-reference (Linux/Windows) we relax to max abs <= 1e-6 per the policy.
# Keyed on the reference platform, not the host OS string, so a future macOS x86
# bless or a different reference would only need this single switch updated.
if(APPLE)
    set(GOLDEN_TOL "0")        # reference platform: float cases must be bit-exact too
else()
    set(GOLDEN_TOL "1e-6")     # off-reference: tolerance compare (testing-strategy.md §4)
endif()
message(STATUS "golden harness: float-stage tolerance = ${GOLDEN_TOL} "
               "(reference platform = bit-exact)")

foreach(i RANGE 0 ${GOLDEN_LAST_INDEX})
    string(JSON CASE_OBJ GET "${GOLDEN_CASES_TEXT}" "cases" ${i})
    string(JSON CASE_ID GET "${CASE_OBJ}" "id")

    # policy is optional in the schema; default to tolerance (the safe gate).
    string(JSON CASE_POLICY ERROR_VARIABLE _policy_err GET "${CASE_OBJ}" "policy")
    if(_policy_err OR CASE_POLICY STREQUAL "")
        set(CASE_POLICY "tolerance")
    endif()

    string(APPEND GOLDEN_CASE_LIST_LINES "${CASE_ID} ${CASE_POLICY}\n")

    # One CTest per case, run via `cmake -P` (cross-platform — task 050). The
    # reference platform blesses bit-exact, so the tolerance gate is run with
    # tol 0 there (a stricter-than-1e-6 reference check); off-reference platforms
    # relax to GOLDEN_TOL (testing-strategy.md §4). `exact`-policy cases ignore
    # the tol arg entirely (bit-exact gate on every platform).
    add_test(
        NAME "golden_${CASE_ID}"
        COMMAND "${CMAKE_COMMAND}"
                -D "RENDER_BIN=${RENDER_BIN}"
                -D "COMPARE_BIN=${COMPARE_BIN}"
                -D "CASES_JSON=${GOLDEN_CASES_JSON}"
                -D "INPUTS_DIR=${GOLDEN_INPUTS_DIR}"
                -D "BLESSED_DIR=${GOLDEN_BLESSED_DIR}"
                -D "SCRATCH_DIR=${GOLDEN_SCRATCH}"
                -D "CASE_ID=${CASE_ID}"
                -D "POLICY=${CASE_POLICY}"
                -D "TOL=${GOLDEN_TOL}"
                -P "${RUN_CASE_CMAKE}"
    )
    set_tests_properties("golden_${CASE_ID}" PROPERTIES LABELS golden)
endforeach()

message(STATUS "golden harness: registered ${GOLDEN_NUM_CASES} cases (label `golden`)")

# The bless script's case list (id + policy), regenerated at configure time.
set(GOLDEN_CASE_LIST "${CMAKE_CURRENT_BINARY_DIR}/golden_cases.txt")
file(WRITE "${GOLDEN_CASE_LIST}" "${GOLDEN_CASE_LIST_LINES}")

# --- bless target -----------------------------------------------------------
# `cmake --build --preset default --target bless_goldens` re-renders every case
# into tests/golden/blessed/ and writes MANIFEST.json. The script REFUSES unless
# BLESS_REASON is set (the gate). The engine version + hash come from the build
# so the manifest records exactly what produced the blessed set.
add_custom_target(bless_goldens
    COMMAND "${CMAKE_COMMAND}" -E echo
            "bless_goldens: invoking tools/bless_goldens.sh (BLESS_REASON gate enforced by the script)"
    COMMAND "${BLESS_SH}"
            "${RENDER_BIN}" "${GOLDEN_CASES_JSON}" "${GOLDEN_CASE_LIST}"
            "${GOLDEN_INPUTS_DIR}" "${GOLDEN_BLESSED_DIR}"
    DEPENDS mwstime_render
    VERBATIM
    USES_TERMINAL
)

# --- input-corpus reproducibility test --------------------------------------
# Regenerate the corpus into a scratch dir and diff each committed input against
# it: the generator must reproduce every WAV byte-for-byte (acceptance criterion).
set(GOLDEN_INPUTS_SCRATCH "${CMAKE_CURRENT_BINARY_DIR}/inputs_scratch")
add_test(
    NAME "goldeninputs: corpus regenerates byte-identical"
    COMMAND "${CMAKE_COMMAND}"
            -D "GEN_BIN=$<TARGET_FILE:gen_golden_inputs>"
            -D "COMMITTED_DIR=${GOLDEN_INPUTS_DIR}"
            -D "SCRATCH_DIR=${GOLDEN_INPUTS_SCRATCH}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckInputsReproducible.cmake"
)
set_tests_properties("goldeninputs: corpus regenerates byte-identical"
    PROPERTIES LABELS golden)
