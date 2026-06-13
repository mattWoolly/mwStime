# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# golden_compare: the two-stage golden-render comparer with diagnostics
# (docs/design/testing-strategy.md §4 "Runner"; task
# plan/backlog/026-golden-harness.md). JUCE-free; links only mwstime_core
# (WavIo). Invoked per case by the CTest golden runner after mwstime-render
# produces a candidate WAV.

# Same FP discipline as the core (architecture.md §7) — the comparer's gate
# arithmetic and FFT diagnostics must not contract/fast-math.
set(MWSTIME_GOLDENCMP_FP_OPTS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>)

add_executable(golden_compare main.cpp)
target_link_libraries(golden_compare PRIVATE mwstime_core)
target_compile_features(golden_compare PRIVATE cxx_std_20)
target_compile_options(golden_compare PRIVATE ${MWSTIME_GOLDENCMP_FP_OPTS})
