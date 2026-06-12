# mwStime — Testing Strategy

Status: accepted (panel synthesis), 2026-06-12. Aligned with plan/ORCHESTRATION.md
phases 4–6 (local verification before PR; CI added late; adversarial QA fleet).

Panel-driven corrections baked in: pluginval covers only VST3/AU (CLAP needs
clap-validator, LV2 needs lv2lint/lv2_validate); bit-exactness is scoped to the
integer CLASSIC stretch path; Akaizer is a *secondary, local-only* cross-check, never
a calibration oracle (its "near-exact" fidelity claim was refuted 0-3 —
deep-research-report.md Finding 6 / Refuted claims); no copyrighted audio in the repo.

---

## 1. Layers

| Layer | Tool | Runs where | Gate |
|---|---|---|---|
| 1. Unit tests (mws core) | Catch2 via CTest | every PR, locally | hard |
| 2. Property/invariant tests (DSP) | Catch2 | every PR | hard |
| 3. Golden-render character regression | `mwstime-render` CLI + comparer | every PR touching `libs/` or `tools/` | hard |
| 4. Plugin format validation | pluginval (VST3/AU) + clap-validator + lv2lint/lv2_validate | every PR touching `plugin/` | hard |
| 5. State round-trip / automation tests | Catch2 + JUCE unit-test harness | every PR touching `plugin/` | hard |
| 6. Host smoke tests | scripted + manual matrix | QA phase + release | soft → hard at release |
| 7. Adversarial QA fleet | agent checklists | QA phase | report-driven |

All of 1–5 run from `ctest --preset default` so a dev agent has a single verification
command (per plan/backlog/README.md workflow).

## 2. Unit tests (`tests/unit/`)

One Catch2 file per `mws::` module. Required minimum coverage by spec section
(dsp-engine.md references):

- `Resampler`: impulse/sine SNR bounds; identity at ratio 1.0; group delay reported
  matches measured (feeds the FX latency formula, dsp-engine.md §7.4).
- `Butterworth` (6th-order, S950 reconstruction): -3 dB point within 1% of requested
  cutoff; ≈ -36 dB/oct slope verified at 2× cutoff; magnitude at fc/2fc/4fc within
  ±0.5 dB of the analytic 6th-order response **at three clock rates** (cutoff
  tracking — deep-research-report.md Finding 4); stability across the 7.5–48 kHz
  clock range and under per-note coefficient recompute.
- `MovingLpf` (3rd-order, S1000/S1100 voice filter): analytic -18 dB/oct response
  check (one real pole + Q=1 pair — NOT three identical one-poles; dsp-engine.md
  §8.2); transparency when fully open (panel critique: the first draft had no test
  for this filter at all).
- `Quantizer`: 12-bit step size exact (`1/2048` full scale); idempotent on already-
  quantized input; distinct-value count ≤ 4096 on arbitrary input.
- `AutoCorr` / auto-cycle (§7.1): synthetic 100 Hz saw at 44.1 kHz must detect a lag
  of 441 ± 1 samples; sub-threshold noise falls back to 1000.
- `WavIo`: round-trip 16/24/32-int + float32, mono/stereo.
- `LcdPageModel` (in plugin tests): hardware-unit formatting (e.g. timeFactor 300 →
  `"300%"`, cycle 1000 → `"1000"`), per-model field visibility per dsp-engine.md §2,
  clamp feedback strings (`999%` on S950, `FX MIN 100%`).

## 3. Property/invariant tests (the authenticity contracts)

These encode the research-pinned behaviors of dsp-engine.md §3 and must never be
weakened without an ADR:

1. **CLASSIC verbatim-copy property**: for transpose=0 and character OFF, every
   output sample of `CyclicEngine(CLASSIC)` equals `src[i]` at an integer offset, or —
   **inside a crossfade region only** — the rounded convex combination of exactly two
   such samples (test with `src[n] = n` index-encoding). Pins "stretch alone never
   resamples" (akaizer-analysis.md §2.4/§4.1) with the crossfade exemption stated
   (panel critique: a blanket zero-interpolation claim fails inside every fade).
