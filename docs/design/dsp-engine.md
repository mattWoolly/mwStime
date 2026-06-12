# mwStime — DSP Engine Specification

Status: accepted (panel synthesis), 2026-06-12. Implements ADR-001, ADR-003, ADR-004,
ADR-006.

This document is self-contained for implementation. Citations:
- [AKZ] `docs/research/akaizer-analysis.md` (algorithm reverse-engineering, pseudocode)
- [MAN] `docs/research/akai-manuals-specs.md` (hardware parameters & signal paths)
- [DRR] `docs/research/deep-research-report.md` (verified/refuted findings)

Marking convention: every number is either cited or tagged **(PI)** = pragmatic
invention (chosen by us; must be tunable; QA may revise after A/B against hardware
renders). Inferred hardware mappings additionally carry **deviation-until-calibrated**
status per the ORCHESTRATION rule that DSP claims trace to research or an ADR.

---

## 1. Engine inventory

| Model | Ships | Stretch engine | Stretch range | Character chain | Source |
|---|---|---|---|---|---|
| S900 (1986) | v1 | **RepitchEngine** — no timestretch on hardware [MAN §1] | n/a (rate = pitch) | 12-bit, variable clock 7.5–40 kHz, tracking Butterworth LPF | [MAN §1, §7][DRR F4] |
| S950 (1988) | v1 | **S950Engine** — STRETCH page 14, D-TIME, Mon1/Pol2 | up to 999% [MAN §2] | 12-bit/16-bit processing, variable clock 7.5–48 kHz, tracking Butterworth LPF | [MAN §2][DRR F4] |
| S1000 (1988) | v1 | **CyclicEngine** (CYCLIC); INTELL deferred to v1.1 (§4) | 25–2000% [MAN §3] | 16-bit, fixed 44.1/22.05 kHz, fixed-rate interp playback; voice LPF -18 dB/oct (default open) | [MAN §3][DRR F5] |
| S1100 (1990) | v1 | same engine as S1000 (manual text verbatim identical [MAN §4]) | 25–2000% | as S1000; 20-bit DAC = slightly lower noise floor (PI dither model) | [MAN §4] |
| S3000 (1992) | **v1.1** (ADR-004) | same engines, S3000 defaults | 25–2000%, default 100% [MAN §5] | 16-bit ADC 64×OS, 28-bit accumulation, -12 dB/oct **resonant** moving LPF | [MAN §5] |

All engines process **mono float32** (stretch path additionally integer-clean, §3.2);
stereo input runs two linked instances with an **identical, shared hop schedule**
(hardware precedent: S1000 stereo = separate -L/-R samples [MAN §3]; identical CYCLIC
settings keep the sides phase-coherent — this coherence is a tested property).
S900/S950 modes sum stereo input to mono first (the S950 is a mono machine [MAN §2]) —
authentic, stated on the LCD.

