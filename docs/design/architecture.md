# mwStime — System Architecture

Status: accepted — architecture-panel synthesis (3 proposals + adversarial critiques), 2026-06-12.
Companion documents: `docs/design/dsp-engine.md`, `docs/design/ui-design.md`,
`docs/design/testing-strategy.md`, ADRs in `plan/decisions/001`–`006`.
Evidence base: `docs/research/akaizer-analysis.md`, `docs/research/akai-manuals-specs.md`,
`docs/research/deep-research-report.md`. Locked constraints: `plan/ORCHESTRATION.md`.

Panel note: three lenses (vintage-DSP authenticity purist; product-minded boutique
designer; pragmatic senior plugin engineer) converged on the same skeleton — one pure
JUCE-free DSP core, offline render as the authenticity reference, a streaming FX
wrapper over the identical grain scheduler, data-driven per-model configuration, and
fully procedural vector UI. Panel disagreements and their resolutions are recorded in
the ADRs ("Options considered" sections).

---

## 1. Goals and constraints (from plan/ORCHESTRATION.md)

- Hybrid, **FX-first** plugin: timestretch effect on incoming/loaded audio + sample
  audition/re-pitch playback. FX mode is the default `pluginMode` and a first-class
  citizen — its semantics are fully specified (ADR-006), not an afterthought.
- **JUCE 8**, CMake + FetchContent, **AGPLv3** open source.
- **Authentic DSP core** (era-correct artifacts, per-model bit depth/filter character)
  + **modern UX** (real-time preview, host tempo sync, drag-and-drop).
- macOS + Linux required; Windows third.
- Models at v1: **S900, S950, S1000, S1100** (the locked minimum). **S3000** is a
  v1.1 fast-follow with its slot reserved in `ModelId`/faceplate data (ADR-004,
  revised per panel 2-1 in favor of deferral).
- Backlog tasks must be executable by small autonomous agents → strict module
  boundaries, a JUCE-free DSP core that builds and tests standalone, and a CLI render
  tool so DSP work never requires a host.

## 2. Top-level module layout

Three layers, strictly one-directional dependencies (lower layers never include upper):

```
+---------------------------------------------------------------+
|  plugin/            JUCE 8 plugin (VST3, AU, LV2, CLAP, app)  |
|   PluginProcessor · PluginEditor · APVTS state · EngineHost   |
+------------------------------▲--------------------------------+
                               | (only plugin/ includes JUCE)
+------------------------------+--------------------------------+
|  libs/mwstime-core/  PURE C++20 STATIC LIB — **no JUCE**       |
|                                                                |
|   mws::core     buffers, resampler, filters, quantizer, rng    |
|   mws::stretch  CyclicEngine (CLASSIC/REVISED), S950Engine,    |
|                 RepitchEngine (S900); IntellEngine (v1.1)      |
|   mws::model    ModelId, ModelSpec tables, CharacterChain      |
|   mws::engine   OfflineRenderer, RealtimeStretcher (FX),       |
|                 ParamSnapshot, TempoMap                         |
+------------------------------▲--------------------------------+
                               |
+------------------------------+--------------------------------+
|  tools/mwstime-render/   CLI: wav in → wav out, all params     |
|                          (drives golden tests; agent debugging)|
+---------------------------------------------------------------+
```

Rules:
- `libs/mwstime-core` has **zero dependencies** beyond the C++20 standard library. It
  compiles with plain `clang++`/`g++`, is unit-tested with Catch2, and is the target of
  all golden-render regression tests. This is what makes the DSP backlog tasks
  independently executable: an agent implementing `S950Engine` needs only this library,
  `docs/design/dsp-engine.md`, and the cited research — no JUCE, no host.
- The core's stretch path with `transpose = 0` and character OFF uses **integer/
  fixed-point arithmetic only** (OpenMPT precedent: integer hop, integer linear interp,
  akaizer-analysis.md §4.1). This is the only path for which cross-platform bit-exact
  golden renders are claimed; float stages (filters, SRC, transpose) are
  reference-platform-exact and tolerance-compared elsewhere (testing-strategy.md §4 —
  scoped per panel critique of the over-broad "bit-exact everywhere" claim).
