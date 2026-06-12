# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# mwstime_core: PURE C++20 static library. Zero dependencies beyond the C++20
# standard library — no JUCE, no third-party includes, ever
# (docs/design/architecture.md §2, plan/decisions/001-dsp-engine-architecture.md).

add_library(mwstime_core STATIC
    src/core/Version.cpp
    src/model/ModelSpec.cpp
)

target_include_directories(mwstime_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(mwstime_core PUBLIC cxx_std_20)

# FP discipline (architecture.md §7): pinned double rounding for deterministic
# golden renders. No -ffast-math anywhere; FMA contraction disabled on this
# target. MSVC's default /fp:precise already disables contraction.
target_compile_options(mwstime_core PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>
)
