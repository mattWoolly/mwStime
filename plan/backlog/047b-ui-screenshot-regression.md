---
id: 047b
title: UI golden screenshot regression harness — macOS-arm64 gate, scale checks
status: in-review
depends-on: [046, 047]
component: qa
estimated-size: M
---

## Objective
The deferred UI regression harness exists: software-renderer golden screenshots of
every model faceplate and key LCD pages, gating bit-exactly on macOS arm64 (the
reference platform), with a tolerance compare for other platforms and 0.6×/2.0×
scale checks.

## Context
Read first:
- docs/design/ui-design.md §4 (screenshot determinism: software renderer, ONE
  reference platform; macOS-arm64-only gates, Linux tolerance compare)
- plan/decisions/005-ui-rendering-approach.md (screenshot-gate ruling)
- docs/design/testing-strategy.md §7 Wave 3 (UI at 0.6×/2.0× scale; golden
  screenshots macOS arm64 only)
- Faceplate/model switching (046 — all four faceplates render), resize (047 —
  scale machinery), task 026 (the blessing-procedure pattern to mirror)

## Scope
- `tests/ui/screenshot/`: a headless harness that constructs the editor offscreen
  with the JUCE software renderer, drives it to a defined set of states — each of
  the four model faceplates at 1.0 scale (S1000 TIME page content fixed/fake data
  for determinism), the S950 2-line page, the FX REALTIME page, plus the S1000
  faceplate at 0.6× and 2.0× — and writes PNGs.
- Comparer: byte/pixel-exact on macOS arm64 against `tests/ui/blessed/`; tolerance
  mode (per-pixel delta + percentage-different threshold) keyed by platform for
  Linux/Windows (cross-platform AA is not deterministic — ADR-005).
- Bless target `bless_ui_goldens` mirroring the 026 procedure (BLESS_REASON
  required, MANIFEST with date/blesser/reason); blessed PNGs committed (small,
  plain git).
- CTest label `ui-golden`; on non-reference platforms the exact gate downgrades to
  the tolerance compare automatically (never silently skips on the reference
  platform).
- Determinism guards: fixed fake data into LcdPageModel, animations/blink disabled
  for capture, fixed scale factors.

## Out of scope
- Per-host window-embedding screenshots (host smoke, 048b).
- Screenshot CI wiring (051 adds the macOS job step using this label).

## Acceptance criteria
- [ ] All defined states render and match the blessed set on macOS arm64
      (pixel-exact, software renderer).
- [ ] 0.6× and 2.0× captures present and gated.
- [ ] Bless target refuses without BLESS_REASON; re-bless on the same commit leaves
      PNGs byte-identical (MANIFEST excluded, same rule as 026).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -L ui-golden --no-tests=error
BLESS_REASON="dry-run check" cmake --build --preset default --target bless_ui_goldens
git status --short -- tests/ui/blessed/ ':(exclude)tests/ui/blessed/MANIFEST.json'   # must be empty
```
