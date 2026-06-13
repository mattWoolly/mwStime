---
id: 026b
title: Hardware-capture sourcing + SpliceCal/D-TIME calibration (v1-freeze gate)
status: in-review
depends-on: [026]
component: qa
estimated-size: M
---

## Objective
Real hardware captures become the primary authenticity oracle: a sourcing plan and
capture kit for S950/S1100 renders of the golden corpus, calibration tooling that
fits `SpliceCal` (and validates the D-TIME mapping) against the captures, and
disjoint calibration/validation fixture sets. The D-TIME mapping is a **v1-freeze
gate** (testing-strategy.md §7 Wave 2); this is explicitly an *early* QA task — it
starts as soon as the golden corpus exists, not at the end of the backlog.

## Context
Read first:
- docs/design/testing-strategy.md §7 Wave 2 (hardware captures = primary oracle;
  first named target = the SPOD S1100 preset Cyclic/1000/300%/Q20/W10; disjoint
  calibration vs validation sets — no overfitting)
- docs/design/architecture.md §10 risk 1 (splice fine structure unknown; captures
  gate any "authentic" labeling)
- docs/design/dsp-engine.md §3.1 (`SpliceCal` — data-only recalibration), §5 (every
  (PI) S950 row is deviation-until-calibrated; D-TIME mapping)
- plan/decisions/001 (SpliceCal rationale; re-bless after calibration needs an ADR)
- Golden corpus + comparer (026)

## Scope
- `docs/qa/hardware-capture-plan.md`: sourcing plan (community/eBay S950 + S1100
  owners), the exact capture protocol — which `tests/golden/inputs/` WAVs, which
  settings per render (first target: the SPOD S1100 preset), sample-rate/transfer
  requirements, and the split into a **calibration set** and a **held-out
  validation set** (disjoint by input file AND by parameter point).
- `tools/calibrate/`: a comparison/fitting tool over mwstime-render output vs a
  capture directory — measures flutter rate (splice-comb frequency), stutter
  schedule, schedule-derived output length, and proposes `SpliceCal` values
  (overlapF/shape/rounding) plus a D-TIME↔cycle-length mapping check; reads
  arbitrary local capture dirs (captures are NOT committed unless rights are clear).
- Tool self-test: a ctest (`calibrate`) that runs the fitter on *synthetic*
  "captures" (renders made with a known, deliberately different SpliceCal) and
  recovers the planted parameters — proves the tooling before real captures arrive.
- Outcome wiring: if captures arrive and SpliceCal changes → calibration ADR +
  golden re-bless per the 026 procedure; if captures cannot be sourced by freeze →
  "authentic" labeling is downgraded in release notes (testing-strategy §7 exit
  criteria) — record the decision path in the plan doc.

## Out of scope
- Akaizer cross-check (task 026c — corroboration only, never calibration).
- Committing any third-party audio to the repo.
- The re-bless itself (separate PR + ADR per the 026 blessing policy).

## Acceptance criteria
- [ ] Capture plan doc complete: protocol, targets, disjoint cal/val sets defined.
- [ ] `tools/calibrate` recovers planted SpliceCal values from synthetic captures
      (ctest `calibrate` passes).
- [ ] D-TIME validation procedure documented as the v1-freeze gate it is.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R calibrate --no-tests=error
```
