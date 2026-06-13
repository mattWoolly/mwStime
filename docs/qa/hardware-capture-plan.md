<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly. -->

# Hardware-capture sourcing + SpliceCal / D-TIME calibration plan

Status: accepted (QA), 2026-06-13. Implements task plan/backlog/026b. This is an
**early** QA task: it starts as soon as the golden corpus exists (task 026), not
at the end of the backlog, because the D-TIME mapping is a **v1-freeze gate**
(docs/design/testing-strategy.md §7 Wave 2; docs/design/dsp-engine.md §5).

Citations:
- [TS] docs/design/testing-strategy.md §7 (QA fleet; Wave 2 authenticity audit)
- [ARCH] docs/design/architecture.md §10 risk 1 (splice fine structure is the sound)
- [DSP] docs/design/dsp-engine.md §3.1 (`SpliceCal`), §5 (S950 / D-TIME mapping)
- [ADR1] plan/decisions/001-dsp-engine-architecture.md (SpliceCal rationale)
- [AKZ] docs/research/akaizer-analysis.md ; [DRR] docs/research/deep-research-report.md

---

## 0. Why captures, and why now

The splice fine structure — the crossfade shape, the overlap fraction `F`, and the
exact hop rounding — "is not recoverable without disassembly" and **it IS the
sound** ([ARCH] risk 1; [AKZ] §2.4). Every one of those constants is a (PI)
pragmatic invention today, centralized in `CyclicEngine::SpliceCal`
(`overlapF` / `shape` / `rounding`, [DSP] §3.1) and in the S950 `mapDTimeToCycleLen`
function ([DSP] §5). Until real hardware confirms them:

- the engine's "authentic" labeling is **provisional** ([TS] §7 exit criteria), and
- the S950 D-TIME mapping is a **v1-freeze gate** ([DSP] §5; [TS] §7 Wave 2) — v1
  does not freeze with an un-validated D-TIME claim labeled "authentic".

Hardware captures are the **primary oracle**. Akaizer is corroboration only and
never a calibration target (its "near-exact" fidelity was refuted 0-3 —
[DRR] Finding 6; task 026c). This plan defines how we source captures, exactly
which renders to take, how the calibration set is kept disjoint from the held-out
validation set (no overfitting), and the two decision paths out of the gate.

---

## 1. Sourcing plan

We do not own an S950 or S1100. Captures come from the community.

| Source | What to ask for | Notes |
|---|---|---|
| eBay / Reverb S950 + S1100 owners | a short paid capture session (render our supplied corpus) | the seller keeps the unit; we get WAVs + a rights release |
| Vintage-sampler forums (Akai S-series, gearspace, KVR, /r/synthesizers) | volunteer captures from owners with working units + a clean digital out path | community goodwill; credit in release notes |
| Studios with a maintained S950/S1100 | a booked capture session | most reliable transfer chain (clocked digital out) |
| The SPOD S1100 jungle setting [DRR F10] owner community | the first named target (see §2) | culturally the most-requested setting |

**Rights (hard rule, [TS] preamble; task scope "out of scope"):** every capture
arrives with an explicit written release allowing us to (a) analyze it and (b)
ship derived calibration constants. Captures themselves are **NEVER committed to
the repo** (no third-party audio in a public AGPL repo). They live in a local,
git-ignored directory that the calibration tool reads by path (`--captures <dir>`).
If a contributor allows redistribution we may host the *analysis report*, never the
audio.

**Transfer / capture chain requirements:**
- Digital out where available (S1100 has digital I/O); otherwise a clean,
  documented A/D at 44.1 kHz, 24-bit, with the converter and gain noted.
- No host-side normalization, no dither added on transfer, no plug-ins.
- The unit's own sample rate is the model rate (S1000/S1100 fixed 44.1/22.05 kHz;
  S950 variable clock — record the BANDWIDTH/clock the unit was set to, [DSP] §8.1).
- One WAV per render, named `<caseId>.wav` to match the manifest (§3).
- A capture log: model, OS version, every front-panel setting, serial if known,
  the transfer chain, and the contributor + rights release.

---

## 2. Capture protocol — exactly what to render

The owner loads each supplied input (our committed, original synthetic corpus —
`tests/golden/inputs/`, all ≤ 2 s, [TS] §4) and renders it at the settings below.
The inputs are legal to share (we generated them; gen_golden_inputs). The
**first named target** is the SPOD S1100 jungle preset
(Cyclic / cycle 1000 / 300% / Q20 / W10) [DRR F10] — capture it first; it both
validates CYCLIC and is the most-requested sound.

Inputs to supply (subset of `tests/golden/inputs/`):
- `sine440.wav` — steady tone: the splice-comb / flutter signature is cleanest here.
- `clicktrain_2hz.wav` — transients: the **stutter schedule** (grain-replay
  spacing) is directly visible.
