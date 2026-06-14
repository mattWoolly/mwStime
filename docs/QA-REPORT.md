# mwStime v1 — QA Report (Adversarial Audit)

**Date:** 2026-06-14
**QA lead:** matt.woolly@postscript.io
**Scope:** v1 ship-readiness audit of the mwStime plugin, DSP core (`libs/mwstime-core`),
CLI (`tools/mwstime-render`), and supporting harnesses/validators.
**Method:** four-dimension adversarial audit (dsp-abuse, authenticity, plugin-state,
ui-docs). Every reported finding was 3-way verified (reproduce → read source →
cite/contradict design docs). Findings the auditors could not refute are listed as
*confirmed*; everything an auditor knocked down is listed under *rejected / false
positives* so the record shows it was checked.

---

## 1. Executive summary — verdict

> **UPDATE 2026-06-14 — both HIGH findings RESOLVED; v1 ship-ready.**
> F1 (INTELL plausible-fake) fixed by task **054** (commit `ef5dd4f`): `stretchMode` is now
> greyed, non-automatable, and unreachable via jog/host; defense-in-depth prevents INTELL
> from ever rendering as CYCLIC. F2 (FxEngine data race) fixed by task **055** (commit
> `6a7069d`): `active_` handoff is atomic on both sides, the LCD reads an audio-thread-
> published snapshot, and the `[tsan]` test now exercises the racing accessors —
> **2/2 ThreadSanitizer tests pass clean**. Final on-main sweep: ctest 497/497 (+3
> validators), TSan 2/2, pluginval/auval/clap all green; CI green on macOS + Linux
> (Windows allow-fail per the 3rd-goal priority). The original audit verdict is preserved
> below for the record.

**Original verdict (pre-fix): NOT ship-ready as-is. Two HIGH-severity defects must be fixed
before v1; the remainder are acceptable-to-document and can ship as known limitations.**

