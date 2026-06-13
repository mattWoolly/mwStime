# (PI)-constant audit

Status: accepted, 2026-06-13 (task plan/backlog/034b-dsp-adversarial-qa.md).

This is the testing-strategy.md §7 exit-criterion ledger: **"every (PI) constant
either confirmed or ticketed."** A **(PI)** tag (dsp-engine.md §0 marking
convention: "pragmatic invention" — a number that is *not* cited to a manual or
research finding) is a deliberate choice that the adversarial QA fleet may retune.
This file sweeps every **(PI)** tag in `docs/design/` and in code comments, and
maps each to one of:

- **Confirmed** — the value is pinned by a unit/property test and carries a tuning
  note explaining the choice and how to retune it (the standing disposition for a
  value that works and has a documented rationale); or
- **Ticketed** — the value needs hardware-capture calibration or a follow-up
  decision; the open issue is the backlog task that will resolve it.

It does **not** retune anything (calibration is Wave 2, tasks 026b/026c). It is
the audit that lets Wave 1 exit.

## How this list was produced

```
grep -rn '(PI)' docs/design/
grep -rn '(PI)' libs/ plugin/ tools/ tests/   # code comments
```

Tags that are *meta* (describing the convention itself, not naming a constant)
are excluded: dsp-engine.md §0 ("every number is either cited or tagged (PI)"),
testing-strategy.md §7 / §7 exit criteria (the two requirements this file
satisfies), the AutoCycle.h / S950Engine.h "named-constants block" header
comments, and the test-file "QA retuning hook" comments
(test_autocycle.cpp / test_s950.cpp / test_varclock.cpp).

---

## DSP / engine constants