2. **CLASSIC length quantization**: output length equals the schedule-derived value
   computed independently in the test from the §3.4 scheduler
   (`(G−1)·hop_out + C` with `hop_in = round(hop_out/T)`), and is NOT `round(N·T)` —
   pins "bad timing" (akaizer-analysis.md §2.2). The length formula MUST come from
   the adopted [AKZ §4.2] scheduler, never from the §10 splice-model `N·C/step`
   (panel critique P1/P3: mixing scheduling models makes this test fail by design).
3. **REVISED timing exactness**: output length within ±1 sample of `N·T` for
   fractional time factors (akaizer-analysis.md §2.2).
4. **FX contract tests** (ADR-006):
   a. T=100%, character OFF: FX output equals input delayed by exactly the reported
      latency — sample-exact null.
   b. **Stream/offline equivalence**: with parameters frozen from a known start
      state, streamed output over a window equals the offline render of the same
      history, sample-for-sample (one core, two front-ends — the panel's shared
      demand made falsifiable).
   c. T<100% FREE: engine output identical to T=100% (clamp active); LCD model
      reports the clamp. T<100% SYNC: window plays compressed then silence to the
      boundary.
   d. T>100% FREE: resync occurs only at grain boundaries and only at history
      exhaustion; SYNC: resync exactly at window boundaries under a scripted
      transport.
   e. Latency: reported value matches the dsp-engine.md §7.4 formula for each model
      and bandwidth setting; changes only on model/bandwidth/FS change.
5. **Stereo coherence**: two-channel render/FX pass with identical params and
   identical channel content is sample-identical per channel; with differing content
   the hop schedule (grain launch times) is identical across channels (CLASSIC
   phase-coherence — akai-manuals-specs.md §3 stereo note).
6. **Determinism + publication safety**: same (input, ParamSnapshot, engine version)
   twice ⇒ bit-identical buffers on the same platform (no uninitialized state; fixed
   rng seed for dithered paths). ThreadSanitizer run over the render-publish/swap
   path and the FX history reconfiguration (architecture.md §4 protocol).
7. **Model clamping**: S950 timeFactor clamps at 999 at the engine while the host
   parameter range stays fixed (akai-manuals-specs.md §2; architecture.md §6);
   S900 ignores stretch-only params (dsp-engine.md §6).
8. **Splice-comb signature**: FFT of a stretched steady sine shows sidebands spaced
   at `modelRate / hop_out` within one bin of prediction — the "metallic ring" made
   measurable (akaizer-analysis.md §3 property 1; panel-endorsed as the best
   character assertion).
9. **12-bit chain**: distinct output values ≤ 4096 lattice pre-reconstruction; ZOH
   images present pre-filter, attenuated post-Butterworth; high-band energy tracks
   downward under pitch-down (the clock-tracking property — deep-research-report.md
   Finding 4).
10. **Render cap**: a render exceeding the 10-min model-rate cap is refused with the
    documented message and no allocation beyond the cap.

## 4. Golden-render character regression

Purpose: freeze the *sound* of each engine + character chain so refactors can't drift.

- **Inputs** (`tests/golden/inputs/`, committed, all ≤ 2 s, 44.1 kHz 16-bit mono
  unless noted): `sine440.wav`, `saw100.wav`, `clicktrain_2hz.wav` (transient
  smear/flam), `noiseburst.wav`, `sweep20-20k.wav` (filter/alias signature),
  `breakslice.wav` (a **self-recorded/synthesized** 1-bar drum loop — original
  material only; committing the Amen break or any copyrighted master to the public
  AGPL repo is prohibited, panel critique P4), `breakslice_22k.wav` (the same at
  22.05 kHz — cycle length is in samples, so rate changes the sound,
  akaizer-analysis.md §2.1 note), `stereo_pan.wav` (coherence).