- `tools/mwstime-render` links only mwstime-core plus a minimal WAV reader/writer
  (`mws::core::WavIo`, also JUCE-free; 16/24/32-int + 32-float, mono/stereo — same
  envelope as Akaizer, see akaizer-analysis.md §2.1).
- `plugin/` is the only place JUCE appears.

### 2.1 mwstime-core internal map

```
libs/mwstime-core/
  include/mws/
    core/Buffer.h            // non-owning AudioView + owning AudioBuffer (float32)
    core/WavIo.h             // for the CLI + tests only
    core/Resampler.h         // windowed-sinc & linear-interp resamplers
    core/Butterworth.h       // 6th-order Butterworth LP (3 cascaded biquads,
                             //   Q = 0.5176/0.7071/1.9319) + 3rd-order variant
    core/MovingLpf.h         // S1000-style -18 dB/oct voice filter (3rd-order
                             //   Butterworth); S3000 -12 dB/oct resonant SVF (v1.1)
    core/Quantizer.h         // bit-depth quantization (12/16-bit), TPDF dither option
    core/AutoCorr.h          // normalized autocorrelation pitch/period estimate
    stretch/CyclicEngine.h   // CLASSIC (integer hop) + REVISED (fractional hop);
                             //   splice constants (overlap F, fade shape, rounding
                             //   rule) are CONSTRUCTOR CALIBRATION PARAMETERS
    stretch/S950Engine.h     // 999% engine, D-TIME, Mon1/Pol2
    stretch/RepitchEngine.h  // S900 varispeed (no stretch)
    stretch/IntellEngine.h   // INTELL approximation — v1.1, spec retained
    model/ModelId.h          // enum: S900, S950, S1000, S1100, S3000(reserved)
    model/ModelSpec.h        // per-model param ranges/defaults + character config
    model/CharacterChain.h   // rate emu + quantize + reconstruction filter per model
    engine/OfflineRenderer.h // hardware-faithful render-to-new-sample
    engine/RealtimeStretcher.h // FX-mode streaming engine (ring buffer; ADR-006)
    engine/Params.h          // POD ParamSnapshot, validated/clamped per ModelSpec
  src/ ...mirrors include/...
```

## 3. Plugin formats at v1 (ADR-002)

