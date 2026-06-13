# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# gen_golden_inputs: the DETERMINISTIC generator for the committed golden input
# corpus (tests/golden/inputs/) — task plan/backlog/026-golden-harness.md.
# Links ONLY mwstime_core (WavIo + Buffer); no JUCE (architecture.md §2).
#
# It is committed so the inputs are reproducible byte-for-byte (acceptance
# criterion). A CTest ("goldeninputs: ...") regenerates the corpus into a scratch
# dir and asserts every committed WAV is byte-identical — so an accidental edit to
# the synthesis is caught.

# Same FP discipline as the core / render CLI (architecture.md §7): the generator
# writes the committed goldens-corpus, so it must not contract/fast-math either.
set(MWSTIME_GENINPUTS_FP_OPTS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>)

add_executable(gen_golden_inputs main.cpp)
target_link_libraries(gen_golden_inputs PRIVATE mwstime_core)
target_compile_features(gen_golden_inputs PRIVATE cxx_std_20)
target_compile_options(gen_golden_inputs PRIVATE ${MWSTIME_GENINPUTS_FP_OPTS})
