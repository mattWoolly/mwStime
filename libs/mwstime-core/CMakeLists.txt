# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# mwstime_core: PURE C++20 static library. Zero dependencies beyond the C++20
# standard library — no JUCE, no third-party includes, ever
# (docs/design/architecture.md §2, plan/decisions/001-dsp-engine-architecture.md).

add_library(mwstime_core STATIC
    src/core/AutoCorr.cpp
    src/core/Butterworth.cpp
    src/core/MovingLpf.cpp
    src/core/Quantizer.cpp
    src/core/Resampler.cpp
    src/core/Version.cpp
    src/core/WavIo.cpp
    src/model/CharacterChain.cpp
    src/model/FixedRateChain.cpp
    src/model/ModelSpec.cpp
    src/model/VarClockChain.cpp
    src/engine/OfflineRenderer.cpp
    src/engine/RealtimeStretcher.cpp
    src/engine/TempoMap.cpp
    src/engine/Transpose.cpp
    src/stretch/AutoCycle.cpp
    src/stretch/CyclicEngine.cpp
    src/stretch/RepitchEngine.cpp
    src/stretch/S950Engine.cpp
)

target_include_directories(mwstime_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(mwstime_core PUBLIC cxx_std_20)

# FP discipline (architecture.md §7): pinned double rounding for deterministic
# golden renders. No -ffast-math anywhere; FMA contraction disabled on this
# target. On Clang/GCC that is -ffp-contract=off; on MSVC the equivalent is
# /fp:precise (the default, but pinned EXPLICITLY here per task 050 so the
# integer-CLASSIC bit-exactness on a third compiler does not silently depend on
# a toolchain default — /fp:precise disables contraction and forbids the
# /fp:fast reorderings, validating the fixed-point discipline on MSVC too).
target_compile_options(mwstime_core PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>
)