- **Cases** (`tests/golden/cases.json`): per model: the dsp-engine.md §9 preset cases
  (incl. the documented S1100 jungle setting Cycle 1000 / Time 300%), plus extremes
  (25%, 999%/2000%, cycle 20, cycle 2000), CLASSIC and REVISED, transpose ±12,
  character ON and OFF, norm OFF (default) and ON. ~60 renders, each named
  `<model>_<case>.wav`.
- **Comparison policy (scoped bit-exactness — panel critique P3 of the purist
  proposal):**
  - CLASSIC stretch-only cases (transpose 0, character OFF): **bit-exact on all
    platforms** — the path is integer-only by construction (dsp-engine.md §3.2) and
    the build pins FP discipline (no -ffast-math, -ffp-contract=off on the core).
  - All float-stage cases (filters, SRC, transpose, REVISED): **bit-exact on the
    reference platform (macOS arm64, where goldens are blessed)**; other platforms
    compare with tight tolerance (max abs ≤ 1e-6, plus the spectral checks below).
- **Runner**: a CTest that invokes `mwstime-render --case <id>` and compares against
  `tests/golden/blessed/` with a two-stage comparer:
  1. exact/tolerance compare per the policy above;
  2. on mismatch, print diagnostics — max abs diff, RMS diff, first divergent sample,
     splice-comb peak location/level, and a 1/3-octave spectral-difference table — so
     the failure is debuggable from agent logs without DAW access.
- **Blessing procedure** (`cmake --build --target bless_goldens`, wraps
  `tools/bless_goldens.sh`):
  1. Re-renders all cases into `tests/golden/blessed/` on the reference platform.
  2. Writes `tests/golden/blessed/MANIFEST.json`: engine version hash, date, blesser,
     and a one-line justification string (the target refuses to run without
     `BLESS_REASON="..."`).
  3. Policy: blessing is only allowed in a PR whose description quotes the reason and
     links an ADR or bug; the review agent must explicitly approve golden changes
     (diff of MANIFEST.json makes this visible). Golden churn without an audible/
     intentional change is a review-rejection criterion. Re-blessing after a splice
     calibration change (`SpliceCal`, dsp-engine.md §3.1) additionally requires the
     calibration ADR.
- Renders are small (<1 MB each); committed via plain git (no LFS — keeps agent
  clones simple).

## 5. Plugin format validation (corrected scope)

pluginval **cannot load LV2 or CLAP** — the v1 format matrix needs three validators
(panel critique, all three reviews):

| Format | Validator | Invocation |
|---|---|---|
| VST3 (mac/Linux) | pluginval, strictness 10 | `tests/plugin/run_pluginval.sh` (pinned release; `--validate-in-process --repeat 3 --randomise`) |
| AU (mac) | pluginval + `auval -v aumf MwS1 MwSt` | note `aumf` — music effect, not `aufx` (architecture.md §7) |
| CLAP | clap-validator (pinned release) | `tests/plugin/run_clap_validator.sh` |
| LV2 | lv2lint + lv2_validate (sord_validate) | `tests/plugin/run_lv2_checks.sh`; Carla load smoke |

Headless Linux runs use xvfb. Run requirement: any PR touching `plugin/`; full matrix
in the QA phase.

## 6. Host smoke tests

Scripted where possible, checklist otherwise (`tests/plugin/host-matrix.md`):

