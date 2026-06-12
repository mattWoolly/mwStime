// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Task 021 — mws::engine::TempoMap: pure, host-agnostic tempo-sync math.
// Every value asserted here traces to docs/design/dsp-engine.md §2
// (`tempoSync` / `fxWindow` rows), architecture.md §5.2 (SYNC window
// semantics + no-transport fallback), and plan/decisions/006-fx-vs-sample-mode.md.
//
// Test-case names begin with the tag word so `ctest -R tempomap` matches
// (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "mws/engine/Params.h"
#include "mws/engine/TempoMap.h"

using Catch::Matchers::WithinAbs;
using mws::engine::FxWindow;
using mws::engine::TempoMap;

// ---------------------------------------------------------------------------
// syncedTimeFactor — timeFactor := 100 × sourceBPM / hostBPM
// (dsp-engine.md §2 `tempoSync` row; direction panel-corrected)
// ---------------------------------------------------------------------------

TEST_CASE("tempomap: 174 BPM source into an 87 BPM host yields 200% (longer)",
          "[tempomap]")
{
    // The documented example: a 174 BPM loop into an 87 BPM host stretches
    // to 200% — longer (dsp-engine.md §2).
    REQUIRE_THAT(TempoMap::syncedTimeFactor(174.0, 87.0), WithinAbs(200.0, 1e-9));
}

TEST_CASE("tempomap: matched tempo yields 100% (no stretch)", "[tempomap]")
{
    REQUIRE_THAT(TempoMap::syncedTimeFactor(174.0, 174.0), WithinAbs(100.0, 1e-9));
}

TEST_CASE("tempomap: slow source into fast host yields 50% (compression)",
          "[tempomap]")
{
    // 87 -> 174 ⇒ 50%. FX clamping of T<100% is handled elsewhere (ADR-006).
    REQUIRE_THAT(TempoMap::syncedTimeFactor(87.0, 174.0), WithinAbs(50.0, 1e-9));
}

// ---------------------------------------------------------------------------
// quantizeClassic — CLASSIC coerces the synced factor to integer %
// ("Akai samplers don't use decimal values for Time Factor", AKZ §2.1)
// + achieved-length hint (the LCD shows the achieved value — ui-design §6.2)
// ---------------------------------------------------------------------------

TEST_CASE("tempomap: quantizeClassic rounds the synced factor to integer percent",
          "[tempomap]")
{
    REQUIRE(TempoMap::quantizeClassic(200.0) == 200);
    REQUIRE(TempoMap::quantizeClassic(133.4) == 133);
    REQUIRE(TempoMap::quantizeClassic(133.5) == 134);
    REQUIRE(TempoMap::quantizeClassic(99.49) == 99);
}

TEST_CASE("tempomap: achievedLengthSamples applies the quantized factor (LCD hint)",
          "[tempomap]")
{
    // The LCD shows the achieved (quantized-factor) length, never the
    // requested one. Engine's schedule-derived length (§3.4) is separate.
    REQUIRE(TempoMap::achievedLengthSamples(200, 48000) == 96000);
    REQUIRE(TempoMap::achievedLengthSamples(150, 1000) == 1500);
    REQUIRE(TempoMap::achievedLengthSamples(133, 1000) == 1330);
}

// ---------------------------------------------------------------------------
// windowBoundary — transport-aligned SYNC window math (architecture.md §5.2)
// ---------------------------------------------------------------------------

TEST_CASE("tempomap: at 120 BPM 4/4 a 1-bar window is exactly 2 s of host samples",
          "[tempomap]")
{
    const double hostRate = 48000.0;
    auto wb = TempoMap::windowBoundary(/*ppq*/ 0.0, /*bpm*/ 120.0,
                                       /*num*/ 4, /*den*/ 4,
                                       FxWindow::OneBar, hostRate);
    REQUIRE_THAT(wb.windowStartPpq, WithinAbs(0.0, 1e-9));
    // 1 bar @ 120 BPM 4/4 = 4 quarter notes = 2 s = 96000 samples @ 48 kHz.
    REQUIRE_THAT(wb.samplesToNextBoundary, WithinAbs(96000.0, 1e-6));
}

TEST_CASE("tempomap: boundaries land on bar lines for non-zero transport offsets",
          "[tempomap]")
{
    const double hostRate = 48000.0;
    // 4/4, 1-bar window ⇒ bar lines at ppq 0, 4, 8, ...
    auto wb = TempoMap::windowBoundary(/*ppq*/ 5.0, /*bpm*/ 120.0,
                                       4, 4, FxWindow::OneBar, hostRate);
    REQUIRE_THAT(wb.windowStartPpq, WithinAbs(4.0, 1e-9));
    // 3 quarter notes remain to ppq 8 = 1.5 s = 72000 samples.
    REQUIRE_THAT(wb.samplesToNextBoundary, WithinAbs(72000.0, 1e-6));
}

TEST_CASE("tempomap: at 120 BPM 3/4 a 1-bar window is exactly 1.5 s of host samples",
          "[tempomap]")
{
    const double hostRate = 48000.0;
    auto wb = TempoMap::windowBoundary(/*ppq*/ 0.0, /*bpm*/ 120.0,
                                       /*num*/ 3, /*den*/ 4,
                                       FxWindow::OneBar, hostRate);
    REQUIRE_THAT(wb.windowStartPpq, WithinAbs(0.0, 1e-9));
    // 1 bar @ 120 BPM 3/4 = 3 quarter notes = 1.5 s = 72000 samples.
    REQUIRE_THAT(wb.samplesToNextBoundary, WithinAbs(72000.0, 1e-6));
}

TEST_CASE("tempomap: fractional windows below one bar align to sub-bar grid",
          "[tempomap]")
{
    const double hostRate = 48000.0;
    // 4/4, 1/4-bar window = 1 quarter note. @120 BPM = 0.5 s = 24000 samples.
    auto wb = TempoMap::windowBoundary(0.0, 120.0, 4, 4, FxWindow::QuarterBar,
                                       hostRate);
    REQUIRE_THAT(wb.samplesToNextBoundary, WithinAbs(24000.0, 1e-6));

    // 4/4, 1/2-bar window = 2 quarter notes. @120 BPM = 1 s = 48000 samples.
    auto half = TempoMap::windowBoundary(0.0, 120.0, 4, 4, FxWindow::HalfBar,
                                         hostRate);
    REQUIRE_THAT(half.samplesToNextBoundary, WithinAbs(48000.0, 1e-6));
}

// ---------------------------------------------------------------------------
// windowFromWallClock — no-transport / Standalone fallback (architecture.md §5.2)
// ---------------------------------------------------------------------------

TEST_CASE("tempomap: no-transport fallback uses 120 BPM when no tempo was ever seen",
          "[tempomap]")
{
    const double hostRate = 48000.0;
    // lastKnownBpm <= 0 ⇒ never seen ⇒ fall back to 120 BPM (PI).
    // 1 bar (assumed 4/4) @ 120 BPM = 2 s = 96000 samples.
    REQUIRE_THAT(TempoMap::windowFromWallClock(0.0, hostRate, FxWindow::OneBar),
                 WithinAbs(96000.0, 1e-6));
}

TEST_CASE("tempomap: wall-clock fallback uses the last-known tempo when available",
          "[tempomap]")
{
    const double hostRate = 48000.0;
    // 1 bar (4/4) @ 60 BPM = 4 s = 192000 samples.
    REQUIRE_THAT(TempoMap::windowFromWallClock(60.0, hostRate, FxWindow::OneBar),
                 WithinAbs(192000.0, 1e-6));
}