**INTELL at v1**: not shipped. Internals are entirely unverified [DRR Caveats #1]; two
of three panel lenses (purist, engineer) refused to ship a guess under an authentic
name, and the critique of the third flagged it as the obvious stage-2 cut. The v1
faceplate shows `qual`/`width` greyed with "INTELL only" exactly as the hardware
manuals scope them [MAN §3 p.47] — the UI itself documents the gap. Spec retained in
§4 for v1.1.

---

## 2. Unified parameter table (plugin-level)

One APVTS parameter set with **fixed superset ranges** (never remapped at runtime —
VST3 hosts cache parameter info; panel critique). The active `ModelSpec` clamps at the
engine and the LCD displays the clamped hardware value. "Hardware unit" is what the
LCD shows. Non-automatable: `model`, `pluginMode`, `sampleRateSel`, `embedAudio`.

| ID | Name (LCD) | Range | Default | Unit | Step | Applies to | Source |
|---|---|---|---|---|---|---|---|
| `model` | MODEL | S900/S950/S1000/S1100 (+S3000 v1.1) | S1000 | — | — | — | locked + ADR-004 |
| `pluginMode` | MODE | FX / SAMPLE | FX | — | — | — | locked (FX-first) |
| `timeFactor` | TIME FACTOR | 25.00–2000.00 | 100.00 | % of original length | int in CLASSIC, 0.01 in REVISED | all stretch engines | [AKZ §2.1][MAN §3]; S950 engine-clamps high end to 999 [MAN §2]; FX FREE mode engine-clamps low end to 100 (ADR-006); default 100 [MAN §5 example screen] |
| `cycleLen` | CYCLE LENGTH (S1000/1100) / D-TIME (S950) | 20–2000 | 1000 | **samples** at model rate | 1 | CYCLIC, S950 | units "in samples" [MAN §3 p.46]; the 20–2000 numeric range is **Akaizer's convention** [AKZ §2.1], adopted as a deviation (the manuals state no range — [DRR Caveats #2]; ADR-001); default 1000 = Akaizer default [AKZ §2.1] + S3000 printed screen [MAN §5] + SPOD jungle setting [DRR F10], three independent confirmations; S950 D-TIME shares this param **(PI: D-TIME ≡ cycle length in samples; deviation-until-calibrated, §5)** |
| `stretchMode` | STRETCH MODE | CYCLIC / INTELL | CYCLIC | — | — | S1000/S1100 (INTELL greyed until v1.1) | [MAN §3 p.45] |
| `hopMode` | TIMING | CLASSIC / REVISED | CLASSIC | — | — | CYCLIC & S950 | [AKZ §2.2]; CLASSIC = hardware-faithful |
| `transpose` | TRANSPOSE | -24.00–+24.00 | 0 | semitones (1-cent steps) | 0.01 | all | hardware ±2 oct, 1-cent steps [MAN §3 spec p.81]; Akaizer ±24 [AKZ §2.1] |
| `qual` | QUAL | 1–99 | 10 | decisions index | 1 | INTELL only (v1.1; greyed at v1) | range [MAN §3 p.47]; default 10 [MAN §5 screen] |
| `width` | WIDTH | 1–99 | 10 | crossfade index | 1 | INTELL only (v1.1; greyed at v1) | range [MAN §3 p.47]; default 10 [MAN §5 screen]; semantics: "crossfade between the original and the inserted data" [MAN §5] |
| `material` | MON1/POL2 | MON1 / POL2 | POL2 | — | — | S950 only | switch documented [MAN §2 p.30]; POL2 default because breaks/loops are the primary use **(PI)**; behavioral mapping is invention, §5 |
| `autoCycle` | autC / AUTO-D | trigger | — | — | — | CYCLIC & S950 | feature documented [MAN §2, §3 p.46]; manual says only "applies software logic … like autolooping" — our autocorrelation detector is an inference **(PI, §7.1)** |
| `bandwidth` | BANDWIDTH | 3.0–19.2 (S950) / 3.0–16.0 (S900) | max | kHz | 0.1 | S900/S950 | S950 spec: bandwidth 3–19.2 kHz, rate 7.5–48 kHz continuously variable [MAN §2 p.74]; rate = 2.5 × bandwidth [DRR F4]; S900 rate ceiling 40 kHz [MAN §1] → BW ≤ 16.0. Non-automatable in FX mode (changes reported latency, §7.4) |
| `sampleRateSel` | FS | 44.1 / 22.05 | 44.1 | kHz | — | S1000/S1100 | [MAN §3 p.81] |
| `character` | CHARACTER | ON / OFF | ON | — | — | all | modern-UX bypass of §8 chain **(PI)** |
| `norm` | NORM | OFF / ON | **OFF** | — | — | SAMPLE renders | OFF is authentic (no manual documents normalization; OpenMPT does not normalize [AKZ §4.1]); ON = Akaizer v1.3 convenience behavior [AKZ §2.1] **(PI default; panel-revised from ON)** |
| `tempoSync` | SYNC | OFF / HOST | OFF | — | — | FX mode | modern UX (locked). **timeFactor := 100 × sourceBPM / hostBPM** (e.g. 174 BPM loop into an 87 BPM host ⇒ 200%, longer). *Direction corrected per panel critique — hostBPM/sourceBPM is inverted.* In CLASSIC the factor quantizes to integer % (LCD hint shows achieved length); REVISED gives sample-exact sync **(PI)** |
| `fxWindow` | WINDOW | 1/4, 1/2, 1, 2, 4, 8 bars / FREE | 1 bar | bars | — | FX mode | capture/resync window, ADR-006 **(PI)** |
| `outTrim` | OUTPUT | -24–+12 | 0 | dB | 0.1 | all | **(PI)**. No per-model trim offsets: the S1000 -3 dBv vs S1100 -5 dBm specs [MAN §8.7] mix reference units and a baked-in level delta would confound A/B and break null tests (panel critique); the spec difference is documented, not modeled |

Plugin parameters are exposed to the host normalized 0–1 with the hardware-unit
`String` conversions above.

---

## 3. The shared cyclic core (`mws::stretch::CyclicEngine`)

This implements the S1000/S1100 CYCLIC mode ("a fixed interpolation rate is
maintained throughout the whole of the sample", [MAN §3 p.45]) and is the base for the
S950 engine.

### 3.0 Scheduling-model decision (panel-resolved)

The research contains **two distinct scheduling models** that must not be conflated
(critique P1 of the engineer proposal — plugging one model's hop formula into the
other's scheduler yields a ~20% stretch-ratio error):

- **[AKZ §4.2] OpenMPT two-grain overlap scheduler**: grains overlap by fraction `F`;
  output hop = `C·(1−F)`; input hop = `round(C·(1−F) / T)`.
- **[AKZ §10] Akaizer-CLASSIC splice model**: full cycles appended with a seam fade;
  output hop ≈ `C`; step = `round(C·100/T)`.

These are the same family parameterized by the overlap fraction (§10 is the F→0
seam-only limit). **We adopt the [AKZ §4.2] scheduler as the reference implementation**
— it is the only fully specified open arithmetic with line-cited source — and make
`F`, the crossfade shape, and the hop rounding rule **constructor calibration
parameters**, so a later hardware-capture calibration (or a move toward the §10
parameterization) is data-only, never a re-architecture. Which parameterization the
real firmware/Akaizer uses is unknown [AKZ §2.4]; this choice is recorded in ADR-001.
All derived quantities (output length, splice-comb frequency) and all property tests
are computed from THIS scheduler — never from the §10 formula (panel critique: mixing
the two makes the length test fail by design).

### 3.1 Definitions

```
C        cycle length, samples at model rate (20–2000), fixed for the whole render
T        time factor as ratio = timeFactor / 100      (0.25 .. 20.0)
F        overlap fraction of each cycle used for crossfade   — CALIBRATION PARAM
hop_out  output spacing of grain launches = C * (1 - F)
hop_in   input advance per grain launch
```

`F = 0.20` default **(PI)** — matches the OpenMPT reference (crossfade over the last
20% of each grain, [AKZ §4.1]); open implementations span 20% (OpenMPT) to 40%
(potenza) to 50% (nanoTracker) [AKZ §8] and Akaizer's true value is unrecoverable
without disassembly [AKZ §2.4]. The refuted "40 ms / 20 ms ballpark" claim must NOT be
used [DRR refuted #2]. Calibration parameters live in one struct
(`CyclicEngine::SpliceCal { float overlapF; FadeShape shape; HopRounding rounding; }`)
so QA can retune against hardware captures without touching engine code.

### 3.2 Hop arithmetic — CLASSIC vs REVISED [AKZ §2.2–2.4, §10]

```
CLASSIC:  hop_in = round(hop_out / T)        // INTEGER source samples.
          timeFactor coerced to integer %.   // "Akai samplers don't use decimal
                                             //  values for Time Factor" [AKZ §2.1]
REVISED:  hop_in = hop_out / T               // fractional (double), exact timing,
                                             // sub-cycle pitch drift [AKZ §2.2]
```

CLASSIC properties that MUST hold (these are tested):
- With `transpose == 0`, every output sample is a **verbatim copy of an input sample
  at an integer offset, or (inside a crossfade region only) the rounded convex
  combination of exactly two such samples** — stretch alone never resamples
  ("perfect pitch", [AKZ §2.4, §4.1 'Always start from integer position']). The
  crossfade exemption is part of the property statement (panel critique: a blanket
  "verbatim, zero interpolation" claim is false inside every fade, [AKZ §4.1
  lines 329–343]).
- Realized stretch ratio = `hop_out / hop_in` — quantized by the integer rounding,
  NOT exactly `T` ("bad timing", [AKZ §2.2]). Output length is derived from this
  schedule (§3.4), never from `round(N·T)`.
- The integer stretch path (transpose 0, character OFF) uses integer/fixed-point
  arithmetic only (32.32 positions, 15-bit fraction lerp — OpenMPT [AKZ §4.1]) ⇒
  cross-platform bit-exact golden renders for exactly this path (architecture.md §2).

### 3.3 Crossfade

**Linear**, complementary (`fade` and `1-fade`) — every faithful open implementation
uses linear, and the -6 dB midpoint dip on uncorrelated material is part of the
characteristic "flutter" [AKZ §3 property 3, §4.1, §8 table]. Shape is a calibration
parameter (§3.1) defaulting to linear; no equal-power option in the authentic engines.

### 3.4 Reference pseudocode (offline, mono)

Implements the [AKZ §4.2] scheduler verbatim (transpose handled separately, §7.2):

```text
INPUT : src[0..N), C (samples), T (ratio), hopMode
CONST : F = 0.20 (SpliceCal)                # overlap fraction
ovStart = C * (1 - F)                       # grain phase where crossfade begins
hop_out = ovStart
hop_in  = (hopMode == CLASSIC) ? round(hop_out / T) : hop_out / T
require hop_in >= 1 (CLASSIC) ; clamp C to N if file shorter [AKZ §2.1, v1.6 behavior]

A.off = 0 ; A.pos = 0 ; B = inactive ; out = []

loop:
    sA = src[A.off + A.pos]                            # verbatim read
    if B.active:
        fade = (A.pos - ovStart) / (C - ovStart)       # linear 0..1 over last F of grain
        sB   = src[B.off + B.pos]
        emit  sA + (sB - sA) * fade                    # integer path: rounded lerp
    else:
        emit  sA
    A.pos += 1 ; if B.active: B.pos += 1
    if not B.active and A.pos >= ovStart:
        B.off = A.off + hop_in                         # CLASSIC: integer; REVISED: frac,
        B.pos = 0 ; B.active = true                    #   reads use linear interpolation
    if A.pos >= C:
        (A, B) = (B, inactive)
    if A.off >= N or A.off + A.pos >= N: break          # ran off the end of input

# Expected output length (CLASSIC), derived FROM THIS SCHEDULER:
#   grains launched G ≈ floor((N - C) / hop_in) + 1
#   length ≈ (G - 1) * hop_out + C        (tests compute this independently)
# REVISED only: fractional B.off ⇒ src reads use 2-point linear interpolation;
# this is the sole source of its "slight pitch drift" [AKZ §2.2].
```

Edge rules:
- reads past `N-1` return 0 (no wrap) **(PI)**;
- `T < 1` (compression) works identically offline — `hop_in > hop_out` skips material
  [AKZ §3, §10]; (FX-mode compression is restricted — §3.5);
- output length capped at 10 min @ model rate per channel (PI) with the authentic
  "not enough memory" refusal (architecture.md §5.1; hardware: [MAN §3 p.47]);
- stereo: run twice with the identical `hop_in` schedule (deterministic ⇒
  phase-coherent).

### 3.5 Real-time variant (`mws::engine::RealtimeStretcher`, FX mode)

Same two-grain scheduler fed from a preallocated history ring (precedent:
potenza-time-stretch [AKZ §5]); the *write* head is the host stream position, the
grain read heads lag. The full causality contract is ADR-006 / architecture.md §5.2;
engine-level summary:

- **T = 100%**: pure delay of exactly the reported latency (null-testable with
  character OFF).
- **T > 100%**: read head lags the write head (lag grows at `1 − 100/T` per output
  sample — unbounded over time, NOT bounded by cycle size; panel-corrected physics).
  FREE: lag bounded by the 30 s **(PI)** history; on exhaustion the read head jumps to
  `writePos − latency` at the next grain boundary. SYNC: hard resync to the window
  start at every `fxWindow` boundary (transport-aligned).
- **T < 100%**: requires future input; impossible as a pure insert. FREE: engine
  clamps effective T to 100% (LCD `FX MIN 100%`). SYNC: the captured window plays
  compressed, then silence to the window boundary **(PI)**.
- **Latency** = `ceil(2000 × hostRate / modelRate) + crossfadeLen + SRC group delay`,
  recomputed in prepareToPlay and on model/bandwidth/FS change (non-automatable
  params); reported via setLatencySamples (§7.4). NOT the bare "2048" constant —
  cycle length is in model-rate samples [AKZ §2.1 note] and the model rate scales it
  (panel critique: 2000 samples @ 7.5 kHz ≈ 267 ms).
- Host-rate processing: `cycleLen` is interpreted in **model-rate samples**; the
  character chain's ingest stage runs the stretch at model rate, so the sound matches
  the offline render at any host rate **(required for authenticity at 96 kHz hosts)**.
- Equivalence test contract: with parameters frozen from a known start state, the
  streamed output over a window equals the offline render of the same history
  sample-for-sample (testing-strategy.md §3.4b). Parameter changes apply at grain
  boundaries (no mid-grain morphing — no smoothing is ever applied inside the
  authentic scheduler).

---

## 4. INTELL approximation (`mws::stretch::IntellEngine`) — **v1.1, spec retained**

**Status: deferred to v1.1; labeled approximation when it ships.** INTELL internals
were never reverse-engineered ("INTELLIGENT mode internals are entirely unverified",
[DRR caveat 1]; "no open clone of it exists" [AKZ §9]); shipping a guess under the
authentic name at v1 was rejected by panel consensus. At v1 the `stretchMode` field
shows INTELL greyed; `qual`/`width` greyed "INTELL only" exactly as the hardware does
for CYCLIC [MAN §3 p.47].

v1.1 plan (offline-only, behind an ADR marking it a deliberate approximation): the
manuals say INTELL "varies the interpolation rate according to the sample content"
[MAN §3 p.45], QUAL = "the time spent determining cycle lengths" / "number of
decisions", WIDTH = "crossfade between original and inserted data", advice "low QUAL ⇒
high WIDTH" [MAN §3 p.47, §5]; the S2800-family manual describes the family algorithm
as "insert or delete blocks of sample data at appropriate places and crossfades…"
[MAN §5]. Approximation: content-adaptive cycle selection over the same cyclic core —
the ancestor-of-SOLA interpretation [AKZ §9; DRR F1]:

```text
Every D output grains, re-estimate the local cycle length:
  decisionInterval D = max(1, round(100 / qual))         (PI)
  searchRange        = C_nominal * [0.5 .. 2.0]          (PI)
  C_local = argmax over lag in searchRange of normalized autocorrelation  (AutoCorr)
  candidates evaluated = 8 + qual                        (PI)
Crossfade fraction F = clamp(width / 100, 0.05, 0.95)    (PI mapping)
Grain scheduling identical to §3.4 with C := C_local re-evaluated at decisions;
hop arithmetic always fractional (INTELL has no documented integer-timing trait).
```

Acceptance bar: (a) smoother than CYCLIC on speech/music as the manual promises,
(b) reproduces the QUAL/WIDTH interaction advice, (c) labeled an approximation in the
UI and docs. Golden tests pin OUR algorithm (product behavior), not hardware
equivalence. Hardware black-box measurement may upgrade it later (open question,
[DRR Open questions]).

---

## 5. S950Engine

The S950's stretch is parameter-distinct from the S1000's [MAN §8 finding 2]:
range to 999%, D-TIME smoothing constant, AUTO-D, Mon1/Pol2 — no CYCLIC/INTELL, no
qual/width [MAN §2 pp.29–30, §6].

Implementation = `CyclicEngine` with S950 semantics. **Every row below marked (PI) is
deviation-until-calibrated: real S950 captures are a v1-freeze gate for the D-TIME
mapping** (panel risk: the S950 is the culturally beloved model and its parameters are
the least documented).

| S950 control | Engine mapping | Justification |
|---|---|---|
| STRETCH % (≤999, display default 200) | `T = stretch/100`, integer %, CLASSIC hop by default | range/default [MAN §2]; integer/CLASSIC per era behavior [AKZ §2.2] |
| D-TIME (display default 1000, units undocumented) | cycle length C in samples, 20–2000 | **(PI, deviation-until-calibrated)** — the manual's described behavior ("longer D-TIME ⇒ slight tremolo, shorter ⇒ metallic" [MAN §2 pp.29–30]) is exactly the documented cycle-length behavior ("decrease … more 'metallic'" [AKZ §2.1]); identical default 1000 corroborates but is one coincidental data point |
| AUTO-D | run §7.1 auto-cycle detector | feature documented [MAN §2]; algorithm ours **(PI)** |
| MON1 | per-grain: snap C to nearest detected pitch period (autocorrelation around C) | **(PI, deviation-until-calibrated)** — manual gives usage guidance only ("more suitable for lengthening a single tone" [MAN §2]); the panel flagged that ANY behavioral mapping here is invention — this one is chosen because it is audible, plausible, and removable without breaking presets |
| POL2 | fixed C exactly as set | **(PI)** — "better suited to … drum loops" = the classic fixed-cycle sound |

Mono: the S950 is a mono machine [MAN §2]; stereo input is summed to mono in
S950/S900 modes (authentic; LCD states it; an A/B against the stereo-preserving
S1000 mode is the documented workaround). Engine output then runs the S950 character
chain (§8.1).

---

## 6. S900 RepitchEngine

The S900 has **no timestretch** — "zero occurrences of the word 'stretch'" in the
manual; OS4's TIME SKEW is not a pitch-preserving stretch and is out of scope
[MAN §1; DRR F3, open question 4]. Per ADR-003 (panel-unanimous), S900 mode is the
honest emulation: **varispeed repitch** — time and pitch are coupled, exactly how S900
users "stretched" breaks, and how trackers paired with cyclic stretch [AKZ §6:
"transpose is done by the tracker's note playback … exactly how people used real
Akais"]. The LCD says so ("S900: NO TIMESTRETCH — VARISPEED").

```text
rate   = 1 / T                      # timeFactor 200% ⇒ playback at half speed,
                                    # pitch drops 12 st; LCD shows the resulting
                                    # semitone offset: -12 * log2(T) st
out    = read src at increment `rate`   # the variable-rate read IS the §8.1 virtual
                                    # DAC clock: clock = f_s * rate * 2^(transpose/12);
                                    # ZOH read, NO interpolation [DRR F4]; the
                                    # tracking Butterworth follows the clock (§8.1)
transpose param: multiplies the same rate (2^(st/12))
```

In S900 mode `stretchMode`, `cycleLen`, `qual`, `width` are inert and the LCD hides
them. FX mode: a variable-rate read head on the history ring; `rate > 1`
(time-compression) consumes input faster than it arrives, so FX FREE mode clamps
rate ≤ 1 just as §3.5 clamps T ≥ 100% (panel critique — same causality, same fix);
SYNC window mode permits it.

---

## 7. Cross-cutting stages

### 7.1 Auto cycle detection (autC / AUTO-D) **(PI)**

Documented as a software-logic helper, "not always infallible" [MAN §3 p.46]; the
manual does NOT say "autocorrelation" — that is our inference, ADR-001-flagged (panel
critique P13). Ours: normalized autocorrelation over the stretch zone (or first 200 ms
**(PI)**), lag range 20–2000 samples; pick the highest peak; if no peak exceeds 0.3
**(PI)**, fall back to 1000. Deterministic, unit-tested.

### 7.2 Transpose (pitch shift) stage

A **separate resampling pass after the stretch**, with anti-aliasing when pitching —
mirrors both Akaizer ("Improved sound quality of pitch shift algorithm by using an
anti-aliasing filter", v1.2 changelog [AKZ §2.4]) and the hardware workflow (stretch
first, repitch at playback [AKZ §2.4, §6]).

- Ratio `p = 2^(-transpose/12)` applied as output-length resample [AKZ §10].
- S1000/S1100 models: 16-tap windowed-sinc (Kaiser β=8) **(PI)** — a quality stand-in
  for the hardware's "interpolation and decimation 24-bit algorithm, custom VLSI"
  [MAN §3 p.81]. The manual says only "interpolates" [DRR F5]; any specific kernel
  (polyphase, linear, sinc) is an assumption — ours is PI-tagged, ADR-001 (panel
  critique P13).
- S900/S950 models: transpose does NOT use the sinc stage — it modulates the
  **virtual DAC clock** in the character chain (§8.1) so the reconstruction filter
  and imaging track pitch — the property RX950 lacks and KVR critics call out ("a bit
  crusher doesn't follow the notes") [DRR F7]. This is the product differentiator.

### 7.3 Output normalization

Default **OFF everywhere** (authentic: no manual documents normalization; OpenMPT
writes samples through unnormalized [AKZ §4.1]). `norm = ON` reproduces Akaizer's
v1.3 peak-normalize-to-source-peak behavior [AKZ §2.1] as a modern-UX opt-in.
(Panel-revised: the first draft defaulted ON in render mode, which is Akaizer
behavior, not hardware behavior — critique P14.) FX mode never normalizes.
Any comparison harness against Akaizer renders must peak-normalize both sides first
(Akaizer output is normalized since v1.3 [AKZ §2.1]).

### 7.4 Latency

- SAMPLE mode: none (offline render; playback is a sample player).
- FX mode: `L = ceil(2000 × hostRate / modelRate) + crossfadeLen + SRC group delay`
  (§3.5). Recomputed in prepareToPlay and on model/bandwidth/FS change; those
  parameters are non-automatable precisely because they change L, and the docs state
  that hosts apply mid-session PDC changes inconsistently. Character-only FX (chain
  without stretch, T locked 100%) reports only the SRC + filter group delay.

---

## 8. Character chains (per model)

Placement (see architecture.md §5.1): ingest emulation BEFORE the stretch engine
(the hardware stretched already-quantized, already-band-limited sample RAM — the
TAL-validated order: "We really down-sample the sample to the desired sampling
frequency, then process" [DRR F8]), playback emulation AFTER stretch+transpose. All
stages float32 internally; "bit depth" stages quantize values, they do not change
storage type.

### 8.1 S900 / S950 — variable-clock 12-bit chain [DRR F4 (service-manual grade), MAN §§1–2]

```
in ──► resample to f_s = 2.5 × bandwidth  (7.5–40 kHz S900 / 7.5–48 kHz S950)
    ──► quantize 12-bit (mid-tread, no dither (PI); stretch arithmetic at 16-bit
        precision per "12-bit sampling/16-bit processing" [MAN §2 p.74])
    ──► [stretch / repitch engine at f_s]
    ──► virtual variable-clock playback: clock = f_s × 2^(transpose/12)
        · zero-order hold at the virtual clock, NO interpolation [DRR F4]
        · IMPLEMENTATION NOTE (PI, panel critique): the ZOH images live at multiples
          of the VOICE clock; rendering this stage directly at host rate folds them
          wrongly. The stage runs at an internal oversampled rate
          ≥ 2 × max(clock, hostRate) (4× host default (PI)) so images are represented
          before filtering, then decimates to host rate after the Butterworth.
          This is the heaviest DSP in the plugin — early prototype task
          (architecture.md §10 risk 3).
    ──► reconstruction: 6th-order Butterworth low-pass (3 cascaded biquads,
        Q = 0.5176 / 0.7071 / 1.9319 — standard Butterworth factorization),
        cutoff = clock / 2.5, cutoff TRACKS the clock  [DRR F4: BA9221 DAC +
        MF6CN-50 switched-cap filter, clock-controlled; rate = 2.5 × bandwidth]
        · per-note retuning (MIDI voice) recomputes coefficients on the audio
          thread, closed-form, no allocation (architecture.md §4.2)
    ──► decimate to host rate
```

Do NOT add preamp/filter saturation layers — that explanation of the S900 sound was
refuted 0-3 [DRR refuted #4]. The SC-filter clock-bleed whine noted by ALM [DRR F4]
is a v2 idea (off by default if ever added) — there is currently no oracle for it
(RX950 cannot validate clock-tracking behavior at all [DRR F7]; ALM MUM M8 is
hardware we hold no captures from — panel critique of the "validate against RX950"
plan).

### 8.2 S1000 / S1100 — fixed-rate 16-bit chain [MAN §3 p.81, DRR F5]

```
in ──► resample to 44.1 or 22.05 kHz (sampleRateSel)
    ──► quantize 16-bit
    ──► [stretch engine]
    ──► transpose via §7.2 windowed-sinc (fixed-rate interpolating playback [DRR F5])
    ──► VOICE FILTER (not a reconstruction stage — panel-corrected): the spec's
        "digital moving low-pass, -18 dB/oct" [MAN §3 p.81] is the per-voice
        keygroup/tone filter and is FULLY OPEN by default on hardware. We implement
        it as a 3rd-order Butterworth (one real pole + complex pair, Q = 1.0 — NOT
        three identical one-poles, which is not Butterworth; panel critique P15),
        default fully open (transparent), not user-exposed at v1 (PI; exposing a
        FILTER control is additive later). Hard-wiring it closed/active would color
        playback the hardware does not — a fabricated artifact (critique P4 of the
        purist proposal).
    ──► resample to host rate (windowed-sinc)
S1100 deltas: identical engine + chain [MAN §4 — stretch chapter verbatim identical];
output stage models the 20-bit DAC as 16-bit quantize with TPDF dither (lower noise
floor) (PI; 20-bit DAC is secondary-source [MAN §4]). NO output-level offset versus
the S1000 (panel-revised, see §2 `outTrim` row).
```

### 8.3 S3000 — 1992 chain [MAN §5] — **v1.1**

```
As 8.2 with: 28-bit accumulation ⇒ no intermediate quantize between stretch and
filter (PI interpretation of "28-bit accumulation"); voice filter is -12 dB/octave
WITH resonance [MAN §5 spec] — a resonant SVF, resonance fixed at minimum for v1.1
(PI); 16-bit quantize at ingest only. ModelId slot and faceplate slot reserved at v1.
```

### 8.4 Character chain bypass

`character = OFF` skips every stage in §8 (engine runs at host rate on unquantized
audio) — modern-UX escape hatch and the basis for engine-only null tests.

---

## 9. Per-model golden presets (validation targets, v1)

| Preset | Model | Params | Source |
|---|---|---|---|
| "Jungle Amen 300" | S1100 | CYCLIC, Cycle 1000, Time 300%, (qual 20 width 10 stored but inert in CYCLIC) | documented S1100 classic-jungle settings [AKZ §9; DRR F10: SPOD] |
| "S950 vocal 200" | S950 | STRETCH 200%, D-TIME 1000, POL2 | manual display defaults [MAN §2 p.30] |
| "S900 half-speed" | S900 | timeFactor 200% (varispeed, -12 st), BW max | ADR-003 semantics |
| "Dred vox" | S1000 | CYCLIC, Cycle 200, Time 400% (PI extremes) | the timestretched-vocal idiom [DRR F9] — settings are PI, the idiom is cited |

(S3000 "factory default" — CYCLIC, Cycle 1000, Time 100%, qual 10, width 10, from the
printed example screen [MAN §5] — moves to v1.1 with the model.) These ship as factory
presets and double as golden-test cases (`tests/golden/cases.json`).

---

## 10. Implementation order (suggested backlog mapping)

1. `mws::core` primitives (Buffer, WavIo, Resampler, Butterworth, Quantizer, AutoCorr)
2. `CyclicEngine` CLASSIC + REVISED (offline) + unit/property tests (§3.2 invariants,
   schedule-derived length)
3. `tools/mwstime-render` CLI + first golden cases
4. **Early-risk prototype: §8.1 variable-clock chain** (oversampled ZOH + tracking
   Butterworth) — validate CPU + property tests before more chain work
5. CharacterChain S1000 → S950/S900 → S1100 delta
6. Transpose stage; S950Engine; RepitchEngine
7. `RealtimeStretcher` to the ADR-006 contract (its own task; contract tests first)
8. Plugin shell (APVTS superset params, threading, state blob cache); MIDI voice
   (own task, architecture.md §4.2); UI
9. v1.1: IntellEngine; S3000 ModelSpec + resonant SVF + faceplate