| # | Constant | Source | Value | Disposition | Tuning note / issue |
|---|---|---|---|---|---|
| 1 | Crossfade fraction `F` | dsp-engine.md §3.1; CyclicEngine.h `SpliceCal` | 0.20 | **Confirmed** | Crossfade over the last 20% of each grain — matches the OpenMPT reference [AKZ §4.1]. Pinned by the CLASSIC property suite (test_cyclic_properties.cpp / test_cyclic_classic.cpp). Re-tune only via the `SpliceCal` calibration ADR (testing-strategy §4 blessing rule). |
| 2 | Reads past `N-1` return 0 (no wrap) | dsp-engine.md §3.4 | — | **Confirmed** | Edge rule, not a magnitude; pinned by the offline-renderer / cyclic tests (a grain reading past source end zero-fills, never wraps). No calibration dependency. |
| 3 | Render memory cap | dsp-engine.md §3.4, §3.5; architecture.md §5.1; OfflineRenderer.h | 10 min @ model rate / channel (600 s) | **Confirmed** | `OfflineRenderer::kMaxRenderSeconds = 600.0`. Pinned by test_offline_renderer.cpp and now by `qa-adversarial: render cap refuses 2000% …` (predicted length > cap, typed `NotEnoughMemory`, no over-cap allocation). The hardware idiom "** NOT ENOUGH MEMORY **" is cited [MAN §3 p.47]; the 10-min figure itself is the (PI). |
| 4 | FX history ring length | dsp-engine.md §3.5; architecture.md §5.2; RealtimeStretcher.h `kHistorySeconds`; FxEngine | 30 s | **Confirmed** | `RealtimeStretcher::kHistorySeconds = 30.0`. Bounds the FREE-mode read-head lag; on exhaustion the read head jumps to `writePos − latency` at the next grain boundary. Pinned by test_rtstretch_free.cpp (history-exhaustion resync) and exercised flat over 30 min by `qa-adversarial: FX 30-minute soak`. Retune only if a use case needs longer FREE lag (memory cost = 30 s × maxModelRate × channels). |
| 5 | SYNC no-transport fallback tempo | dsp-engine.md §3.5; architecture.md §5.2; RealtimeStretcher.cpp; TempoMap.h | 120 BPM | **Confirmed** | Last-known host tempo, else 120 BPM (Standalone has no bar grid). Pinned by test_tempomap.cpp (`lastKnownBpm <= 0 ⇒ 120`) and test_rtstretch_sync.cpp. Conventional default; no hardware analogue (modern-UX SYNC). |
| 6 | SYNC T<100% "silence to window boundary" | dsp-engine.md §3.5; architecture.md §5.2 | — | **Confirmed** | Edge behavior (the captured window plays compressed, then silence). Pinned by test_rtstretch_sync.cpp. "Loop-fill is a possible later option" — see issue **T-1** below if revisited. |
| 7 | FX FREE T<100% clamp to 100% | dsp-engine.md §2 (timeFactor), §3.5; architecture.md §5.2; ModelSpec::clamp | 100% floor | **Confirmed** | ADR-006 causality clamp (`FX MIN 100%`); a pure insert cannot read future input. Pinned by test_rtstretch_free.cpp + test_enginehost_fx_reconfig.cpp; exercised by `qa-adversarial: FX path stays finite …` (T=300% FREE). |
| 8 | INTELL `decisionInterval = max(1, round(100/qual))` | dsp-engine.md §4 | — | **Ticketed** | INTELL is **deferred to v1.1** (ADR-001; internals unverified, [DRR caveat 1]). No code, no test at v1. Issue: revisit when INTELL ships — the §4 spec is retained but unimplemented. Tracked by the §4 "v1.1, spec retained" status; no v1 backlog task. |
| 9 | INTELL `searchRange = C·[0.5..2.0]` | dsp-engine.md §4 | — | **Ticketed** | Same as #8 — INTELL v1.1, no v1 surface. |
| 10 | INTELL `candidates = 8 + qual` | dsp-engine.md §4 | — | **Ticketed** | Same as #8 — INTELL v1.1, no v1 surface. |
| 11 | S950 AUTO-D algorithm | dsp-engine.md §5, §7.1; S950Engine.h | (see #16–#19) | **Ticketed** | The AUTO-D *feature* is documented [MAN §2]; the *algorithm* (autocorrelation cycle detection) is ours. Confirmed-by-test for determinism (test_s950.cpp) but the mapping to real S950 behavior is **deviation-until-calibrated** — hardware-capture validation is task **026b**. |
| 12 | S950 POL2 = fixed C exactly as set | dsp-engine.md §5; S950Engine.h | — | **Ticketed** | The "better suited to drum loops" framing is cited [MAN §2]; that POL2 ≡ a fixed-cycle CyclicEngine is our behavioral mapping. Pinned by test_s950.cpp (POL2 degenerates to CyclicEngine); semantic fidelity is task **026b**. |
| 13 | S950 D-TIME ≡ cycle length in samples | dsp-engine.md §2 (cycleLen row), §5; S950Engine.cpp | 1:1 mapping | **Ticketed** | The single D-TIME→C mapping. **Deviation-until-calibrated** — the manual gives D-TIME no sample mapping. Calibration tool is `tools/calibrate/` (the (PI) mapping under test); hardware-capture validation is task **026b**; the D-TIME mapping is a v1-freeze gate (testing-strategy §7 Wave 2). |
| 14 | Transpose resampler: 16-tap Kaiser(β=8) windowed-sinc | dsp-engine.md §7.2; Resampler.h | 16 taps, β=8 | **Confirmed** | A quality stand-in for the era hardware's pitch path. Pinned by test_resampler.cpp (impulse/sine SNR, identity at ratio 1.0, reported group delay) and test_transpose.cpp. Quality knob, not an authenticity claim; retune taps/β if SNR targets change. |
| 15 | 12-bit quantize: mid-tread, no dither | dsp-engine.md §8.1; VarClockChain.h/.cpp | no dither | **Confirmed** | Authentic: the S900/S950 converters did not dither at ingest. Pinned by test_quantizer.cpp (exact 1/2048 step, idempotent, ≤4096 distinct values) and test_varclock.cpp. The *no-dither* choice is the (PI); the 12-bit depth is cited [DRR F4]. |
| 16 | VarClock oversample rate (4× host default) | dsp-engine.md §8.1; VarClockChain.h/.cpp | max(4×host, 2×clock) | **Confirmed** | Internal oversample so ZOH images are represented before the Butterworth, then decimated. Pinned by test_varclock.cpp (ZOH images present pre-filter, ≥24 dB attenuated post-Butterworth). Raise the factor only if alias products are found above the spec floor. |
| 17 | AutoCycle `kAnalysisWindowSeconds` (~200 ms) | dsp-engine.md §7.1; AutoCycle.h | first 200 ms | **Confirmed** | Analysis window for autC; pure inference (the manual says only the feature is "not always infallible"). Pinned by test_autocycle.cpp. Tuning note in AutoCycle.h: enlarge if detection is jittery on slow-onset material. |
| 18 | AutoCycle `kPeakThreshold` | dsp-engine.md §7.1; AutoCycle.h | 0.3 | **Confirmed** | Min normalized-autocorrelation peak to accept a content-derived cycle (rejects white noise ~0.05, accepts pitched ~0.9). Pinned by test_autocycle.cpp. Tuning note: lower if hardware autC proposes cycles on noisier material, raise if it falls back more readily. |
| 19 | AutoCycle `kFallbackCycleLen` (= default 1000) | dsp-engine.md §7.1; AutoCycle.h | 1000 | **Confirmed** | The *value* 1000 is the cited hardware default CYCLE [AKZ §2.1, MAN §5, DRR F10]; its *use as the no-detection fallback* is the (PI). Pinned by test_autocycle.cpp. Verify against hardware autC-on-noise (task 026b). |
| 20 | S950 `kMon1SearchFraction` | dsp-engine.md §5; S950Engine.h | 0.5 | **Ticketed** | MON1 snap half-width as a fraction of C — pure inference, no manual documents MON1 internals. Pinned for determinism (test_s950.cpp) but the value is **retune-against-hardware** (task 026b, Wave 2). |
| 21 | S950 `kMon1PeakThreshold` | dsp-engine.md §5; S950Engine.h | 0.3 | **Confirmed** | Same confidence floor as #18 (rejects noise, accepts pitched); below it a grain keeps the previous C (no snap thrash on silence). Pinned by test_s950.cpp. |
| 22 | S950 `kMon1WindowCycles` | dsp-engine.md §5; S950Engine.h | 4 | **Confirmed** | Per-grain analysis window in multiples of C — 4 cycles gives >2 periods of overlap at the largest searched lag while staying local in time. Pinned by test_s950.cpp. |
| 23 | S1100 output dither: fixed internal seed | dsp-engine.md §8.2; CharacterChain.cpp | fixed seed | **Confirmed** | Determinism requirement (architecture.md §6): the seeded 20-bit-DAC dither delta must render bit-identically. Pinned by the determinism property (testing-strategy §3 item 6) + golden renders. The *seed* is the (PI); the dither *existence* is the S1100 character delta. |
| 24 | S900/S950 mono-sum: equal-weight average | dsp-engine.md §5; CharacterChain.cpp | 0.5/0.5 | **Confirmed** | The S900/S950 are mono machines [MAN §2]; equal-weight sum preserves level for correlated material. Pinned by golden stereo→mono cases (e.g. `s900_stereo_monosum`, `s950_stereo_monosum_200`) and test_characterchain.cpp. |
| 25 | S3000 chain: 16-bit quantize at ingest only | dsp-engine.md §8.3 | — | **Ticketed** | S3000 is a **reserved v1.1 slot** (ADR-004): id + faceplate slot, NO behavior at v1. No code path, no test. Revisit when S3000 ships. |
| 26 | `outTrim` range −24…+12 dB | dsp-engine.md §2; ModelSpec.h `kOutTrimMin/MaxDb` | −24, +12 | **Confirmed** | Modern-UX output trim (no per-model level offset — the spec difference is documented, not modeled, to keep null tests exact). Pinned by test_parameters.cpp + the FX/SAMPLE null tests (0 dB short-circuits to bit-exact). Range is a UX choice; widen freely. |

## Plugin / threading constants

| # | Constant | Source | Value | Disposition | Tuning note / issue |
|---|---|---|---|---|---|
| 27 | EngineHost request/event FIFO capacities | EngineHost.h `kRequestCapacity`/`kEventCapacity` | 8 / 1024 | **Confirmed** | Request FIFO shallow (latest-wins coalescing); event FIFO absorbs a full render's progress posts between 30 Hz UI polls. Pinned by test_enginehost.cpp / test_publication.cpp (no event loss). Raise event capacity if a longer render overflows it. |
| 28 | FileLoader UI-feedback FIFO capacity | FileLoader.h | (PI) | **Confirmed** | A load posts at most Started + Finished; pinned by test_fileloader.cpp. |
| 29 | StateTree extra field(s) beyond §6 | StateTree.h | — | **Confirmed** | A documented schema extension owned by the state layer; pinned by test_state.cpp / test_state_embed.cpp round-trips + migration tests. |

## The three flagged late additions (testing-strategy §7 Wave 1 callout)

These were added after the design freeze and are explicitly named in this task as
must-audit:

| # | Constant | Source | Value | Disposition | Tuning note / issue |
|---|---|---|---|---|---|
| 30 | **034 mode-switch fade** | EngineHost.h `kModeSwitchFadeBlocks`; task 034 | 1 block | **Ticketed** | **Invention beyond spec.** ui-design.md §6.5 specifies a one-block crossfade only for **MODEL** switches; applying it to **MODE** (FX↔SAMPLE) switches is ours. The header comment flags it for this audit. The fade *length* (one block) matches the model-switch precedent and is pinned where wired; the *decision to fade mode switches at all* needs a product/owner sign-off. Issue: **PI-MODEFADE** — confirm or remove the FX↔SAMPLE crossfade in the editor-interactions / model-switching work (tasks 045b / 046); if removed, drop `kModeSwitchFadeBlocks`. |
| 31 | **035 ~5 ms declick** | task 035 (`035-midi-repitch-voice.md`) | ~5 ms | **Ticketed** | The MIDI-repitch voice's short declick fade on note steal / note-off (PI, ~5 ms). Task 035 is **not yet implemented** (depends on the v1 MIDI voice). Issue: confirm the 5 ms value against audible click suppression when 035 lands (add a click-energy test); no hardware analogue — purely anti-click. Tracked by task **035**. |
| 32 | **036 16-bit export default** | task 036 (`036-drag-out-export.md`) | 16-bit default | **Ticketed** | Drag-out export depth policy: 16-bit default, 24-bit per source depth (PI). Task 036 is **not yet implemented**. Issue: confirm 16-bit is the right *default* (vs following source depth) when 036 lands — it is a UX/file-size tradeoff, not an authenticity constraint; the export round-trips through `mws::core::WavIo` either way. Tracked by task **036**. |

## UI / cosmetic constants (confirmed as a class)

UI palette, glow colors, faceplate geometry, jog-wheel gains, LCD grid size,
hold-to-abort time, and look-and-feel insets are all **(PI)** by construction
(ui-design.md §1/§2 mockup-derived) and are **Confirmed as a class**: they are
stylistic inventions evoking period hardware, carry no authenticity claim, and
are pinned where behavioral (e.g. the 600 ms hold-F8 abort gesture is pinned by
test_inputcluster.cpp; the 40×6 LCD grid + page formatting by
test_lcdfont.cpp / test_lcdpagemodel.cpp). Individually enumerated tags:

- ui-design.md §2 palette table; §6.5 LCD grid 40×6; LcdDisplay.h grid; LcdFont.h
  glyph shapes; FaceplateSpec.h / FaceplateGeometry.h (chassis, glow inks);
  JogWheel.cpp drag/wheel gains; SeriesLookAndFeel.cpp cap/fader insets;
  Faceplate.cpp sheen/slot cosmetics; ModelSelector 150 ms faceplate cross-fade;
  LcdPageModel.cpp hardware-idiom wording (`** WRONG DISK **`-flavored) and the
  appended bandwidth field deviation; SoftKeyBar.h / ui-design.md §6.3 hold-F8
  ≥ 600 ms abort.

Disposition: **Confirmed** — cosmetic/UX, no calibration dependency. Behavioral
ones are test-pinned (see above). They are listed so the sweep is exhaustive, not
because any needs a hardware oracle.

## Open issues (the "ticketed" half)

The constants whose disposition is **Ticketed** resolve through existing backlog
tasks / a freeze gate — none is left dangling:

| Issue | Covers (rows) | Resolved by |
|---|---|---|
| Hardware-capture calibration | 11, 12, 13, 20 (S950 AUTO-D / POL2 / D-TIME / MON1) + the autC values 17–19 | task **026b** (hardware captures = primary oracle), with `tools/calibrate/`; D-TIME mapping is a v1-freeze gate |
| Akaizer secondary cross-check | corroborates 1, 13 (CLASSIC schedule / D-TIME) | task **026c** (local-only, never a gate — see docs/qa/akaizer-crosscheck.md) |
| INTELL v1.1 | 8, 9, 10 | deferred (ADR-001); §4 spec retained, no v1 task |
| S3000 v1.1 | 25 | reserved slot (ADR-004); no v1 task |
| T-1 SYNC loop-fill | 6 | optional future enhancement (architecture.md §5.2 note); no task at v1 |
| **PI-MODEFADE** | 30 | confirm/remove FX↔SAMPLE crossfade in tasks **045b / 046** |
| MIDI declick 5 ms | 31 | task **035** |
| Export 16-bit default | 32 | task **036** |

## Exit-criterion statement

Every **(PI)** constant in `docs/design/` and in code comments is accounted for
above: each is either **Confirmed** (test-pinned with a tuning note) or
**Ticketed** (mapped to an open backlog task / freeze gate). This satisfies
testing-strategy.md §7 Wave 1 / exit criteria: *"every (PI) constant either
confirmed or ticketed."*
