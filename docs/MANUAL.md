<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly. -->

# mwStime — User Manual

This manual covers using mwStime as a player would. For the *what* and *why* see
the [README](../README.md); for building from source see
[BUILDING.md](BUILDING.md).

mwStime presents an Akai S-series control panel: a grey chassis, a green/amber LCD,
soft keys **F1–F8** under the screen, a **jog wheel**, cursor keys, and a model
selector. Every control is also keyboard-focusable (see [Accessibility](#accessibility)).

There are two operating modes, set by the **MODE** switch:

- **FX** (default) — a live insert; audio passing through is stretched in
  real time. The defining tradeoffs are in the
  [causality contract](#fx-mode-the-causality-contract).
- **SAMPLE** — the authentic hardware workflow: load a file, render offline,
  audition, drag out.

---

## 1. Load a sample (SAMPLE mode)

1. Switch **MODE** to **SAMPLE**.
2. **Drag an audio file onto the waveform area** to load it. Supported formats:
   **WAV, AIFF, FLAC** (no MP3 at v1). The file decodes off the audio thread, so the
   UI never stalls.
3. The LCD's top line shows the sample name and the waveform draws. The stretch
   **zone** defaults to the full sample length (`STRETCH ZONE / TO`).
4. If the format is unsupported you get a hardware-style LCD message line rather
   than a modal dialog.

---

## 2. Set stretch parameters

1. Use the **cursor keys** (or click) to focus an LCD field; edit it with the
   **jog wheel** (mouse wheel / Up-Down) or by typing a value directly.
2. As you change **TIME FACTOR**, the LCD updates the computed new length and
   memory readout live. In **CLASSIC** timing the *achieved*
   (schedule-quantized) length is shown — never the requested one — so the
   honest "bad timing" of the hardware is visible.
3. Press **F2 `autC`** to run **auto cycle detection**: it analyzes the zone and
   writes a cycle length into the `CYCLE LENGTH` field. On the S950 the same key
   reads `AUTO-D`.
4. **Tempo sync (F7 `SYNC`):** type or tap the source BPM (a filename like
   `break_174bpm.wav` is auto-guessed, always overridable). The LCD shows the
   resulting factor, e.g. `174.0 -> 87.0 = 200%`.

---

## 3. Audition & render (SAMPLE mode)

1. **F3 `ZONE`** loops the selected zone through the stretcher at the current
   settings for a live preview (CYCLIC modes only, exactly as the hardware
   documents).
2. **F4 `GO`** requests the offline render. The LCD shows progress and a
   remaining-time line (the hardware's own countdown). **Hold F8 to abort.** A
   render that would exceed the memory cap is refused with
   `** NOT ENOUGH MEMORY **` — see [Troubleshooting](#troubleshooting).
3. **F5 `PLAY`** plays the rendered sample; **F6 `A/B`** toggles between the
   original and the stretched result. **MIDI notes also trigger playback** (root
   C3, chromatic repitch, monophonic at v1).
4. **Export the render** two ways, once a `GO` render has completed (the waveform
   shows the stretched result and the `A/B` overlay becomes available):
   - **Drag the waveform out** to your host timeline or the desktop. *(Dragging
     directly into a DAW track is host-dependent on macOS — if a host doesn't
     accept the drop, use the menu below.)*
   - **Menu → "Export rendered sample…"** (the hamburger/`☰` menu) opens a file
     chooser and writes the stretched render to a WAV. This path is
     host-independent and is the reliable way to export. The item is greyed until
     a render exists.

---

## 4. FX mode (the default)

1. Switch **MODE** to **FX**. The waveform area becomes a live input scope; the
   `GO` / `PLAY` / `A-B` soft keys grey out (they are SAMPLE-only); the **WINDOW**
   selector becomes active.
2. The engine follows the [causality contract](#fx-mode-the-causality-contract):
   - **TIME FACTOR < 100% in FREE mode** displays `FX MIN 100%` (the engine
     clamps; your automation value is preserved).
   - **SYNC mode** shows the active window and its resync behavior.
   - Parameter changes take effect at grain boundaries (no smoothing inside the
     authentic scheduler).
3. The LCD shows a live **`TIME-STRETCH (REALTIME)`** page — deliberately
   non-hardware wording so it is always clear this mode exceeds what the original
   samplers could do.

### FX mode: the causality contract

A live stretcher has only the audio it has already received. mwStime is honest
about the consequences:

| Time factor | FREE (no transport) | SYNC (window 1/4 … 8 bars, default 1 bar) |
|---|---|---|
| **= 100%** | pure delay of exactly the reported latency (nulls vs dry with CHARACTER OFF) | same |
| **> 100%** (expansion) | read head lags, bounded by a 30 s history; on exhaustion it resyncs at the next grain boundary (audible jump) | hard-resyncs to the window start at each transport-aligned boundary — "stretch the last bar" |
| **< 100%** (compression) | **clamped to 100%**, LCD shows `FX MIN 100%` | captured window plays compressed, then silence to the boundary |

**Latency** is reported to the host and recomputed when you change **model**,
**bandwidth**, or **FS** (these are not automatable). Changing cycle length never
changes latency. Model switching is a latency-updating action — see PDC under
[Troubleshooting](#troubleshooting).

---

## 5. Switch models

1. Selecting a different model in the **MODEL** selector cross-fades the faceplate
   palette, re-lays-out the LCD, and clamps out-of-range parameters **at the
   engine** (the host-visible parameter ranges never change). Your pre-clamp value
   is remembered and restored if you switch back.
2. Selecting the **S900** or **S950** shows the **mono-sum notice**: those engines
   process a mono sum of the input (the S950 is a mono machine).
3. In FX mode a model switch cross-fades the engines over one block and re-reports
   latency to the host.

The four shipping models differ by character chain — see the
[model table in the README](../README.md#models). The **S900 has no timestretch**
(it is varispeed repitch; the time factor changes pitch). **INTELL** is a v1.1
feature; its `QUAL` / `WIDTH` fields are greyed at v1.

---

## Parameter reference

These are the plugin parameters, with the **hardware unit** shown on the LCD.
Ranges are a fixed superset across all models; the active model clamps at the
engine and the LCD shows the clamped hardware value (e.g. TIME FACTOR maxes at
999% while the S950 is selected). Parameters marked *non-automatable* cannot be
moved by host automation. (Source: [dsp-engine §2](design/dsp-engine.md).)

| LCD name | Range | Default | Unit | Applies to |
|---|---|---|---|---|
| **MODEL** *(non-automatable)* | S900 / S950 / S1000 / S1100 (+S3000 v1.1) | S1000 | — | — |
| **MODE** *(non-automatable)* | FX / SAMPLE | FX | — | — |
| **TIME FACTOR** | 25.00–2000.00 | 100.00 | % of original length | all stretch engines (S950 clamps to 999; FX FREE clamps low end to 100) |
| **CYCLE LENGTH** / **D-TIME** (S950) | 20–2000 | 1000 | samples at model rate | CYCLIC, S950 |
| **STRETCH MODE** | CYCLIC / INTELL | CYCLIC | — | S1000/S1100 (INTELL greyed until v1.1) |
| **TIMING** | CLASSIC / REVISED | CLASSIC | — | CYCLIC & S950 (CLASSIC = hardware-faithful) |
| **TRANSPOSE** | -24.00–+24.00 | 0 | semitones (1-cent steps) | all |
| **QUAL** | 1–99 | 10 | decisions index | INTELL only (v1.1; greyed at v1) |
| **WIDTH** | 1–99 | 10 | crossfade index | INTELL only (v1.1; greyed at v1) |
| **MON1 / POL2** | MON1 / POL2 | POL2 | — | S950 only |
| **autC / AUTO-D** | trigger | — | — | CYCLIC & S950 |
| **BANDWIDTH** *(non-automatable in FX)* | 3.0–19.2 (S950) / 3.0–16.0 (S900) | max | kHz | S900 / S950 |
| **FS** *(non-automatable)* | 44.1 / 22.05 | 44.1 | kHz | S1000/S1100 |
| **CHARACTER** | ON / OFF | ON | — | all (bypasses the per-model character chain) |
| **NORM** | OFF / ON | **OFF** | — | SAMPLE renders (OFF = authentic) |
| **SYNC** | OFF / HOST | OFF | — | FX mode (`TIME FACTOR := 100 × sourceBPM / hostBPM`) |
| **WINDOW** | 1/4, 1/2, 1, 2, 4, 8 bars / FREE | 1 bar | bars | FX mode |
| **OUTPUT** | -24–+12 | 0 | dB | all |

Notes:

- **CLASSIC vs REVISED timing.** CLASSIC uses an integer-hop schedule — pitch is
  perfect but the output length is quantized (this is the hardware-faithful path
  the golden tests pin). REVISED uses a fractional hop for sample-exact length.
- **CHARACTER OFF** bypasses the per-model bit-depth/clock/filter chain — useful
  for clean stretching or for FX null tests at TIME FACTOR = 100%.
- **NORM** defaults OFF because no Akai manual documents normalization; ON
  reproduces the Akaizer v1.3 convenience behavior. **OUTPUT** is a plain output
  trim; there are deliberately no per-model level offsets (they would break A/B
  null tests).

---

## Troubleshooting

**`** NOT ENOUGH MEMORY **` when I press GO.**
The render output length is capped at **10 minutes at the model rate per channel**.
A long source at a high TIME FACTOR (e.g. 2000% of a multi-minute file) exceeds
the cap and the render is refused with this hardware-idiom message. Reduce the
zone length, lower the time factor, or render in sections.

**Latency / timing shifts when I switch models (PDC).**
Reported latency depends on the model rate, so switching models (or changing
bandwidth / FS) recomputes it and re-reports it to the host. This is a
**plugin-delay-compensation (PDC) updating action**. Some hosts honor mid-session
latency changes cleanly; others need the transport stopped, the track
reselected, or the project reloaded to pick up the new latency. If FX timing
seems off right after a model switch, stop and restart playback.

**My stereo file sounds mono on the S900 or S950.**
That is authentic. The **S900 and S950 engines process a mono sum** of the input
(the S950 is a mono machine). The LCD shows the mono-sum notice when those models
are selected. For stereo-preserving stretching use the **S1000** or **S1100**.

**TIME FACTOR won't go below 100% in FX mode.**
In FREE (non-synced) FX mode, compression (< 100%) needs future input the plugin
does not have, so the engine clamps to 100% and the LCD shows `FX MIN 100%`. Your
automation value is preserved — only the engine clamps. To compress in real time,
switch SYNC on and use a WINDOW (the captured window plays compressed, then
silence to the boundary), or compress offline in SAMPLE mode.

**Expansion drifts / I hear a jump in FX mode.**
At TIME FACTOR > 100% the read head lags the write head and the lag grows over
time. mwStime bounds this with a 30-second history; when it is exhausted the read
head resyncs at the next grain boundary, which is audible. Use **SYNC** with a
WINDOW for transport-locked, drift-free expansion.

**Logic Pro won't send MIDI to the plugin.**
mwStime is a **music effect** (`aumf`), not an instrument. Insert it on an audio
track. MIDI repitch is an *enhancement* — audition is always available from the UI
(the **PLAY** soft key or clicking the waveform), so MIDI is never the only way to
trigger playback.

**The render reloaded differently / I expected the audio in my session.**
Renders are deterministic for a given (source, parameters, engine version). By
default mwStime stores the render *recipe* and re-renders on load rather than
embedding the audio (small embedded audio is the default for files ≤ 16 MB
encoded). If your source file moved, reload it.

---

## Accessibility

All controls are focusable; arrow keys + Enter mirror the cursor/ENT keys, and the
jog wheel responds to the mouse wheel and Up/Down. Parameter tooltips show both
the hardware unit and the host-normalized value, and screen-reader names match the
LCD field labels. The UI is resizable (vector graphics) and stays crisp at any
scale.
