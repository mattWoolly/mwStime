# Akaizer / Akai S-series Cyclic Timestretch — Source Research & Algorithm Analysis

*Research date: 2026-06-12. Local source cache: `research-cache/akaizer/` (scratch, not committed).*

---

## 1. Headline finding: Akaizer itself is NOT open source

Despite being widely described as a "free tool", **Akaizer has never had its source code published**.
What exists publicly:

- **Official site:** https://the-akaizer-project.blogspot.com/ — as of March 2026 Akaizer is at
  **v2.5 and is now £10 payware** (PayHip), Windows-only (Wine for Linux/macOS). No source, no
  license file, author anonymous.
- **Binary archive (GitHub):** https://github.com/juanpc2018/Akai-S1000-Project — folder
  `Akaizer/` contains `akaizer_cli_v2.2` zips for Windows/macOS/Linux plus v2.3/v2.4 GUI builds.
  I downloaded and unpacked the Linux CLI build into
  `research-cache/akaizer/cli-lin/` (`Akaizer` ELF binary, `Readme.txt`, `Changelog.txt`).
- The v2.2 changelog itself confirms the closed/grey status
  (`research-cache/akaizer/cli-lin/Changelog.txt`, entry "6 October 2017 - v2.2"):
  > *"REMOVED: References to copyright and name of developer. Why? Copyright law is a bit of a
  > grey area where reverse engineering is concerned."*
- The Renoise "Akaizer" tool (renoise.com/tools/akaizer) is only a **Lua wrapper that shells out
  to the closed binary** — no DSP source there either.

So this document does three things:

1. Extracts everything knowable about Akaizer's algorithm from its **primary sources**: the
   official CLI `Readme.txt`/`Changelog.txt` and symbols recovered from the shipped binary.
2. Deeply analyzes the **closest open-source implementations** of the same Akai-style cyclic
   stretch, with file/line citations:
   - **OpenMPT** `LoFi` stretch (C++, BSD) — the cleanest documented implementation, whose own
     comments explicitly compare it to Akaizer.
   - **potenza-time-stretch** (C++, real-time, from the JUCE forum thread "A lightweight
     Akai-style time-stretch algorithm (realtime!)").
   - **MilkyTracker** "90s Timestretch" (C++).
   - **nanoTracker `akaizer.ntsfx`** (JS AudioWorklet port, self-described Akaizer port).
3. Reconstructs Akaizer's *classic* vs *revised* modes from the documented behavior + binary
   evidence, clearly labeled as inference.

---

## 2. Akaizer: everything the primary sources tell us

Source of truth: `research-cache/akaizer/cli-lin/Readme.txt` (official, v2.2, Oct 2017) and
`Changelog.txt`. CLI syntax:

```
akaizer <file> <time factor> <cycle length> <transpose> [-c] [-l] [-p]
```

### 2.1 Parameter table (official)

| Parameter | Units | Range | Default | Notes (verbatim or paraphrased from Readme.txt) |
|---|---|---|---|---|
| Time Factor | percent of original duration | **25 – 2000 %** | 100 | 50 = half length, 200 = double. **Two decimal places (e.g. 207.59) allowed only in the REVISED algorithm** — "Akai samplers don't use decimal values for Time Factor" (Changelog v2.1). |
| Cycle Length | **samples** (not ms, not Hz) | **20 – 2000 samples** | 1000 | "Decrease the value to get a more 'metallic' effect and vice versa. 1000 is a good central starting point." Auto-clamped down to the file length if the file is shorter (Changelog v1.6); binary strings confirm: `"Cycle Length has been limited to 20 samples for this particular file."` |
| Transpose | semitones | **-24 – +24** (v2.4 GUI extended to ±36) | 0 | Independent of time factor; both can be applied in one pass. |
| `-c` | flag | — | off | Selects **CLASSIC** algorithm; default (flag absent) is **REVISED**. |
| `-l` / `-p` | flags | — | off | Preview-loop / preview only; no DSP effect. |

Constraints: input must be ≥ 40 ms (v2.1); WAV/AIFF 8/16/24/32-int and 32-float, mono/stereo;
all processing in **32-bit float** since v1.1; output is **peak-normalized to the source peak**
since v1.3.

Note the units carefully: **cycle length is in raw samples**, so its real-world duration depends
on the file's sample rate (1000 samples ≈ 22.7 ms @ 44.1 kHz, ≈ 45.4 ms @ 22.05 kHz). This is
why Akaizing low-sample-rate jungle breaks sounds different from 44.1 kHz material at the same
setting.

### 2.2 CLASSIC vs REVISED (official description, Readme.txt "USAGE" section)

> *"'Classic' simulates the Akai cyclic time stretch as faithfully as possible, with perfect
> pitch but with minor quirks like **bad timing**. 'Revised' improves on the 'classic' algorithm
> with **perfect timing and a fuller sound** but with the minor compromise of the **pitch
> drifting ever so slightly away from its true key** with some settings. The 'revised' algorithm
> is basically the same one as used in previous 1.x versions of Akaizer."*

Changelog v2.1 adds: *"The CLASSIC algorithm is now pretty much **sample accurate** [vs. real
Akai output] for the majority of settings, mainly when Time Factor is between 120% and 2000%
with any Cycle Length between 20 to 2000."* (The author verified against real hardware renders.)

