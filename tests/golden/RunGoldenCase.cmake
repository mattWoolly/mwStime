# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# RunGoldenCase.cmake — one golden-render regression case, run via `cmake -P`
# (testing-strategy.md §4 runner; tasks 026/049/050). This is the CROSS-PLATFORM
# replacement for tools/run_golden_case.sh: `cmake` is present on every platform,
# so the golden CTests run identically on macOS, Linux and Windows (MSVC) without
# needing bash. It renders a single case with mwstime-render into a private
# scratch file, then gates it against the blessed render with golden_compare.
#
# Required -D vars (all set by tests/golden/CMakeLists.txt at add_test() time):
#   RENDER_BIN   path to mwstime_render
#   COMPARE_BIN  path to golden_compare
#   CASES_JSON   path to tests/golden/cases.json
#   INPUTS_DIR   path to tests/golden/inputs
#   BLESSED_DIR  path to tests/golden/blessed
#   SCRATCH_DIR  scratch dir for the candidate render
#   CASE_ID      the cases.json case id (== blessed file stem)
#   POLICY       exact | tolerance  (selects the golden_compare gate)
#   TOL          per-sample tolerance for the tolerance policy (0 on the
#                reference platform; 1e-6 off-reference — testing-strategy.md §4)
#
# Exit 0 = case matches the blessed render within policy; FATAL_ERROR otherwise
# (the render-refusal / mismatch diagnostics printed by the sub-tools land in the
# CTest --output-on-failure log without DAW access). FATAL_ERROR makes `cmake -P`
# exit nonzero, which CTest reads as a failed test.

foreach(_v RENDER_BIN COMPARE_BIN CASES_JSON INPUTS_DIR BLESSED_DIR SCRATCH_DIR CASE_ID POLICY)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "RunGoldenCase: required -D ${_v}=... is missing")
    endif()
endforeach()
if(NOT DEFINED TOL OR "${TOL}" STREQUAL "")
    set(TOL "1e-6")
endif()

file(MAKE_DIRECTORY "${SCRATCH_DIR}")
set(candidate "${SCRATCH_DIR}/${CASE_ID}.wav")
set(blessed "${BLESSED_DIR}/${CASE_ID}.wav")

if(NOT EXISTS "${blessed}")
    message(FATAL_ERROR
        "RunGoldenCase: NO BLESSED RENDER for '${CASE_ID}' (${blessed}).\n"
        "  Bless the goldens first (reference platform only):\n"
        "    BLESS_REASON=\"...\" cmake --build --preset default --target bless_goldens")
endif()

# 1) Render the candidate (bit depth comes from the case's declared bitDepth).
execute_process(
    COMMAND "${RENDER_BIN}" --case "${CASE_ID}" --cases "${CASES_JSON}"
            --inputs-dir "${INPUTS_DIR}" --out "${candidate}"
    RESULT_VARIABLE render_rc
    OUTPUT_VARIABLE render_out
    ERROR_VARIABLE  render_err
)
if(NOT render_rc EQUAL 0)
    message(FATAL_ERROR
        "RunGoldenCase: mwstime_render failed for '${CASE_ID}' (rc=${render_rc}):\n"
        "${render_out}\n${render_err}")
endif()

# 2) Gate against the blessed render with the case's comparison policy. The
#    sub-tool prints its own diagnostics; forward them so they land in the log.
#    NB: quote ${POLICY} — bare `POLICY` is a reserved first keyword in if()
#    (if(POLICY <name>)), which would mis-parse the comparison.
if("${POLICY}" STREQUAL "exact")
    execute_process(
        COMMAND "${COMPARE_BIN}" --candidate "${candidate}" --blessed "${blessed}"
                --policy exact
        RESULT_VARIABLE cmp_rc
        OUTPUT_VARIABLE cmp_out
        ERROR_VARIABLE  cmp_err
    )
else()
    execute_process(
        COMMAND "${COMPARE_BIN}" --candidate "${candidate}" --blessed "${blessed}"
                --policy tolerance --tol "${TOL}"
        RESULT_VARIABLE cmp_rc
        OUTPUT_VARIABLE cmp_out
        ERROR_VARIABLE  cmp_err
    )
endif()

# Always surface the comparer's output (it prints the max/RMS diff, first
# divergent sample, splice-comb peak and the 1/3-octave table on mismatch).
if(NOT cmp_out STREQUAL "")
    message(STATUS "${cmp_out}")
endif()
if(NOT cmp_err STREQUAL "")
    message(STATUS "${cmp_err}")
endif()

if(NOT cmp_rc EQUAL 0)
    message(FATAL_ERROR
        "RunGoldenCase: golden mismatch for '${CASE_ID}' (policy ${POLICY}, tol ${TOL}, rc=${cmp_rc}).")
endif()