| Host | Platform | Checks |
|---|---|---|
| auval | macOS | `auval -v aumf MwS1 MwSt` pass; MIDI reaches the plugin in Logic (music-effect slot) |
| REAPER | macOS, Linux | insert FX, automation write/read of timeFactor, state save/reload (embedded sample), render offline = realtime output, latency compensation null at T=100%, **latency re-report on model switch honored**, tempo change mid-play (SYNC window follows), transport stop/loop while FX windowed |
| Bitwig | Linux, macOS | CLAP load, per-track sandbox survival, tempo-sync follows host BPM change |
| Ardour | Linux | LV2 load, session reload |
| Carla / lv2lint | Linux | LV2 manifest validity |
| Logic | macOS | AU (aumf) load, MIDI routing, project reload, bounce, autosave under 16 MB embedded state (no message-thread stall — architecture.md §6 cached blob) |
| Standalone | all | FX SYNC fallback with no transport (ADR-006) |

Smoke checklist items map 1:1 to interaction flows in ui-design.md §6 (load sample,
set stretch, audition, render, FX mode, model switch).

## 7. QA phase plan (adversarial fleet, ORCHESTRATION phase 5)

Wave 1 — **DSP adversaries** (per model): hunt for NaN/inf/denormals (feed DC, ±1 FS
squares, denormal tails); extreme params (cycle 20 @ 999%, zone of 40 ms minimum —
akaizer-analysis.md §2.1 input floor); sample-rate matrix 44.1/48/88.2/96/192 kHz host
rates verifying model-rate invariance (dsp-engine.md §3.5); long-run FX soak (30 min,
memory/CPU flat); render-cap behavior at 2000% on long files.

Wave 2 — **Authenticity audit** (Akaizer demoted to corroboration — panel ruling):
- Akaizer CLASSIC CLI renders are a **secondary, local-only cross-check**: the binary
  is closed payware that cannot be committed or run in CI (32-bit i386, `research-
  cache/` is scratch/not committed — akaizer-analysis.md §1/§2.3), its fidelity is
  the developer's own characterization (refuted "near-exactly", deep-research-report.md
  Finding 6), and its certified window is **T 120–2000% only** — there is no Akaizer
  oracle for compression (25–119%) or for S950 D-TIME semantics. Procedure: QA agents
  with a locally licensed copy render the corpus, **peak-normalize both sides**
  (Akaizer normalizes since v1.3 — akaizer-analysis.md §2.1), then compare flutter
  rate (splice-comb frequency), stutter schedule, and schedule-derived output length
  analytically. Not expected to null; deviations documented.
- **Hardware captures are the primary oracle and the gate for any "authentic"
  labeling** (panel ruling on both purist and product proposals): source real
  S950/S1100 renders of the exact test corpus (community/eBay); first named target =
  the SPOD S1100 preset (Cyclic/1000/300%/Q20/W10 — deep-research-report.md
  Finding 10). Calibration of `SpliceCal` uses a **calibration set disjoint from a
  held-out validation set** (no overfitting — panel critique); the D-TIME mapping
  (dsp-engine.md §5) is a v1-freeze gate.
- Verify every (PI)-tagged constant has a tuning note or open issue.

Wave 3 — **Plugin/UI adversaries**: full §5 validator matrix, host smoke matrix,
state migration from every released stateVersion, automation spam (1000 param
changes/s incl. timeFactor across the FX clamp boundary), UI at 0.6×/2.0× scale on
HiDPI and 100% Linux X11, keyboard-only operation, screen-reader labels. UI golden
screenshots gate on macOS arm64 only (software renderer); Linux uses tolerance
compare (cross-platform AA is not deterministic — panel critique P11).

Exit criteria: all hard gates green, zero P1s, every (PI) constant either confirmed or
ticketed, golden MANIFEST stable for the final release candidate, hardware-capture
calibration either completed or "authentic" claims downgraded in release notes.

## 8. CI (deferred, ORCHESTRATION phase 6)

When enabled: GitHub Actions macOS + Linux jobs run exactly the local presets
(`configure → build → ctest → format validators`), then artifact upload. Windows job
added last. No test logic may live only in CI YAML — everything must be runnable
locally by an agent first. The Akaizer cross-check is explicitly excluded from CI
(closed binary, cannot be redistributed or fetched).