- `breakslice.wav` — the original synthetic 1-bar loop: the musical reference.
- `saw100.wav` — pitched, harmonically rich: D-TIME "metallic vs tremolo" audition.

Per-model render settings (front-panel values; the tool's manifest mirrors them):

| Capture caseId | Model | Setting | Why |
|---|---|---|---|
| `s1100_jungle_amen_300` | S1100 | CYCLIC, cycle 1000, 300%, Q20/W10, breakslice | first named target [DRR F10] |
| `s1100_sine_300` | S1100 | CYCLIC, cycle 1000, 300%, sine440 | clean comb for `overlapF` fit |
| `s1100_sine_cycle500_200` | S1100 | CYCLIC, cycle 500, 200%, sine440 | second `(C,T)` point for the fit |
| `s1100_click_300` | S1100 | CYCLIC, cycle 1000, 300%, clicktrain | stutter schedule (hop_out / hop_in) |
| `s950_sine_dtime1000_200` | S950 | 200%, D-TIME 1000, POL2, sine440 | **D-TIME mapping gate** point A |
| `s950_sine_dtime500_200` | S950 | 200%, D-TIME 500, POL2, sine440 | **D-TIME mapping gate** point B |
| `s950_sine_dtime2000_200` | S950 | 200%, D-TIME 2000, POL2, sine440 | **D-TIME mapping gate** point C |
| `s950_click_dtime1000_300` | S950 | 300%, D-TIME 1000, POL2, clicktrain | S950 stutter schedule |

Set CHARACTER as the hardware naturally produces it (the unit IS the character);
when fitting the **splice** alone we re-render with CHARACTER OFF on our side and
compare the stretch-stage geometry (output length + comb), not the full chain —
the chain is calibrated separately and is orthogonal to `SpliceCal` ([DSP] §8).

---

## 3. Disjoint calibration vs validation sets (no overfitting)

The panel critique is explicit: the calibration set and the held-out validation
set must be **disjoint by input file AND by parameter point** ([TS] §7 Wave 2), so
a `SpliceCal` value fitted on the calibration set is *predicting* the validation
captures, not memorizing them.

**Calibration set** (used to FIT `SpliceCal` / check the D-TIME mapping):
- inputs: `sine440.wav`, `clicktrain_2hz.wav`
- parameter points: S1100 cycle 1000 @ 300%, cycle 500 @ 200%; S950 D-TIME 1000 @
  200%, D-TIME 500 @ 200%
- caseIds: `s1100_sine_300`, `s1100_sine_cycle500_200`, `s1100_click_300`,
  `s950_sine_dtime1000_200`, `s950_sine_dtime500_200`, `s950_click_dtime1000_300`

**Validation set** (held out; NEVER fed to the fitter — only PREDICTED):
- inputs: `breakslice.wav`, `saw100.wav` (different files)
- parameter points: S1100 cycle 1000 @ 300% on breakslice (the jungle target),
  S950 D-TIME 2000 @ 200% on sine (a different D-TIME point than any calibration
  point)
- caseIds: `s1100_jungle_amen_300`, `s950_sine_dtime2000_200`

Disjointness holds on both axes: the validation inputs (`breakslice`, `saw100`) are
not in the calibration inputs (`sine440`, `clicktrain_2hz`), and the validation
D-TIME point (2000) is not a calibration D-TIME point (1000, 500). The fitter is
run on the calibration set; the proposed `SpliceCal` is then scored against the
validation set without re-fitting. A good fit predicts the held-out captures within
the documented tolerance; a fit that only matches the calibration set is overfit
and rejected.

The tool enforces the split: `--captures <dir> --set calibration` fits, and
`--captures <dir> --set validation --overlap-f <fitted>` scores the held-out set
with the fitted value frozen (no search). The two sets are tagged in the manifest
(`set: "calibration" | "validation"`), and the tool refuses to fit on a case tagged
`validation`.

---

## 4. The calibration tool (`tools/calibrate`)

`tools/calibrate` is JUCE-free and links only `mwstime_core` (+ the shared
`mwstime_render_lib` case schema). It reads an arbitrary local capture directory
(captures are not committed) plus a calibration manifest (same schema as
`tests/golden/cases.json`, extended with a `set` tag) and an inputs directory.

For each case it **measures**:
- **flutter rate** — the splice-comb characteristic frequency, `modelRate / hop_out`
  ([TS] §3 property 8: stretching a steady sine puts sidebands at this spacing);
- **stutter schedule** — `hop_out` (output grain spacing) and `hop_in` (input
  advance, `round(hop_out / T)` in CLASSIC, [DSP] §3.2);
- **schedule-derived output length** — `(G−1)·hop_out + C`, the "bad timing" length,
  NOT `round(N·T)` ([DSP] §3.4; [TS] §3 property 2).

It then **fits `SpliceCal`** by the only honest method when the hardware cannot be
analytically inverted: a grid search over `overlapF` (and the discrete `shape` /
`rounding` choices) that **re-renders the engine** at each candidate and minimizes a
distance to the capture (output-length match + RMS / spectral distance after
peak-normalizing both sides — Akaizer-style normalization is irrelevant here since
captures are unnormalized, but the metric is normalization-robust). For a synthetic
capture made at a known `SpliceCal` the search recovers it exactly (distance 0); for
a real capture it finds the closest `overlapF` and reports the residual. The tool
prints the proposed `SpliceCal { overlapF, shape, rounding }`.

It also performs the **D-TIME mapping check**: for each S950 case it confirms the
fitted `hop_out` is consistent with `C · (1 − F)` where `C = mapDTimeToCycleLen(dTime)`
([DSP] §5) — i.e. that the single (PI) assumption "D-TIME ≡ cycle length in samples"
predicts the measured comb across the disjoint D-TIME points. A mapping that
predicts all three D-TIME points (calibration 1000/500 + validation 2000) within
tolerance **passes the v1-freeze gate**; one that does not fails it (§6).

**Tool self-test (ctest `calibrate`):** before any real capture exists, a Catch2
test renders the corpus at the *default* `SpliceCal` and again at a *deliberately
different planted* `SpliceCal` (the planted renders stand in for "captures"), runs
the fitter, and asserts it recovers the planted `overlapF` (and the planted D-TIME
consistency). This proves the tooling is correct before hardware arrives.

---

## 5. D-TIME validation procedure (the v1-freeze gate)

The S950 D-TIME mapping (`mapDTimeToCycleLen`, [DSP] §5) is **(PI),
deviation-until-calibrated**, and it is a **v1-freeze gate**. Procedure:

1. Source the three S950 D-TIME captures (D-TIME 1000, 500, 2000 — two calibration,
   one validation; §3) per the §2 protocol.
2. Run `tools/calibrate --set calibration` on the D-TIME 1000 and 500 captures: fit
   `overlapF` AND confirm that interpreting D-TIME as cycle length in samples (the
   mapping under test) predicts each capture's measured comb / output length within
   tolerance.
3. Run `tools/calibrate --set validation --overlap-f <fitted>` on the held-out
   D-TIME 2000 capture: with `overlapF` frozen, the D-TIME→C mapping must PREDICT the
   2000 capture (different D-TIME point, different from any fitted point).
4. **Gate decision:**
   - PASS — all three D-TIME points are predicted within tolerance ⇒ the mapping is
     confirmed; freeze v1 with the "authentic" S950 D-TIME label intact. If the fit
     changed `SpliceCal` or the mapping at all, that change ships as a calibration
     ADR + golden re-bless (§6, path A).
   - FAIL or NO CAPTURES by freeze ⇒ the "authentic" label is **downgraded** in
     release notes for the affected models ([TS] §7 exit criteria), the (PI) tags and
     this open gate are stated, and the constants remain provisional for a post-v1
     calibration (§6, path B).

---

## 6. Outcome wiring (decision paths)

Recorded here per task 026b scope; executed when captures arrive (separate PRs).

**Path A — captures arrive and `SpliceCal` (or the D-TIME mapping) changes:**
1. Open a **calibration ADR** (plan/decisions/NNN) recording the captured evidence,
   the fitted constants, the disjoint cal/val residuals, and the rights provenance.
   Re-blessing after a splice-calibration change additionally requires this ADR
   ([TS] §4 blessing policy; [ADR1] consequences).
2. Change the constants in their single home — `CyclicEngine::SpliceCal` defaults
   ([DSP] §3.1) and/or `S950Engine::mapDTimeToCycleLen` ([DSP] §5) — a **data-only**
   change, never an engine re-architecture (the whole point of `SpliceCal`, [ADR1]).
3. Re-bless the goldens via the task-026 procedure
   (`cmake --build --target bless_goldens`, `BLESS_REASON` quoting the ADR) in a
   **separate PR** (the re-bless itself is out of scope here, task 026b "out of
   scope"). The review agent must approve the MANIFEST.json diff.
4. Update the (PI) tuning notes to "confirmed against hardware (ADR-NNN)".

**Path B — captures cannot be sourced by freeze:**
1. "Authentic" labeling is **downgraded** for the un-validated models in the release
   notes ([TS] §7 exit criteria): the UI/docs state the splice constants and the
   S950 D-TIME mapping are provisional (PI) pending hardware calibration.
2. The open gate stays ticketed; every (PI) constant retains its tuning note
   ([TS] §7: "every (PI) constant either confirmed or ticketed").
3. No constant changes (no ADR, no re-bless) — the provisional defaults ship as-is
   with honest labeling.

Either way the constants only ever move as a data change in their single home, and
the golden matrix ([TS] §4) catches any unintended drift.
