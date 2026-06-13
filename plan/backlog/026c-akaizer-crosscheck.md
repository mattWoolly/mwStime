---
id: 026c
title: Akaizer secondary cross-check — local-only corroboration procedure (never CI)
status: in-review
depends-on: [026]
component: qa
estimated-size: S
---

## Objective
The documented, local-only Akaizer corroboration run: a script + procedure that
compares mwstime-render output against locally produced Akaizer CLASSIC renders of
the corpus — peak-normalizing both sides, restricted to T 120–2000%, comparing
flutter rate / stutter schedule / schedule-derived length analytically. Explicitly a
*secondary* cross-check (Akaizer's fidelity claim was refuted 0-3) and explicitly
excluded from CI.

## Context
Read first:
- docs/design/testing-strategy.md §7 Wave 2 (the whole Akaizer-demotion paragraph —
  this IS the spec: closed payware, cannot be committed or run in CI, certified
  window T 120–2000% only, no oracle for compression or S950 D-TIME), §8 (Akaizer
  cross-check explicitly excluded from CI)
- docs/design/dsp-engine.md §7.3 (any Akaizer comparison must peak-normalize both
  sides — Akaizer normalizes since v1.3)
- docs/research/akaizer-analysis.md §1/§2.3 (research-cache is scratch, not
  committed)
- Golden corpus + comparer diagnostics (026)

## Scope
- `tools/akaizer_crosscheck/` (or a script): given a local directory of Akaizer
  renders of the golden inputs, peak-normalize both sides, filter cases to
  T 120–2000% (skip everything else with an explicit "no Akaizer oracle" note),
  then compare per case: splice-comb/flutter frequency, stutter schedule, and
  schedule-derived output length — analytic comparisons, NOT a null test
  ("not expected to null; deviations documented").
- `docs/qa/akaizer-crosscheck.md`: how a QA agent with a locally licensed Akaizer
  copy produces the renders (research-cache/ scratch dir, never committed), runs
  the script, and where deviation reports go.
- NOT registered in the default ctest preset and never referenced from CI (the 051
  task already forbids it — keep it that way); a self-test of the comparison math
  on synthetic data MAY be a ctest (`akzcheck`) since it needs no Akaizer binary.
- Output: a deviation report format (per-case table) the QA fleet can attach to
  issues.

## Out of scope
- Treating Akaizer as a calibration target (hardware captures are the oracle —
  task 026b; ADR-001 consequence).
- Committing Akaizer binaries, renders, or any research-cache content.

## Acceptance criteria
- [ ] Script + procedure doc exist; comparison math self-test passes without any
      Akaizer binary present.
- [ ] Nothing Akaizer-related is registered in the default ctest preset or
      reachable from CI config.
- [ ] Procedure enforces peak-normalization of both sides and the T 120–2000%
      window.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R akzcheck --no-tests=error
grep -ri akaizer .github/ tests/CMakeLists.txt 2>/dev/null; test $? -ne 0  # no CI/test-preset references
```
