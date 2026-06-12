---
id: 026
title: Golden-render harness — inputs, cases.json, comparer, bless target, blessed goldens
status: todo
depends-on: [025]
component: qa
estimated-size: M
---

## Objective
The character-regression system: committed synthetic input WAVs, the case matrix, a
two-stage comparer with diagnostics, the gated blessing procedure, and the initial
blessed goldens rendered on macOS arm64 (the reference platform).

## Context
Read first:
- docs/design/testing-strategy.md §4 (the entire section — inputs list, case matrix,
  comparison policy, runner, blessing procedure; this is the spec)
- docs/design/dsp-engine.md §9 (the four preset cases that double as golden cases)
- docs/design/architecture.md §2 (bit-exactness scope: integer CLASSIC path only
  cross-platform; float stages reference-platform exact)
- mwstime-render CLI (task 025)

LEGAL RULE: all inputs are generated/original — NO copyrighted audio (no Amen break).

## Scope
- `tests/golden/inputs/`: committed WAVs ≤ 2 s, 44.1 kHz 16-bit mono unless noted —
  `sine440.wav`, `saw100.wav`, `clicktrain_2hz.wav`, `noiseburst.wav`,
  `sweep20-20k.wav`, `breakslice.wav` (synthesized original 1-bar drum loop —
  generate from synthesis primitives, deterministic seed), `breakslice_22k.wav`,
  `stereo_pan.wav` — plus the deterministic generator
  (`tools/gen_golden_inputs/` or a script) committed so inputs are reproducible.
- `tests/golden/cases.json`: per testing-strategy §4 — the dsp-engine §9 presets
  (incl. S1100 Cycle 1000 / Time 300%), extremes (25%, 999%/2000%, cycle 20/2000),
  CLASSIC + REVISED, transpose ±12, character ON/OFF, norm OFF/ON (~60 cases),
  output names `<model>_<case>.wav`.
- Comparer (`tools/golden_compare/` or a test executable):
  1. exact compare for CLASSIC stretch-only (transpose 0, character OFF) cases;
     tolerance (max abs ≤ 1e-6) for float-stage cases off-reference,
  2. on mismatch print: max abs diff, RMS diff, first divergent sample, splice-comb
     peak location/level, 1/3-octave spectral-difference table.
- CTest registration: one test per case invoking `mwstime-render --case` + comparer;
  every case carries CTest **LABEL `golden`** and a test name prefixed
  `golden_<model>_<case>` (so both `-L golden` and `-R golden` selectors match —
  silent-pass rule in plan/backlog/README.md).
- Bless target: `cmake --build --preset default --target bless_goldens` wrapping
  `tools/bless_goldens.sh`; refuses without `BLESS_REASON="..."`; writes
  `tests/golden/blessed/MANIFEST.json` (engine version hash, date, blesser, reason).
- Initial blessing: run on macOS arm64, commit `tests/golden/blessed/` (each < 1 MB,
  plain git, no LFS) with reason "initial blessing, task 026".

## Out of scope
- Akaizer cross-check renders (QA phase Wave 2 — local-only, never committed/CI).
- Hardware-capture calibration (QA phase).
- Plugin-level tests.

## Acceptance criteria
- [ ] All inputs committed and regenerable by the committed generator (byte-identical).
- [ ] ~60 cases registered in CTest under label/tag `golden`; all pass against the
      blessed set on macOS arm64.
- [ ] Bless target refuses without BLESS_REASON; MANIFEST.json present and accurate.
- [ ] Comparer diagnostics print on a deliberately corrupted render (demonstrate in
      PR description).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -L golden --no-tests=error
BLESS_REASON="dry-run check" cmake --build --preset default --target bless_goldens
git status --short -- tests/golden/blessed/ ':(exclude)tests/golden/blessed/MANIFEST.json'
#   ^ must print nothing: the WAVs must be byte-identical after a same-commit
#     re-bless. MANIFEST.json is excluded — re-blessing rewrites its date/blesser/
#     reason fields by design, so it is expected to differ here.
```