### 2.3 Binary evidence (`research-cache/akaizer/cli-lin/Akaizer`, ELF 32-bit i386, stripped)

`strings` on the binary shows Akaizer is written in **REALbasic/Xojo** (RB runtime symbols, MBS
PortAudio plugin classes for preview playback). One custom DSP symbol survives stripping:

```
Module1.Calculate_AkaiStepLen%i4%i4i4     ← Calculate_AkaiStepLen(int32, int32) -> int32
```

i.e. the CLASSIC path computes an **integer "Akai step length" from two integer inputs**
(almost certainly time factor and cycle length). This matches the documented behavior exactly:
integer-only Time Factor in CLASSIC, "perfect pitch but bad timing" — see §3.

### 2.4 What the two modes imply (inference, but tightly constrained)

The trade-off stated by the author is the key to reverse-engineering the structure:

- **CLASSIC ("perfect pitch, bad timing")**: every output cycle is a **verbatim, unresampled
  copy** of an input cycle (hence zero pitch error), and the stretch is achieved purely by an
  **integer schedule of cycle repeats/skips** (hence the output length is quantized — "bad
  timing"). `Calculate_AkaiStepLen(timeFactor, cycleLen)` computes the integer input-hop
  (step) between successive cycle reads. For time factor `T`% and cycle length `C`:
  - conceptual input hop per output cycle: `step = round(C * 100 / T)` (an integer number of
    samples — the only way to get both integer arithmetic and verbatim copies);
  - `T = 200%` → step = C/2: each cycle effectively played twice; `T = 50%` → step = 2C: every
    other cycle skipped; non-round ratios produce a Bresenham-like uneven repeat pattern, which
    is exactly the rhythmic "stutter/flam" timing quirk old Akais are known for.
- **REVISED ("perfect timing, slight pitch drift")**: the input read position advances by a
  **fractional hop** (`C * 100 / T` kept as float), so the output length is exact for any
  decimal time factor; but to keep cycle boundaries aligned the cycle replay must be slightly
  rate-warped or the hop rounded per-cycle and compensated, producing the documented sub-cent
  pitch drift. "Fuller sound" suggests a longer/equal-power overlap than the classic mode.

Pitch shifting (Transpose) is a **separate resampling stage** (changelog v1.2: *"Improved sound
quality of pitch shift algorithm by using an anti-aliasing filter"*) — i.e. classic
varispeed-style resampling, combined with the cyclic stretch to give independent pitch/time.
That mirrors how the hardware did it (stretch first, then play back at a different keygroup
pitch, or vice versa).

What is **not** recoverable without disassembly: the exact crossfade shape and length Akaizer
uses between cycles. The open-source implementations below show the spectrum of choices, and
all of them note (or audibly exhibit) the same characteristic artifacts.

---

## 3. The cyclic algorithm, in general form

All implementations share this skeleton, which is the essence of the Akai S950/S1000 "CYCLIC"
timestretch:

```
C        = cycle length (grain size), FIXED for the whole file (samples)
T        = stretch ratio (outputLen / inputLen)            e.g. 2.0 for 200%
p        = pitch ratio (2^(semitones/12)), 1.0 if no transpose

# Two grain players, A and B. Each plays C samples of the source verbatim
# (or resampled by p if transposing). Grains are launched so that consecutive
# grains overlap by some fraction; the *input* start position of each new
# grain advances by hop_in = hop_out / T, where hop_out is the output spacing
# of grain launches. T > 1 ⇒ hop_in < hop_out ⇒ material is re-read/repeated.
# T < 1 ⇒ hop_in > hop_out ⇒ material is skipped.

for each output sample:
    out  = grainA.read(p) * w(grainA.phase)
    if grainB.active:
        out += grainB.read(p) * (1 - w(grainA.phase))      # complementary fade
    if grainA.phase >= overlapStart:                       # time to launch next grain
        grainB.start(at = grainA.inputStart + hop_in)
    if grainA.phase >= C: swap(A, B)
```

The characteristic **metallic/fluttery sound** comes from four properties:

1. **Fixed, content-blind cycle size.** Splice points land anywhere in the waveform; when `C`
   doesn't match the local pitch period, every splice adds a discontinuity that repeats at the
   grain rate `sr / hop_out` — an audible comb/ring at a constant frequency. Smaller `C` →
   higher, more "metallic" ring (Akaizer readme says exactly this).
2. **Repetition of identical cycles.** At T ≥ 2 every cycle is heard ≥ 2 times verbatim —
   the "stutter" in classic jungle vocal/break stretches.
3. **Short, simple crossfades** (linear in every faithful implementation found) — energy dips
   and phase cancellation at each splice = "flutter".
4. **Detuning of low frequencies**: any period longer than `C` cannot survive splicing; OpenMPT's
   comment (below) calls out the resulting "ringmod-like sound at very small grain sizes".

---

## 4. Open-source implementation #1: OpenMPT `LoFi` (primary reference)

- **Repo:** https://github.com/OpenMPT/openmpt (BSD license)
- **File:** `tracklib/TimeStretchPitchShift.cpp`, `Result LoFi::Process()` — **lines 244–383**
- **Commit at time of reading:** `4029340922ef66b4164f626e9ec3c18da2b784aa` (2026-04-27)
- Local copy: `research-cache/akaizer/openmpt/TimeStretchPitchShift.cpp`
- UI: grain size is an editable combo (samples), default index pre-selected
  (`mptrack/Ctrl_smp.cpp:305`); minimum grain size **16 samples**
  (`TimeStretchPitchShift.cpp:248`).

The authors' own comment (lines 253–259) is the best concise description in any open codebase:

```cpp
// General idea of "Akai-style" cyclic time-stretch (not necessarily how the hardware
// actually does it - Akaizer produces different results):
// We divide the sample into lots of small grains of a fixed size.
// A grain is played and once we get close to the end of the grain, we start playing the
// next grain further into the sample, and cross-fade between them.
// Pitch shift simply changes the speed at which the grains themselves are played.
// Note that the current implementation has some problems:
// - Flutter from crossfades may be audible
// - Low frequencies (period > grain size) will get detuned; ringmod-like sound at very small grain sizes
```

### 4.1 Exact arithmetic (with line numbers)

- **Overlap point** (line 260–261): `overlapAt = 0.8` → crossfade occupies the **last 20% of
  each grain**; `overlapStartSample = grainSize * 0.8`.
- **Input hop** (line 262):
  ```cpp
  nextGrainIncrement = round(grainSize * 0.8 / (stretchRatio * pitch));
  // "Always start from integer position to avoid resampling artifacts when not pitch-shifting"
  ```
  Output hop is `0.8 * grainSize`; input hop is that divided by stretch *and* pitch ratio
  (dividing by pitch keeps duration correct because grains are read back at `pitch` rate).
  **Rounded to an integer source offset** — deliberate, so unpitched stretches copy samples
  verbatim (the "perfect pitch" property of Akai classic mode).
- **Position format**: 32.32 fixed point (`SamplePosition`); interpolation fraction truncated
  to 15 bits (line 277: `fract = GetFract() >> 17`).
- **Sample interpolation** (lines 293–298) — **integer linear interpolation** on the native
  8/16-bit data, no float in the signal path:
  ```cpp
  int32 smp1 = in[intPos * numChans];
  int32 smp2 = in[(intPos + 1) * numChans];
  return smp1 + (smp2 - smp1) * fract / 32768;
  ```
- **Crossfade** (lines 329–343) — **linear**, computed in doubles, then **rounded to int**:
  ```cpp
  fade   = (grains[0].playPos - overlapStartSample) * overlapSamplesInv;  // 0..1 over last 20%
  grain1 = round(grain1 + (grain2 - grain1) * fade);
  ```
  This is a plain linear fade (not equal-power), so correlated material crossfades cleanly but
  uncorrelated splices dip ~ -6 dB at the midpoint → flutter.
- **Grain state machine** (lines 356–374): exactly two grains; grain B is launched when grain A
  passes the 80% mark (`playPos >= overlapStartSample`), at source offset
  `offsetA + nextGrainIncrement`; when a grain passes `grainSize` it is retired and B becomes A.
- **Pitch shift** (lines 263, 356–357): per-sample increment = `pitch` ratio. **Stretch alone
  never resamples** (increment = 1.0); transpose resamples inside the grains, classic Akai
  style. Output is written sample-by-sample directly into the destination buffer at the
  original bit depth (lines 345–353) — no normalization stage.

### 4.2 Faithful pseudocode of the OpenMPT core loop

```text
INPUT : src[0..N), grainSize C (samples, >=16), stretch T, pitch p
CONST : overlapAt = 0.8
hopIn  = round(C * overlapAt / (T * p))          # integer source samples
ovStart= C * overlapAt
A.off = selectionStart ; A.pos = 0 ; B = inactive

for each output sample (until round(N*T) written):
    s1 = lerp(src, A.off + A.pos)                # int linear interp, 15-bit fraction
    if B active:
        fade = (A.pos - ovStart) / (C - ovStart) # linear 0..1
        s2   = lerp(src, B.off + B.pos)
        s1   = round(s1 + (s2 - s1) * fade)
    emit s1
    A.pos += p ; B.pos += p                      # p = 1.0 when only stretching
    if B inactive and A.pos >= ovStart:
        B.off = A.off + hopIn ; B.pos = 0 ; B active
    if A.pos >= C:  A = B ; B = inactive
```

---

## 5. Open-source implementation #2: potenza-time-stretch (real-time)

- **Repo:** https://github.com/dar-io-p/potenza-time-stretch
- **Commit:** `ddb44a8f949b3f49320932e1d2e997b3a02149bb` (2024-03-19)
- **File:** `TimeStretch.h` (entire algorithm, 165 lines, header-only struct)
- Origin: JUCE forum thread *"A lightweight Akai-style time-stretch algorithm (realtime!)"*
  (https://forum.juce.com/t/a-lightweight-akai-style-time-stretch-algorithm-realtime/60514);
  the author built it to emulate the S950 sound for the "Amigo Sampler" plugin.
- Local clone: `research-cache/akaizer/potenza/`

Differences vs OpenMPT, all visible in `TimeStretch.h`:

- **Crossfade fraction is 40%**, not 20%: `double c = 0.4; cPrime = 0.6;` (lines 156–157),
  fade-out trigger at `f2 = grainSize * cPrime` (line 17/83). Longer overlap = smoother, less
  metallic; the JUCE thread notes it still gets "sharper transients than the Akai" at 2000%.
- **Linear window**, computed as `window = m * phi` with `m = 1/(grainSize*c)` (lines 14,
  119–124): after a swap the *new* grain's phase drives the fade, ramping 0→1 over its first
  40% while the retiring grain gets `1 - window`. Two grains max, same as OpenMPT.
- **Input hop**: `grainOffset = grainSize * cPrime / stretchFactor` (line 21) — output hop
  `0.6·C` divided by stretch; **kept as a double** (no rounding) → this behaves like Akaizer's
  REVISED mode (exact timing, possible sub-sample phase error instead).
- **Pitch**: playback rate `pitchDelta` per sample with a pluggable interpolation lambda
  (line 113); optional `pitchCompensator = stretchFactor` (line 7) ties replay rate to stretch
  (so 200% stretch = +12 st grain replay), i.e. a "repitch within grains" mode.
- Extras the offline tools don't have: **reverse playback** (direction = -1 path, lines
  89–108) and loop-point handling (`loop()`, lines 48–61). All math in doubles, float samples.

---

## 6. Open-source implementation #3: MilkyTracker "90s Timestretch"

- **Repo:** https://github.com/milkytracker/MilkyTracker
- **Commit:** `15302cf44d734b126e8856112c403ea9aad26f7a` (2026-05-01)
- **File:** `src/tracker/SampleEditor.cpp`, `tool_timestretch()` — **lines 3632–3697**
  (comment at 3657: `// 90s akai-style timestretch algo`)
- Dialog ranges (`src/tracker/SampleEditorControlToolHandler.cpp:298–305`):
  **Grainsize 1–10000 samples, default 3900; Stretch 0–20, default 3.**
- Local copy: `research-cache/akaizer/milkytracker/SampleEditor.cpp`

This is the crudest and most "hardware-naive" of the three — an **overlap-add repeater**:

- Walks the input in steps of `grain` samples (line 3658–3672, `gi = (gi+1) % grain` gate).
- For each input grain it writes the grain **`stretch = 1 + param` times** into the output,
  each copy advancing the output write head by **`grain/2`** (50% overlap-add, line 3661).
  Effective time factor ≈ `stretch / 2` — i.e. integer-quantized, like Akai/Akaizer CLASSIC.
  It can only stretch (no compression path).
- **Window** (line 3663–3664): `scale = sin(gin / M_PI * 9.8664)` with `gin ∈ [0,1)`.
  `9.8664 / π ≈ 3.1403`, so this is a hand-rolled approximation of `sin(π·gin)` — a **half-sine
  window** over each grain, overlap-added at 50%. (sin² would sum to unity; plain sin does not,
  so level ripples at the grain rate — part of its lo-fi charm, then patched globally by the
  tracker's own normalization.)
- Float accumulation buffer, integer sample I/O; no interpolation, no pitch option at all
  (transpose is done by the tracker's note playback instead — exactly how people used real
  Akais: stretch first, repitch by playing at a different note).

---

## 7. Implementation #4: nanoTracker `akaizer.ntsfx` (JS port, "port of 2DaT Akaizer")

- **Repo:** https://github.com/conradzeus/nanotracker---akaizer
  (commit `a3c7a84f18c2f953129eb0cc979fa3344503485b`, 2026-04-18); the `.ntsfx` is a zip;
  extracted to `research-cache/akaizer/nanotracker-port/` (`plugin.json`, `script.js`).
- Real-time AudioWorklet effect (203 lines, `script.js`). Parameters (`plugin.json`):
  stretch 0.25–4.0, pitch ±24 st, **grain 5–120 ms (default 30 ms)**, **crossfade 5–95% of
  grain (default 50%)**, plus deliberately lo-fi extras: bit depth 4–16 (default **12-bit**,
  the S950's word size) and a sample-rate divider (zero-order hold), `script.js:120–124,
  186–196`.
- DSP (`script.js:152–197`): ring buffer, two grain slots, **raised-cosine (Hann-edge) fades**
  (`_window()`, lines 110–118), `hop = grainLen - xfadeLen`, read position advancing
  `hop * pitchRatio / stretch` per grain (line 170).
- Verdict: the least faithful structurally (ms-based grains, cosine fades — real Akai-style
  implementations use sample-based cycles and linear fades), but it documents the *popular
  conception* of the sound: fixed grains + crossfade + 12-bit grit.

---

## 8. Cross-implementation comparison

| | Akaizer (closed; per docs/binary) | OpenMPT `LoFi` | potenza | MilkyTracker | nanoTracker port |
|---|---|---|---|---|---|
| Cycle/grain units | **samples** | samples | samples | samples | **ms** |
| Range / default | 20–2000 / 1000 | ≥16 / UI list | free | 1–10000 / 3900 | 5–120 ms / 30 ms |
| Stretch range | 25–2000 % | any float | any float | ×0.5–×10 (integer repeats) | 25–400 % |
| Crossfade shape | unknown (closed) | **linear** | **linear** | half-sine OLA | Hann edges |
| Overlap fraction | unknown | 20 % of grain | 40 % of grain | 50 % | 5–95 % (50 %) |
| Input hop arithmetic | CLASSIC: **integer** (`Calculate_AkaiStepLen`); REVISED: fractional | **integer** (`round`) | fractional double | integer | fractional |
| Pitch shift | resample + anti-alias filter | grain-replay resampling, int lerp | grain-replay resampling | none | grain-replay resampling |
| Signal arithmetic | 32-bit float internally | **integer** (int lerp, /32768, round) | double/float | float accumulate | float + optional bitcrush |
| Timing vs pitch trade | classic: pitch-perfect/timing-quantized; revised: timing-perfect/pitch-drift | pitch-perfect (integer hop ⇒ length rounding) | timing-perfect | quantized (integer repeats) | timing-perfect |

The OpenMPT integer-hop choice and the Akaizer CLASSIC integer step are the same idea — and
both reproduce the hardware trait that **stretch-only never interpolates samples**, which keeps
transients crisp and pitch exact while making output length only approximately right.

---

## 9. What the real Akai hardware does (and what these tools capture)

From the Akai S1000 operator's manual (timestretch added in OS v2.x; the juanpc2018 archive
readme says the EPROM v2.2 update introduced it on the S1000) and period documentation:

- The hardware offers **two stretch modes**: **CYCLIC** — "maintains a fixed interpolation rate
  throughout the whole sample" — and **INTELL(igent)** — "varies the interpolation rate
  according to the sample content" (pitch-period-adaptive splicing, intended for speech/music).
- Hardware parameters beyond Time Factor (25–2000%) and Cycle Length include a **Quality**
  setting (processing-time vs splice-search quality trade-off) and, on the S1100, **Width**;
  a documented S1100 example setting is *"Cycle Length: 1000, Time Factor: 300%, Stretch Mode:
  Cyclic, Qual: 20, Width: 10"* (spod.com.au S1100 timestretch page) — note the hardware's
  cycle length of 1000 matching Akaizer's default of 1000 samples.
- Hardware timestretch is an **offline render** (the sampler computes a new sample), exactly
  like Akaizer/OpenMPT/MilkyTracker; the real-time ports (potenza, nanoTracker) are modern
  conveniences the hardware never had.
- **Captured by Akaizer (per its own docs):** CYCLIC mode only, sample-accurately for Time
  Factor 120–2000% (Changelog v2.1), including the integer-schedule timing quirks; combined
  stretch+transpose; the 25–2000% range and cycle-length conventions.
- **Not captured / out of scope:** the INTELL mode (no open clone of it exists either — it is
  the ancestor of content-aware splicing à la SOLA/WSOLA); the Quality/Width parameters; the
  converter/filter coloration of the actual hardware (S950 12-bit / 7.5–40 kHz variable rate,
  S1000 16-bit) — the nanoTracker port fakes this with bitcrush/SR-divide, Akaizer does not
  attempt it. OpenMPT's comment explicitly warns its LoFi mode is "not necessarily how the
  hardware actually does it – Akaizer produces different results", the main visible deltas
  being crossfade length/shape and repeat scheduling.

---

## 10. Reconstructed Akaizer-CLASSIC pseudocode (inference — labeled as such)

Constrained by: official docs (§2.2), binary symbol `Calculate_AkaiStepLen(i4,i4)→i4` (§2.3),
and the OpenMPT/potenza structure that matches its audible output:

```text
INPUT : src[0..N), timeFactor T (integer %, 25..2000), cycleLen C (20..2000 samples),
        transpose t (integer semitones)
step  = Calculate_AkaiStepLen(T, C)        # ≈ round(C * 100 / T)  — INTEGER source hop
readPos = 0 ; out = []

while readPos < N:
    cycle = src[readPos : readPos + C]     # verbatim copy — no resampling ⇒ perfect pitch
    out  += crossfade_splice(out_tail, cycle)   # short fade at the seam (shape unknown;
                                                # all faithful clones use LINEAR)
    readPos += step                        # T>100 ⇒ step<C ⇒ material re-read (repeats)
                                           # T<100 ⇒ step>C ⇒ material skipped
# output length = N * C / step  ≈ N * T/100, but quantized by integer step ⇒ "bad timing"

if t != 0:
    out = resample(out, 2^(-t/12))         # separate pass, with anti-aliasing filter (v1.2)
out = normalize_to(source_peak)            # v1.3+
```

REVISED differs only in keeping `step` fractional (and accepting decimal `T`), trading the
length quantization for sub-cycle phase/pitch error ("pitch drifting ever so slightly").

---

## 11. Sources

**Binaries / primary documents**
- Akaizer CLI v2.2 (official Readme.txt, Changelog.txt, Linux ELF):
  https://github.com/juanpc2018/Akai-S1000-Project (`Akaizer/akaizer_cli_v2.2-lin.zip`),
  cached at `research-cache/akaizer/cli-lin/`
- Official site: https://the-akaizer-project.blogspot.com/ (v2.5, payware, closed source)

**Source code read in full**
- OpenMPT — `tracklib/TimeStretchPitchShift.cpp` lines 244–383, `.h` lines 83–97;
  https://github.com/OpenMPT/openmpt @ `4029340922ef66b4164f626e9ec3c18da2b784aa`
- potenza-time-stretch — `TimeStretch.h` (165 lines);
  https://github.com/dar-io-p/potenza-time-stretch @ `ddb44a8f949b3f49320932e1d2e997b3a02149bb`
- MilkyTracker — `src/tracker/SampleEditor.cpp` lines 3632–3697,
  `src/tracker/SampleEditorControlToolHandler.cpp` lines 298–305;
  https://github.com/milkytracker/MilkyTracker @ `15302cf44d734b126e8856112c403ea9aad26f7a`
- nanoTracker akaizer port — `script.js` (203 lines), `plugin.json`;
  https://github.com/conradzeus/nanotracker---akaizer @ `a3c7a84f18c2f953129eb0cc979fa3344503485b`

**Secondary**
- JUCE forum: "A lightweight Akai-style time-stretch algorithm (realtime!)"
  https://forum.juce.com/t/a-lightweight-akai-style-time-stretch-algorithm-realtime/60514
- Renoise Akaizer wrapper tool: https://www.renoise.com/tools/akaizer
- S1100 timestretch settings example: https://www.spod.com.au/oldgear-zone/2021/10/12/akai-s1100-timestretch-settings
- lewloiwc's Sound Design Suite (JSFX "Sample Warp – Texture Mode", an Akaizer-like warper):
  https://github.com/Suzuki-Re/Suzuki-Scripts
