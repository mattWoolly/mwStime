# Backlog index — v1 execution order

All tasks `status: todo`. Rows are grouped into **execution waves**: every task in a
wave depends only on tasks in earlier waves, so all tasks within a wave can run in
parallel (one agent + one worktree each, per plan/ORCHESTRATION.md). The task file's
`meta.status` is the source of truth; this table is the dependency map.

Suffixed ids (026b, 045b, …) were inserted by backlog review to preserve the
ordinal-encodes-rough-order convention without renumbering.

Deferred to v1.1 by ADR (no tasks here beyond reserved slots): INTELL engine
(dsp-engine.md §4), S3000 model (ADR-004).

| Wave | id | Title | Component | Size | Depends on | Status |
|---|---|---|---|---|---|---|
| 1 | 001 | CMake project skeleton, presets, Catch2, core stub, license-header check | infra | M | — | done |
| 2 | 002 | core Buffer / AudioView | dsp | S | 001 | done |
| 2 | 009 | ModelId, ModelSpec, ParamSnapshot (incl. qual/width) + clamping | dsp | M | 001 | done |
| 2 | 027 | JUCE 8 plugin skeleton (all v1 formats, COPY_PLUGIN_AFTER_BUILD) | infra | M | 001 | done |
| 3 | 003 | WavIo (JUCE-free WAV read/write) | dsp | M | 002 | done |
| 3 | 004 | Resampler (windowed-sinc + linear) | dsp | M | 002 | done |
| 3 | 005 | Butterworth 6th-order tracking LP | dsp | M | 002 | done |
| 3 | 006 | MovingLpf 3rd-order voice filter | dsp | S | 002 | done |
| 3 | 007 | Quantizer (12/16-bit + TPDF) | dsp | S | 002 | done |
| 3 | 008 | AutoCorr period estimator | dsp | S | 002 | done |
| 3 | 010 | CyclicEngine CLASSIC + SpliceCal | dsp | M | 002 | done |
| 3 | 013 | RepitchEngine (S900 varispeed) | dsp | S | 002 | done |
| 3 | 021 | TempoMap (sync math, window boundaries incl. time-sig numerator) | engine | S | 009 | done |
| 3 | 028 | APVTS parameter layout (+ autoCycle trigger, tests/plugin harness) | plugin | M | 009, 027 | done |
| 3 | 039 | SeriesLookAndFeel + FaceplateSpec + Faceplate | ui | M | 027 | done |
| 4 | 011 | CyclicEngine REVISED | dsp | S | 010 | done |
| 4 | 014 | Auto cycle detection (autC/AUTO-D) | dsp | S | 008 | done |
| 4 | 016 | Variable-clock chain (S900/S950, early-risk) | dsp | M | 004, 005, 007 | done |
| 4 | 017 | S1000/S1100 fixed-rate chain | dsp | M | 004, 006, 007 | done |
| 4 | 029 | State tree, stateVersion, migrations (incl. zone + clampMemory fields) | plugin | S | 028 | done |
| 4 | 040 | LCD font + LcdDisplay | ui | M | 039 | done |
| 4 | 041 | LcdPageModel (headless; load-error + embed-status lines) | ui | M | 010, 028 | done |
| 4 | 042 | SoftKeyBar + cursor/ENT + JogWheel | ui | M | 039 | done |
| 4 | 043 | WaveformView (+ click-to-audition event) | ui | M | 039 | done |
| 4 | 044 | ModelSelector + control panel | ui | S | 028, 039 | done |
| 5 | 012 | Cyclic property suite (comb/stereo/determinism) | qa | S | 011 | done |
| 5 | 015 | S950Engine (D-TIME, MON1/POL2, AUTO-D) | dsp | M | 010, 014 | done |
| 5 | 018 | Transpose stage (sinc + clock-modulation) | dsp | M | 004, 009, 016 | done |
| 5 | 019 | CharacterChain unified API + bypass | dsp | M | 009, 016, 017 | done |
| 6 | 020 | OfflineRenderer (cap, norm, progress/abort) | engine | M | 009, 011, 013, 015, 018, 019 | done |
| 6 | 022 | RealtimeStretcher FREE + latency + null (multichannel, shared schedule) | engine | M | 009, 011, 012, 013, 019 | done |
| 7 | 023 | RealtimeStretcher SYNC windows | engine | M | 021, 022 | done |
| 7 | 024 | Stream/offline equivalence (+ host-rate matrix, FX stereo) | qa | S | 020, 022 | done |
| 7 | 025 | mwstime-render CLI | engine | M | 003, 020 | done |
| 7 | 030 | Render worker + RCU publication + TSan presets | plugin | M | 020, 027 | done |
| 8 | 026 | Golden harness, inputs, comparer, bless | qa | M | 025 | done |
| 8 | 031 | FileLoader thread (WAV/AIFF/FLAC) | plugin | M | 027, 030 | done |
| 8 | 033 | EngineHost FX path + latency reporting + scope FIFO | plugin | M | 022, 023, 028, 030 | done |
| 9 | 026b | Hardware-capture sourcing + SpliceCal/D-TIME calibration (early QA, v1-freeze gate) | qa | M | 026 | done |
| 9 | 026c | Akaizer secondary cross-check (local-only, never CI) | qa | S | 026 | done |
| 9 | 032 | FLAC state-blob cache / embed audio | plugin | M | 029, 031 | done |
| 9 | 034 | SAMPLE mode: slot, GO flow, SamplePlayer, zone preview, outTrim | plugin | M | 022, 028, 029, 030, 031 | done |
| 9 | 037 | Host tempo sync + source BPM | plugin | S | 021, 029, 033 | done |
| 10 | 034b | Adversarial DSP QA — NaN/denormals, rate matrix, soak, (PI) audit | qa | M | 033, 034 | done |
| 10 | 035 | MIDI repitch voice (mono, filter retune) | plugin | M | 016, 034 | done |
| 10 | 036 | Drag-out export | plugin | S | 003, 034 | done |
| 10 | 038 | Factory presets (dsp-engine §9) | plugin | S | 026, 029, 034 | done |
| 10 | 045 | PluginEditor assembly — layout, LCD binding, field editing | ui | M | 033, 034, 040, 041, 042, 043, 044 | done |
| 11 | 045b | Editor interactions — soft keys, drop/drag, FX greying, a11y | ui | M | 036, 037, 045 | done |
| 12 | 046 | Model switching (cross-fade, clamp memory, PDC) | ui | M | 029, 045b | done |
| 12 | 047 | Resizable editor + persisted scale + full hamburger menu | ui | S | 029, 045b | done |
| 12 | 048 | pluginval/auval/clap-validator scripts (macOS) | qa | S | 045b | done |
| 13 | 047b | UI golden screenshot regression harness (macOS-arm64 gate) | qa | M | 046, 047 | done |
| 13 | 048b | Host smoke test matrix (host-matrix.md + macOS host runs) | qa | M | 046, 048 | todo |
| 13 | 049 | Linux build bring-up + LV2 validators + Carla/Ardour smoke | infra | M | 026, 048 | todo |
| 14 | 050 | Windows build bring-up | infra | M | 049 | todo |
| 14 | 052 | README + user docs | docs | S | 046, 048, 049 | todo |
| 15 | 051 | GitHub Actions CI (deferred trigger — last) | infra | M | 026, 049, 050 | todo |

Notes:
- Waves 3–4 are the widest (up to 11 parallel tasks); DSP, plugin-shell, and UI
  streams are deliberately independent until wave 10 (editor assembly).
- 026b is intentionally placed at its earliest possible wave (right after the golden
  corpus): hardware-capture sourcing is an *early* QA task and a v1-freeze gate
  (testing-strategy.md §7 Wave 2) — do not defer it to the end.
- 045 was split (review): 045 = layout + LCD binding + field editing; 045b = soft-key
  actions, drop-in/drag-out, FX greying, accessibility. 046/047/048 hang off 045b.
- 052 sits in wave 14 because it links docs/BUILDING.md created by 049.
- 051 (CI) is intentionally last per the locked ORCHESTRATION decision; 026's golden
  set and 049/050 platform presets are its inputs. The Akaizer cross-check (026c) and
  host smoke matrix (048b) are never CI steps.