The product is in strong shape. The DSP core is unusually faithful to the documented
Akai S-series emulation: integer-hop CLASSIC scheduling, sample-exact REVISED length,
pure-varispeed S900, the S950 999% clamp / D-TIME→cycle / MON1 autocorrelation snap,
correct per-model bit depths and filter slopes, model-rate invariance, and an authentic
memory cap all verified against `dsp-engine.md`, the ADRs, and the research. Roughly
1,700 hostile renders produced **zero** crashes, segfaults, or aborts. All validators
(pluginval VST3+AU strictness 10, auval `-v aumf`, clap-validator 0.3.2) pass against
the actually-built bundles, and the ctest suite is green (485/485 on a fresh build;
the prompt's "499" is a stale count).

Two findings, however, block a clean v1:

1. **Authenticity (HIGH): INTELL is silently rendered as CYCLIC.** The `stretchMode`
   field that `dsp-engine.md` §4 says must be *greyed* at v1 is in fact editable, host-
   automatable, and reachable through the jog wheel — and when INTELL is selected the
   engine produces byte-identical CYCLIC audio under the INTELL name. This is *exactly*
   the "plausible-fake" failure mode ADR-001 exists to prevent. It is an authenticity-
   integrity defect, not merely a UI nit.

2. **Plugin-state (HIGH): ThreadSanitizer-proven data race on `FxEngine::active_`.** The
   non-atomic `shared_ptr` handoff in the always-on FX path is written on the audio
   thread and read by the 30 Hz message-thread LCD accessors, with no synchronization —
   a use-after-free-class race plus torn-bool reads, triggered by ordinary user actions
   (model/bandwidth/FS/character change while audio plays and the editor is open). This
   directly contradicts the cross-thread-handoff guarantee in `architecture.md` §4 and
   the TSan-coverage claim in `testing-strategy.md` §6, and the existing FX-reconfig TSan
   test does not exercise the racing accessors.

Both are reproducible and verified. Neither is a crash-on-launch, but both undermine a
core ship guarantee (authenticity integrity; lock-free correctness), so v1 should not
ship until they are resolved. The medium/low findings are real but bounded and
design-acknowledged; they are tracked as known limitations.

**Confirmed findings by severity:** critical 0 · high 2 · medium 3 · low 3.

---

## 2. Confirmed findings

| # | Dimension | Sev | Title | Location | Recommended fix | Contradicts |
|---|-----------|-----|-------|----------|-----------------|-------------|
| F1 | authenticity | **high** | INTELL is silently rendered as CYCLIC — the "plausible-fake" failure ADR-001 exists to prevent | `libs/mwstime-core/src/engine/OfflineRenderer.cpp:321-345` (S1000/S1100/`default:` branch never reads `params.stretchMode`); display `plugin/ui/LcdPageModel.cpp:90` | Gate INTELL at the UI/param layer per the spec: grey the `stretchMode` field (`editable=false`) and/or coerce/refuse INTELL; mark the param `withAutomatable(false)`. The renderer fallback is harmless once INTELL is genuinely unreachable. | `dsp-engine.md` §4 L239-240 ("INTELL greyed"); ADR-001 (`plan/decisions/001-dsp-engine-architecture.md`) res.#2 L53-56 ("plausible-fake failure mode"); `Parameters.cpp:151-153` self-contradicting comment |
| F2 | plugin-state | **high** | Data race on `FxEngine::active_` (non-atomic `shared_ptr`) between audio-thread `processBlock` and message-thread LCD accessors | `plugin/src/FxEngine.h:272-273` (audio write) racing `:325-328` `clampActive()` / `:336-344` `monoSummed()`; reached via `EngineHost.h:242,246` and `PluginEditor.cpp:350,377` (30 Hz poll); torn bool `RealtimeStretcher.cpp:142` | Make `active_` an `atomic<shared_ptr>` (or `atomic_load`/`atomic_store` on both sides) and expose an audio-thread-published snapshot for the LCD; extend the `[tsan]` test to call `clampActive()`/`monoSummed()` concurrently. | `architecture.md` §4 L161-168 (all cross-thread buffer handoffs are `shared_ptr` + TSan-tested, incl. "FX history reconfiguration"); `testing-strategy.md` §6 L92-95 (TSan over FX history reconfiguration); FxEngine.h:461-462 comment ("never touched by the message thread") |
| F3 | authenticity | medium | STRETCH MODE field is freely editable to INTELL and the param is host-automatable — not greyed/blocked as the spec requires | `plugin/ui/LcdPageModel.cpp:183` (`editable=true`, no Greyed style); `plugin/src/state/Parameters.cpp:154-156` (`AudioParameterChoice {CYCLIC,INTELL}` with no `withAutomatable(false)`) | Same UI/param gate as F1 (grey + non-automatable). Add a test that asserts the UI constraint, not just choices/default. | `dsp-engine.md` §4 L239-240 and §2 L57; ADR-001 L89 ("INTELL absence … communicated in the UI (greyed fields)") |
| F4 | plugin-state | medium | CHARACTER is automatable yet its listener triggers a 30 s history-ring allocation + `setLatencySamples` on the message thread | `plugin/src/PluginProcessor.cpp:26,169-182` → `FxEngine.h:195-212` `requestReconfigure` (buildPrepared allocates) + `setLatencySamples`; `Parameters.cpp:215-219` created without `withAutomatable(false)` | Mark CHARACTER `withAutomatable(false)` like BANDWIDTH/FS, since it changes reported latency L (it is part of the latency `ConfigKey`). | `dsp-engine.md` §7.4 L369 (params non-automatable "precisely because they change L"); ADR-006; `architecture.md` §6 L277-279; FxEngine.h:94-96 ConfigKey contract comment (self-contradicting) |
| F5 | dsp-abuse | medium | NaN parameter value defeats `ModelSpec::clamp`, producing a silent zero-length render reported as success | `libs/mwstime-core/src/model/ModelSpec.cpp:179,185,189-190,192` (`std::clamp(NaN)` returns NaN); manifests `OfflineRenderer.cpp:130,160-161,235-243`; CLI parser `tools/mwstime-render/Args.cpp:32-43` (no `isfinite` check despite its own contract) | Add `std::isfinite` normalization in `ModelSpec::clamp` (the single clamping authority) and honor `parseDouble`'s "finite" contract in `Args.cpp`. | ModelSpec "single clamping authority" comments (`ModelSpec.h:9-10`, `ModelSpec.cpp:4`, `OfflineRenderer.cpp:187`); `dsp-engine.md:47-48`; `testing-strategy.md:190` ("hunt for NaN/inf/denormals"); `Args.cpp:32` parseDouble "finite" doc |
| F6 | dsp-abuse | medium | NORM ON amplifies a single non-finite sample into a whole-buffer NaN | `libs/mwstime-core/src/engine/OfflineRenderer.cpp:390-401` (norm block) + `peakAbs` `:54-61` (no `isfinite` guard; `gain = Inf/Inf = NaN`) | One-line `isfinite` guard on `sourcePeak`/`outPeak`/`gain`. | `dsp-engine.md` §7.3 (gain = sourcePeak/outPeak, no non-finite contemplation); `testing-strategy.md:190`. NORM is OFF by default (modern-UX opt-in) and the trigger is an out-of-domain non-finite input WAV. |
| F7 | dsp-abuse | low | VarClock (S900/S950) Butterworth leaks float32 denormals into SAMPLE-mode float output on decaying/silent tails | `libs/mwstime-core/src/model/VarClockChain.cpp` + `libs/mwstime-core/src/core/Butterworth.cpp` (no FTZ/flush in `libs/mwstime-core`); offline render runs on a JUCE worker with no `ScopedNoDenormals` (`EngineHost.cpp:81-82,231`) | Add a `ScopedNoDenormals`/FTZ scope around the offline render (or flush in the Butterworth/output stage) and tighten the SAMPLE-path test to assert denormal-freeness. | `tests/plugin/test_dsp_adversarial.cpp:108-110` (states "denormals flushed" intent) only enforced on FX path; `testing-strategy.md:190-191`. Output stays finite and inaudible (~1e-39); offline-only; negligible CPU penalty on the validated Apple-Silicon platform. |
| F8 | plugin-state | low | Embedded-FLAC session restore is 24-bit lossy → re-render-on-load not bit-identical to the original render for >24-bit sources (e.g. 32-bit-float WAV) | `plugin/src/state/StateBlobCache.cpp:158-161` (`withBitsPerSample(24)`), `decodeFlac:198-211`; tolerance `tests/plugin/test_state_embed.cpp:287-293` (margin 2e-4) | Document the 24-bit-embed caveat (or PI-scope it) in `architecture.md` §6 re-render-determinism; a truly lossless float embed needs a non-FLAC container (JUCE FLAC hard-caps at 24-bit). | `architecture.md` §6 re-render-determinism (literal triple-→-bit-identical not strictly violated; the implicit "original render reproduced" + embedded-vs-path equivalence are). Error bounded ~1.2e-7, design-acknowledged ("audition-identical"). |
| F9 | ui-docs | low | `PluginEditor::processor` shadows `juce::AudioProcessorEditor::processor` (`-Wshadow-field`) on every clean build | `plugin/src/PluginEditor.h:121` (vs base `juce_AudioProcessorEditor.h:69`); flag via `JUCEHelperTargets.cmake:54` `-Wshadow-all`, linked at `plugin/CMakeLists.txt:98` | Rename to `processor_` or use the inherited reference. No functional consequence; build is not `-Werror`. | None. Build-hygiene noise, consistent with ADR-005's expectation of a clean C++ UI backlog. |

---

## 3. Rejected / false positives (checked and dismissed)

These were investigated and **refuted** by the auditors. Listed so the record shows they
were verified, not skipped.

- **Engine character-OFF null path passes non-finite input through verbatim** (dsp-abuse).
  *Refuted.* Reproduces, but it is the documented `dsp-engine.md` §3.2/§8.4 verbatim
  null-test bypass — garbage-in/garbage-out is the contract. No sanitization contract
  exists anywhere; integer/default output paths clamp to ±FS (`WavIo.cpp:66-75`); the
  live FX bus is separately finiteness-tested. Not a bug.

- **CLASSIC realized stretch ratio runs ~8-10% short of requested** (authenticity).
  *Refuted.* Reproduced the exact arithmetic against the compiled engine — it matches
  `CyclicEngine.cpp:75-81` and is the *prescribed* behavior: `dsp-engine.md` §3.2
  L141-143 ("realized ratio = hop_out/hop_in … quantized … 'bad timing'"), corroborated
  by `akaizer-analysis.md`. The "sample accurate" research claim means fidelity-to-
  hardware, which itself has the quirk. Finding self-refutes ("contradicts: none"). Not
  a bug.

- **Committed `build/` artifacts are stale, masking test results / parallel TSan fail**
  (plugin-state). *Refuted.* `git ls-files build/` returns nothing; `build/` is
  gitignored. No committed artifact exists. The local binary is up-to-date; the
  publication TSan test passes 7/7 across repeated `-j6` runs. Premise factually false.

- **Test count / validator-availability claims don't match repo** (plugin-state).
  *Refuted.* `ctest -N` reports 485 (the prompt's "499" is stale; it appears in no design
  doc/ADR). clap-validator was reproduced as a real PASS (21 tests, 18 passed, 3 internal
  skips) against the built `.clap` — not a self-skip. `pluginval`/`clap-validator` absent
  from PATH is by design (pinned, checksummed binaries in `build/validator-tools/`).

- **README "Screenshots" present blessed PNGs as "the UI" but omit live components**
  (ui-docs). *Refuted.* Observations accurate (the harness links only Faceplate+LCD; soft
  keys/jog/model-selector/waveform render as labeled placeholder slots the Faceplate
  itself draws), but the README captions each image as a "faceplate"/"LCD page" and frames
  them as test artifacts — no completeness claim, no broken render, no doc/ADR
  contradiction. Cosmetic-tone nitpick, not a defect.

- **Blessed UI golden MANIFEST records placeholder bless reason "dry-run check"**
  (ui-docs). *Refuted.* `tests/ui/blessed/MANIFEST.json:7` does contain that string and is
  committed, but the bless gate only requires a non-empty reason and it passes; nothing
  misbehaves; no schema/ADR contradiction. Cosmetic provenance nit, not a bug.

---

## 4. Verification performed

What was actually run during the audit (not a plan — these were executed):

**Build / environment**
- Rebuilt the stale local `build/default` from current sources; confirmed up-to-date via
  no-op `cmake --build` (the publication binary was newer than its sources after rebuild).

**Test suite**
- Full ctest: **485/485 pass** serial; **485/485 pass** under `-j6` on a fresh build.
  (One host-smoke standalone test flaked under parallelism but passes on isolated rerun —
  infra/host flake, not a DSP issue.)
- Targeted re-runs: `[publication]` (2038 assertions, 5 cases) green across 5 consecutive
  `-j6` runs; `[tsan]` / publication / threading subset 7/7 across runs; `[state_embed]`
  (883 assertions) pass; `[parameters]` non-automatable-set test (38 assertions) pass.

**Validators (against the actually-built bundles)**
- pluginval VST3 strictness 10 — PASS.
- pluginval AU strictness 10 — PASS.
- `auval -v aumf` — PASS.
- clap-validator 0.3.2 (pinned binary) against the built `.clap` — PASS (21 run, 18
  passed, 0 failed, 3 internal skips), EXIT 0 — verified a real validation, not a skip.

**DSP render abuse (CLI: `build/default/tools/mwstime-render/mwstime-render`)**
- ~25 hostile input WAVs (silence, DC, full-scale, impulse, single/two-sample,
  0-frame/empty, NaN/Inf/all-NaN, denormal/decay tails, tiny, huge 1e20, stereo, and a
  22.05/44.1/48/88.2/96 kHz host-rate matrix).
- 257 structured sweeps + 1,200 randomized fuzz renders across all 4 models, ratios
  24-5000% (at/past clamps), cycle 20-2000 (and -100..100000), transpose ±25/Inf/NaN,
  bandwidth extremes, character/norm toggles, all bit depths. **Zero** crashes/segfaults/
  aborts in ~1,700 renders.
- Verified positives: CLASSIC/REVISED output-length formulas match `dsp-engine.md` §3.4
  (30/30); memory cap refuses 60 s @ 2000% with the authentic "** NOT ENOUGH MEMORY **"
  before allocating, while 20 s @ 2000% succeeds; cycle clamp 20..2000 correct; model-
  rate invariance holds at every host rate (`dsp-engine.md` §3.5); empty/single-sample
  handled.

**Authenticity arithmetic probes**
- 5 standalone probe programs linked against `libmwstime_core.a` exercising the real
  arithmetic (CLASSIC integer hop = round(hop_out/T), REVISED fractional reads, S900 ZOH
  zero-interpolation, S950 MON1 autocorrelation snap, clamp behavior). `std::clamp(NaN)`
  defeat confirmed by a 4-line probe (`std::clamp(nan,-24,24)==nan`; `±inf` clamp
  correctly). INTELL≡CYCLIC byte-identity confirmed via `cmp` + matching md5
  (`c331dbf64a34293f7234a62df10727ff`) on `--stretch-mode CYCLIC` vs `--stretch-mode
  INTELL` renders.

**Threading**
- Standalone ThreadSanitizer reproduction (`clang++ -fsanitize=thread`) built against the
  real `plugin/src/FxEngine.h` + core sources (FxEngine.h is JUCE-free), with audio /
  reconfigure / LCD-poll threads mirroring production. TSan reported two races: size-8
  `shared_ptr` write-vs-read on `active_` (via `EngineGraveyard::retire`, `FxEngine.h:477`
  from `:272`) and size-1 torn `clampActive_` (`RealtimeStretcher.cpp:142`). Verified the
  existing `[tsan]` test (`test_enginehost_fx_reconfig.cpp:181-245`) never calls the
  racing accessors.

**Code reading** — `dsp-engine.md`, `architecture.md`, `testing-strategy.md`,
`ui-design.md`, ADRs 001/003/005/006, all three research docs, and the full core engine +
plugin state/threading/UI sources were read to ground every contradiction citation.

---

## 5. Known limitations / follow-ups

The HIGH findings (F1, F2) have backlog tasks created (see below). The medium/low findings
are tracked here:

- **F3 / F4 (medium):** INTELL UI gating and CHARACTER automatability are the same
  authenticity/PDC-discipline class as F1. Folding F3's fix into the F1 task (054) is the
  natural grouping; F4 (CHARACTER `withAutomatable(false)`) is a one-line change worth a
  small follow-up but is not v1-blocking on its own.
- **F5 / F6 (medium, dsp-abuse):** Non-finite hardening. Both are two-line/one-line fixes
  (`isfinite` in the single clamping authority `ModelSpec::clamp`; `isfinite` guard on the
  norm gain). Real but only reachable via deliberately non-finite literals/inputs; NORM is
  OFF by default. Recommend bundling into one small "non-finite hardening" task pre-v1.1.
- **F7 (low):** Offline denormal flush — add `ScopedNoDenormals`/FTZ around the offline
  render and assert denormal-freeness on the SAMPLE path. Inaudible; offline-only;
  negligible on Apple Silicon.
- **F8 (low):** Document the 24-bit embedded-FLAC re-render caveat in `architecture.md`
  §6 (or PI-scope it). Inaudible (~1.2e-7); a lossless float embed is constrained by
  JUCE's 24-bit FLAC cap.
- **F9 (low):** Rename `PluginEditor::processor` → `processor_` to clear the
  `-Wshadow-field` warning. Build hygiene only.

**Created backlog tasks (HIGH findings) — both RESOLVED 2026-06-14:**
- `plan/backlog/054-gate-intell-stretch-mode.md` — F1 (+ F3) — status: **done** (PR #60, `ef5dd4f`)
- `plan/backlog/055-fxengine-active-atomic-shared-ptr-race.md` — F2 — status: **done** (PR #61, `6a7069d`)

**Remaining follow-ups (non-blocking, tracked for v1.1):**
- F4–F9 medium/low findings above remain documented known limitations (no separate tasks).
- `plan/backlog/053-fx-streaming-character-chain.md` — FX-path per-block character chain enhancement.
- Harness note: running `ctest --preset tsan` *unfiltered* reports test #53 ("S3000 is
  reserved — fails loudly") as a failure because that intentional `abort()` death test trips
  the sanitizer's exit handling. It passes in the normal build and is **not** a defect. The
  TSan preset is meant to run only the `[tsan]`-labelled concurrency tests
  (`ctest --preset tsan -L tsan`, 2/2 clean). A tiny v1.1 cleanup could exclude death tests
  from the tsan preset.