| Format | Platform | v1 | Mechanism | Shell validator |
|---|---|---|---|---|
| VST3 | macOS, Linux, (Windows later) | yes | JUCE 8 (AGPLv3 build is GPL-compatible with Steinberg's GPLv3 option) | pluginval (strictness 10) |
| AU (v2) | macOS | yes | JUCE 8 — exported as **`aumf` (music effect)**, see §7 | pluginval + `auval -v aumf` |
| LV2 | Linux (primary), macOS | yes | JUCE 8 native LV2 export | **lv2lint + lv2_validate** (pluginval cannot load LV2) |
| CLAP | all | yes | `clap-juce-extensions` via FetchContent | **clap-validator** (pluginval cannot load CLAP) |
| Standalone app | all | yes (dev/debug vehicle, also the open "Akaizer replacement") | JUCE 8 | manual smoke |

pluginval validates only VST3/AU — the per-format validator column above is the
corrected QA plan (all three panel critiques flagged the original "pluginval on every
format" claim as factually wrong). AUv3 and AAX: out of scope for v1 (AAX requires a
closed SDK agreement incompatible with the fully-open AGPLv3 workflow).
clap-juce-extensions is an unofficial shim that can lag JUCE releases — its pinned tag
lives in `cmake/FetchClapExtensions.cmake` and bumping it is an explicit backlog task,
not an incidental upgrade.

## 4. Runtime architecture and threading model

Three thread domains plus one worker:

```
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ MESSAGE THREAD (JUCE)                                                   │
 │  UI · APVTS listeners · drag-and-drop · file chooser                    │
 └────────────┬───────────────────────────────▲────────────────────────────┘
   param writes (atomic, APVTS)         render-done / meter / LCD updates
              │                               │ (lock-free FIFO → timer poll)
 ┌────────────▼───────────────────────────────┴────────────────────────────┐
 │ AUDIO THREAD (processBlock)                                             │
 │  1. snapshot params (ParamSnapshot, plain loads of std::atomic)         │
 │  2. FX mode: RealtimeStretcher::process(in, out)                        │
 │     SAMPLE mode: SamplePlayer::process(out)   (plays rendered buffer;   │
 │     MIDI voice = real-time variable-rate read, see §4.2)                │
 │  3. CharacterChain::process(out)  (when not pre-baked into a render)    │
 │  NEVER: allocation, locks, file IO, ValueTree access                    │
 └────────────┬─────────────────────────────────────────────▲──────────────┘
   render request (lock-free queue)            std::shared_ptr<RenderedSample>
              │                                  swap-in (atomic exchange)
 ┌────────────▼─────────────────────────────────────────────┴──────────────┐
 │ RENDER WORKER THREAD (std::thread / juce::ThreadPool, 1 worker)         │
 │  OfflineRenderer: authentic CYCLIC/S950 render → new buffer             │
 │  (mirrors the hardware's "GO … remaining-time countdown" workflow;      │
 │   S1000 manual pp.45–47)  · progress + abort flag (hardware: hold F8)   │
 │  · output-length cap → "** NOT ENOUGH MEMORY **" LCD message (§5.1)     │
 └──────────────────────────────────────────────────────────────────────────┘
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ FILE LOADER THREAD: decode dropped/chosen audio file → SourceSample;    │
 │  publishes via shared_ptr swap; also pre-encodes the FLAC state blob    │
 │  (§6) so getStateInformation never encodes on the message thread        │
 └──────────────────────────────────────────────────────────────────────────┘
```

Ownership/publication protocol (closes the use-after-free hole flagged in panel
critique of raw-atomic-pointer swaps): all audio buffers shared across threads are
immutable once published and held by `std::shared_ptr`; the audio thread copies the
pointer once per block (RCU-style) so a buffer can never be freed mid-block. The audio
thread releases old pointers into a "graveyard" lock-free FIFO that the message thread
drains; deallocation never happens on the audio thread. This protocol is a hard
requirement on every cross-thread buffer handoff (render results, loaded samples, FX
history reconfiguration) and is ThreadSanitizer-tested (testing-strategy.md §3.6).

### 4.1 Why offline rendering exists at all (not just real-time)

The hardware timestretch is an **offline render to a new named sample** on every model
(akai-manuals-specs.md §§2–4; akaizer-analysis.md §9). The CLASSIC integer-hop schedule
("perfect pitch, bad timing", akaizer-analysis.md §2.2/§10) produces an output whose
*length is quantized* — semantics that only make sense offline. So:

- **SAMPLE mode** uses `OfflineRenderer` for bit-faithful authenticity (this is the path
  golden tests pin down).
- **FX mode** uses `RealtimeStretcher`, a streaming variant of the same cyclic core
  (precedent: potenza-time-stretch, akaizer-analysis.md §5), with the causality
  contract and bounded deviations of ADR-006 (§5.2 below). Both variants consume the
  *same* grain-scheduler code; a dedicated test asserts that with frozen parameters
  from a known start state the streamed output equals the offline render
  sample-for-sample over the comparable region (testing-strategy.md §3.4b).

### 4.2 MIDI playback voice (SAMPLE mode)

Chromatic MIDI repitch is **not** the offline transpose stage re-used — it is a
distinct real-time path (panel critique: "~50 lines" was wrong by an order of
magnitude): a variable-rate read head over the published render buffer; on S900/S950
models the per-voice virtual clock and the tracking-filter coefficients are recomputed
on note-on on the audio thread (closed-form biquad coefficients, no allocation).
v1 is **monophonic, last-note priority** (PI — bounds scope; polyphony is additive
later). This is its own backlog task with its own RT-safety tests.

## 5. Data flow

### 5.1 SAMPLE mode (authentic path)

```
 drop/choose file ──► FileLoader ──► SourceSample (float32, original SR kept)
                                          │
        user edits params, presses GO     ▼
 UI ──► render request ──► OfflineRenderer:
        ┌────────────────────────────────────────────────────────────┐
        │ [1] Ingest emulation: resample to model rate, quantize     │
        │     to model bit depth            (CharacterChain, input)  │
        │ [2] Stretch engine (per model, dsp-engine.md §3–§6)        │
        │ [3] Transpose: resample + anti-alias filter (separate      │
        │     stage, akaizer-analysis.md §2.4)                       │
        │ [4] Playback emulation: reconstruction filter per model    │
        │                                   (CharacterChain, output) │
        │ [5] OPTIONAL peak-normalize to source peak — default OFF   │
        │     (authentic: no manual documents normalization and      │
        │     OpenMPT doesn't normalize; ON reproduces Akaizer v1.3  │
        │     convenience behavior [AKZ §2.1])                       │
        └────────────────────────────────────────────────────────────┘
                                          │
                          RenderedSample ─┴─► atomic swap into SamplePlayer
                                              ─► audition via keyboard/host MIDI
                                              ─► drag-out / save as WAV
```

Render memory cap: output length is capped at **10 minutes at model rate per channel
(PI)**; a render that would exceed it is refused with an LCD message in the hardware's
own idiom ("make sure that these figures do not exceed the amount of memory available",
S1000 manual p.47, akai-manuals-specs.md §3) — the cap is authentic in spirit and
prevents the unbounded-allocation failure the panel flagged (2000% × 30 s ≈ 211 MB).

### 5.2 FX mode (real-time path) — causality contract (ADR-006)

The panel established the correct physics (two proposals had it inverted): a live
streaming stretcher at time t has received t seconds of input. **Expansion (T>100%) is
causal but the read head lags the write head without bound** (lag grows at
`1 − 100/T` per unit time). **Compression (T<100%) requires future input** — it is
impossible as a pure insert for *every* T below 100%, regardless of reported latency.
The only true zero-drift insert is T = 100% (+ transpose). The contract:

```
 host audio in ──► history ring (preallocated in prepareToPlay) ──► RealtimeStretcher
                                                        │  same two-grain cyclic core,
                                                        │  integer or fractional hop
                                                        ▼
                   CharacterChain (real-time variant) ──► host audio out

 host tempo/transport ──► TempoMap ──► SYNC window boundaries + tempo-synced T
```

| Case | FREE (no transport sync) | SYNC (WINDOW = 1/4…8 bars, default 1 bar (PI)) |
|---|---|---|
| T = 100% | pure delay of exactly the reported latency (null-testable, character OFF) | same |
| T > 100% | read head lags; lag allowed up to the history bound (30 s (PI)); on exhaustion the read head jumps to `writePos − latency` at the next grain boundary (audible resync, documented) | read head hard-resyncs to the window start at every transport-aligned window boundary — "stretch the last bar", beat-repeat semantics |
| T < 100% | **clamped to 100%** (LCD shows `FX MIN 100%`; automation values preserved, clamp applied at the engine) (PI) | allowed: the captured window plays compressed, then **silence to the window boundary** (PI; loop-fill is a possible later option) |

- No transport / Standalone: FREE rules apply; SYNC falls back to a wall-clock window
  derived from the last known tempo or 120 BPM (PI).
- **Latency**: `L = ceil(C_max,host) + crossfadeLen + SRC group delay`, where
  `C_max,host = 2000 × hostRate / modelRate` for the **current** model rate. L is
  recomputed in `prepareToPlay` and whenever model / bandwidth / FS change (all
  non-automatable parameters) and reported via `setLatencySamples`. Cycle-length
  automation does NOT change L (worst case is already covered). Note the honest cost:
  at extreme S950 bandwidth settings (7.5 kHz rate) L ≈ 12,800 samples @ 48 kHz; at
  the default max bandwidth it is ≈ 2.2 k. Hosts honor mid-session latency changes
  inconsistently — model switching is documented as a PDC-updating action.
- At T=100% with character OFF the stretcher degenerates to a pure delay of exactly L,
  so null tests against a delayed dry signal are possible.
- Stereo: dual-mono with a **shared hop schedule** → phase-coherent in CLASSIC
  (hardware precedent: stereo = separate -L/-R samples with identical settings,
  akai-manuals-specs.md §3; coherence is tested, testing-strategy.md §3.5).

## 6. State model

- **Parameters** (automatable): `juce::AudioProcessorValueTreeState`. One unified
  parameter set across models with **fixed superset ranges** (e.g. timeFactor
  25–2000% always); the active `ModelSpec` *clamps at the engine* and the LCD shows
  the clamped hardware value (e.g. 999% max while model=S950 — akai-manuals-specs.md
  §2). Ranges never change at runtime — this avoids the VST3 host parameter-info
  caching problem the panel flagged. Model, mode, FS and bandwidth-range selection are
  non-automatable. Full table in dsp-engine.md §2.
- **Non-parameter state** (ValueTree under the APVTS root):
  - `mode` (FX | SAMPLE), `sourceFile` (path + content hash), `embedAudio` (bool,
    default on for files ≤ 16 MB encoded): the FLAC blob is **pre-encoded on the file
    loader thread when the sample loads/changes and cached**; `getStateInformation`
    only memcpys the cached blob — never encodes on the message thread (host-autosave
    reality, panel critique).
  - render metadata (engine version hash, params used) so a reloaded session
    re-renders deterministically instead of storing the rendered output,
  - UI state: zoom/scale factor, LCD page.
- **Versioning**: state root carries `stateVersion`; migrations are explicit functions
  in `plugin/src/state/Migrations.cpp`.
- Determinism rule: a (sourceAudio, ParamSnapshot, engineVersion) triple always
  produces a bit-identical render **on a given platform/build** — required by the
  golden-test strategy and by re-render-on-load. Cross-platform bit-exactness is
  claimed only for the integer CLASSIC stretch path (§2 rules).

## 7. Build system

CMake ≥ 3.25, JUCE 8 + deps via FetchContent (no submodules — agent-friendly clean
clones). Top-level targets:

```cmake
# CMakeLists.txt (sketch)
project(mwStime VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
# FP discipline for the deterministic core: no -ffast-math anywhere in libs/;
# -ffp-contract=off on mwstime_core (pinned double rounding for golden renders)

include(cmake/FetchJUCE.cmake)        # JUCE 8.x pinned tag
include(cmake/FetchClapExtensions.cmake)
include(cmake/FetchCatch2.cmake)      # tests only

add_subdirectory(libs/mwstime-core)   # mwstime_core  (static, JUCE-free)
add_subdirectory(tools/mwstime-render)# mwstime_render (CLI)
add_subdirectory(plugin)              # juce_add_plugin(... FORMATS VST3 AU LV2 Standalone)
                                      # + clap_juce_extensions_plugin(...)
add_subdirectory(tests)               # ctest: unit + golden
```

`plugin/CMakeLists.txt` uses `juce_add_plugin` with
`COMPANY_NAME mattWoolly`, `PLUGIN_MANUFACTURER_CODE MwSt`, `PLUGIN_CODE MwS1`,
`FORMATS AU VST3 LV2 Standalone`, `IS_SYNTH FALSE`, `NEEDS_MIDI_INPUT TRUE`.
**Plugin category note (panel critique — hybrid MIDI+FX is a format minefield):** with
`IS_SYNTH FALSE` + MIDI input, JUCE exports the AU as `aumf`
(kAudioUnitType_MusicEffect) so Logic routes MIDI to it; VST3 is categorized Fx with a
MIDI input bus. Hosts that refuse MIDI to effect slots degrade gracefully — audition
is always available from the UI (PLAY soft key / waveform click), MIDI is an
enhancement, never the only trigger path. `auval` must be invoked as
`auval -v aumf MwS1 MwSt`. CI is deferred per ORCHESTRATION.md; local verification is
`cmake --preset default && cmake --build --preset default && ctest --preset default`
(presets committed in `CMakePresets.json`).

## 8. Directory tree

```
mwStime/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
│   ├── FetchJUCE.cmake
│   ├── FetchClapExtensions.cmake
│   └── FetchCatch2.cmake
├── libs/
│   └── mwstime-core/
│       ├── CMakeLists.txt
│       ├── include/mws/...        (see §2.1)
│       └── src/...
├── plugin/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── PluginProcessor.{h,cpp}     # APVTS, threading, EngineHost
│   │   ├── EngineHost.{h,cpp}          # owns RealtimeStretcher/OfflineRenderer glue
│   │   ├── MidiVoice.{h,cpp}           # §4.2 real-time repitch voice
│   │   ├── state/{Parameters,Migrations,StateBlobCache}.{h,cpp}
│   │   └── PluginEditor.{h,cpp}
│   └── ui/                             # see ui-design.md
│       ├── Faceplate.{h,cpp}  LcdDisplay.{h,cpp}  SoftKeyBar.{h,cpp}
│       ├── JogWheel.{h,cpp}   ModelSelector.{h,cpp}
│       └── lookandfeel/SeriesLookAndFeel.{h,cpp}  FaceplateSpec.h
├── tools/
│   └── mwstime-render/                 # CLI golden-render driver
│       ├── CMakeLists.txt
│       └── main.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── unit/                           # Catch2, per mws:: module
│   ├── golden/
│   │   ├── inputs/                     # tiny deterministic WAVs (committed,
│   │   │                               #   original/synthetic — NO copyrighted breaks)
│   │   ├── blessed/                    # blessed golden renders (committed)
│   │   └── cases.json                  # param matrix per test case
│   └── plugin/                         # pluginval/clap-validator/lv2lint scripts,
│                                       #   state round-trip tests
├── docs/{research,design}/ ...
└── plan/{ORCHESTRATION.md,backlog/,decisions/}
```

## 9. Authenticity vs modern-UX boundary (summary)

| Concern | Authentic (research-pinned) | Modern (pragmatic) |
|---|---|---|
| Stretch math, per model | OfflineRenderer engines, dsp-engine.md | RealtimeStretcher deviations (ADR-006) |
| Parameter names/units/ranges on LCD | hardware units (%, samples, 01–99) | normalized host automation values |
| Render-to-new-sample workflow, GO/abort/progress/memory cap | kept | plus instant preview while editing |
| Output level | no normalization (hardware/OpenMPT behavior); models gain-matched (no per-model trim theater — the -3 dBv/-5 dBm spec difference mixes units and would break A/B nulls, panel critique) | NORM opt-in (Akaizer v1.3 behavior); OUTPUT trim |
| Mono engines (S950) | engine processes mono sum (authentic) | LCD states it; stereo-bypass documented |
| Character chain | per-model bit depth, rates, filters | global CHARACTER on/off bypass |
| Tempo sync, drag-and-drop, resizable UI, FX mode itself | — | provided |

Every numeric DSP value is specified (with citation or "pragmatic invention" label) in
`docs/design/dsp-engine.md`. UI specifics live in `docs/design/ui-design.md`.

## 10. Top risks (panel-consolidated)

1. **The splice fine structure is unknown and it IS the sound.** Crossfade shape,
   overlap fraction, and the exact step formula are "not recoverable without
   disassembly" (akaizer-analysis.md §2.4). Mitigation: all three are constructor
   calibration parameters of `CyclicEngine` (data-only recalibration); Akaizer CLASSIC
   renders are a *secondary, local-only* cross-check (its "near-exact" fidelity claim
   was refuted 0-3 — deep-research-report.md Finding 6/Refuted), valid only for
   T 120–2000%; **real hardware captures are the primary oracle and gate any
   "authentic" labeling** — sourcing S950/S1100 renders of the test corpus is an early
   QA task, with calibration/validation fixture sets kept disjoint (no overfitting).
2. **FX-mode causality and latency semantics.** Solved by contract before code
   (§5.2/ADR-006); the contract is itself host-tested (latency reporting, transport
   edge cases, tempo changes — testing-strategy.md §6).
3. **Variable-clock reconstruction (S900/S950 chain) is the heaviest and least
   oracle-backed DSP.** RX950 *cannot* validate clock-tracking (it lacks exactly that,
   Finding 7). Mitigation: early prototype backlog task; internal oversampling for ZOH
   imaging (dsp-engine.md §8.1); property tests against the analytic Butterworth
   response; hardware capture A/B when available.
4. **Scope vs atomic-agent execution.** Staged out of v1: INTELL (unverified
   internals), S3000 (data+filter later), MIDI polyphony, clock-bleed whine, TIME
   SKEW. The v1 surface decomposes into single-agent tasks (dsp-engine.md §10).
