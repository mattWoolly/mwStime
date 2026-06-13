---
id: 035
title: MIDI repitch voice — monophonic real-time variable-rate playback with per-note filter retune
status: done
depends-on: [016, 034]
component: plugin
estimated-size: M
---

## Objective
Chromatic MIDI audition in SAMPLE mode: a monophonic, last-note-priority voice doing a
real-time variable-rate read over the published render buffer; on S900/S950 the
per-voice virtual clock and tracking-filter coefficients recompute on note-on on the
audio thread.

## Context
Read first:
- docs/design/architecture.md §4.2 (the whole section — this is explicitly NOT the
  offline transpose stage; own RT-safety tests; mono last-note v1 (PI)), §7 (MIDI
  routing reality: aumf; MIDI is an enhancement, UI audition always works)
- docs/design/dsp-engine.md §8.1 (per-note retuning: closed-form biquad coefficients,
  no allocation)
- docs/design/ui-design.md §6.3 step 3 (root C3, chromatic repitch, monophonic)
- Butterworth retune (005 via 016), SamplePlayer/published render (034)

## Scope
- `plugin/src/MidiVoice.{h,cpp}`:
  - note-on: rate = `2^((note−60)/12)`; variable-rate read head over the published
    RenderedSample (linear-interp read for S1000/S1100 models; ZOH for S900/S950 —
    matching each model's playback paradigm),
  - S900/S950: per-note virtual clock = modelClock × rate; recompute the 6th-order
    tracking Butterworth coefficients closed-form on the audio thread at note-on
    (no allocation),
  - monophonic last-note priority, short declick fade on steal/note-off (PI, ~5 ms),
  - host MIDI and UI audition triggers (PLAY soft key / waveform click —
    architecture.md §7; there is no on-screen keyboard in the design) feed the same
    event queue (UI wiring lands in 045b).
- Tests (`tests/plugin/test_midivoice.cpp`):
  - C3 plays at rate 1.0 (output matches SamplePlayer playback), C4 at 2.0 (length
    halves, 440→880 spot check),
  - last-note priority: overlapping notes steal correctly, no click > −40 dBFS
    discontinuity at the steal point,
  - S950 model: note-down a fifth shifts the measured filter cutoff proportionally
    (clock tracking audible property — DRR F4/F7),
  - RT-safety: note-on path allocation-free (debug allocator hook); no locks.

## Out of scope
- Polyphony (additive later — architecture.md §4.2), MPE, sustain pedal.
- FX-mode MIDI behavior (none at v1).

## Acceptance criteria
- [ ] Tests pass incl. the clock-tracking property and RT-safety checks.
- [ ] Voice reads only published buffers via the task-030 copy-once protocol.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R midivoice --no-tests=error
```
