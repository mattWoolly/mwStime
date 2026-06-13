---
id: 041
title: LcdPageModel — headless page content, hardware units, clamp feedback, visibility
status: done
depends-on: [010, 028]
component: ui
estimated-size: M
---

## Objective
`LcdPageModel`: the pure-C++, GUI-free single source of truth for everything the LCD
shows — per-model page text mirroring the manuals' printed screens, hardware-unit
formatting, engine-clamp feedback strings, and per-model field visibility. Fully
unit-testable without a window.

## Context
Read first:
- docs/design/ui-design.md §2 (LcdPageModel role + the mapping rules paragraph), §1
  (page content in the mockup), §3 (ParamVisibility, showsModeRow/showsQualWidth)
- docs/design/dsp-engine.md §2 (hardware units per row — the LCD shows the CLAMPED
  hardware value), §5 (S950 page-14 layout source), §6 (S900 "NO TIMESTRETCH —
  VARISPEED" notice + semitone readout), §3.2 (achieved length display honesty)
- docs/design/testing-strategy.md §2 LcdPageModel bullet (required tests)
- Parameters (028), ModelSpec clamp (009 via 028),
  `CyclicEngine::expectedOutputLength` (010 — the achieved-length readout source)

## Scope
- `plugin/ui/LcdPageModel.{h,cpp}` (no juce::Component includes; juce::String OK or
  std::string preferred):
  - input: ParamSnapshot + ModelSpec + sample/render info (name, lengths, mem%,
    achieved length/ratio, monoSummed, sync readout, FX clamp flag, progress,
    load-error code, embed/path-only persistence flag) — plain structs (the
    FileLoader error enum and 032 embed flag are mirrored as plain input fields so
    this file stays headless and JUCE-free),
  - output: a `Page` = rows of `{text, perCellStyle}` for the active model/page:
    S1000/S1100 multi-line TIME-STRETCH page; S950 2-line PAGE 14 STRETCH; S900
    varispeed page with the ADR-003 notice; FX mode "TIME-STRETCH (REALTIME)" page
    (ui-design §6.4),
  - formatting: clamped hardware values (`999%` cap on S950, `FX MIN 100%` in FX
    FREE), CLASSIC achieved (schedule-quantized) new-length readout — never the
    requested one (ui-design §6.2), `qual/width` greyed "INTELL only" at v1,
    mono-sum notice on S900/S950, `** NOT ENOUGH MEMORY **`, progress/remaining-time
    line, sync readout `174.0 -> 87.0 = 200%`,
    file-load errors as hardware-idiom LCD message lines from the typed error code
    (e.g. `** WRONG DISK **`-flavored wording **(PI)** — ui-design §6.1 step 3; the
    code→message map lives here), and an embed-status line when the sample exceeds
    the 16 MB cap and persists path-only (task 032 flag surfaced to the user),
  - field map: which cells are editable fields, their order (cursor navigation), and
    each field's parameter binding (consumed by 045).
- Tests (`tests/plugin/test_lcdpagemodel.cpp`, headless):
  - formatting: timeFactor 300 → `300%`, cycle 1000 → `1000`, transpose −12 →
    `-12.00`,
  - S950 model + timeFactor 1500 ⇒ page shows `999%`,
  - FX FREE T=80 ⇒ `FX MIN 100%` present,
  - per-model visibility: S900 hides cycle/stretch-mode fields; S950 hides
    mode row; S1000 greys qual/width,
  - achieved-length honesty: CLASSIC readout equals the schedule-derived length from
    the engine helper (010 `expectedOutputLength`), not round(N·T),
  - unsupported-format error code ⇒ the hardware-idiom message line is present,
  - over-cap/path-only flag ⇒ the embed-status line is present.

## Out of scope
- Rendering (040), editing/navigation behavior (045).

## Acceptance criteria
- [ ] All testing-strategy §2 LcdPageModel assertions implemented and passing.
- [ ] File includes no GUI headers (headless rule).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R lcdpagemodel --no-tests=error
```
