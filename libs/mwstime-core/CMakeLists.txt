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

# Position-independent code: mwstime_core is a STATIC lib but it is linked INTO
# the JUCE plugin SHARED objects (LV2/VST3/CLAP .so on Linux). A shared object
# requires every object it pulls in to be PIC, so on Linux/ELF the non-PIC core
# objects (e.g. WavIo's <fstream>/<charconv> vtables) trip
# "relocation R_X86_64_PC32 ... recompile with -fPIC" at the LV2 .so link
# (GNU ld masks this as "final link failed: bad value"; lld reports it exactly).
# macOS/Mach-O does not require it, hence it only surfaced on the first Linux CI
# build. PIC has negligible cost and changes no numeric output, so it is safe for
# the deterministic core. (Fixed when wiring CI — task 051.)
set_target_properties(mwstime_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
