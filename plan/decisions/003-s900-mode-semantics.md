# ADR 003: S900 mode semantics — honest repitch (varispeed), no fake timestretch

Status: accepted
Date: 2026-06-12

## Context

The model selector must include the S900 (locked decision), but the S900 has **no
timestretch**: the operator's manual contains zero occurrences of "stretch", and even
OS 4.0 only added TIME SKEW, which is not a pitch-preserving stretch
(docs/research/akai-manuals-specs.md §1; deep-research-report.md Finding 3). What does
the stretch UI do when S900 is selected?

## Options considered

Panel positions — **unanimous** on the substance, with one product-process flag:
- **Authenticity purist**: "S900 mode = no timestretch, and we say so on the panel" —
  vari-speed repitch through the 12-bit/SC-filter path is the classic pre-1988
  break-pitching workflow.
- **Product designer**: "honest vari-speed … the LCD literally shows 'NO TIMESTRETCH
  ON S900' — that's the screenshot"; doubles as the plugin's best pure-character mode
  (the RX950 use case, deep-research-report.md Finding 7).
- **Pragmatic engineer**: same; TIME SKEW stays out (forum-grade evidence only).
- Critique flag (purist review, P15): ORCHESTRATION locked S900 into the model
  selector of a *timestretch* plugin — users may read the selector as four stretch
  flavors, so the **owner should explicitly ratify** shipping a model whose headline
  feature is absent. Carried as a consequence below.

Option space:
- **A. Backported cyclic stretch ("what it would have sounded like").** Pro: uniform
  UX. Con: pure fabrication — violates the locked "authentic DSP core" and the
  ORCHESTRATION traceability rule; indistinguishable from S950 mode minus D-TIME.
- **B. S900 = repitch/varispeed mode: time factor maps to playback rate, pitch
  follows (LCD shows the resulting semitone offset), through the 12-bit / 7.5–40 kHz
  variable-clock character chain.** Historically honest — precisely how S900 users
  "stretched" breaks (akaizer-analysis.md §6 tracker/Akai workflow note); the
  per-voice variable-clock DAC + tracking Butterworth character is
  service-manual-grade verified (deep-research-report.md Finding 4) and is the S900's
  actual sonic identity. Stretch-specific params go inert (LCD field visibility,
  ui-design.md §3).
- **C. Emulate TIME SKEW as well.** Rejected for v1: forum-grade documentation only
  (deep-research-report.md caveat 6 / open question 4); revisit if research improves.

## Decision

S900 mode is **RepitchEngine**: `rate = 1/T`, pitch coupled (displayed as
`-12·log2(T)` semitones), transpose multiplies the same rate; the 12-bit quantize +
variable-clock playback + clock-tracking 6th-order Butterworth chain provides the
model character, including pitch-tracking filter/aliasing behavior (the feature RX950
notably lacks — deep-research-report.md Finding 7). The LCD states "S900: NO
TIMESTRETCH — VARISPEED". Stretch-only parameters are hidden on the S900 faceplate.
FX-mode causality: rate > 1 is clamped in FREE mode exactly like T < 100%
(ADR-006). Spec: docs/design/dsp-engine.md §6, §8.1.

## Consequences

- The plugin's docs/UI must communicate "the S900 had no timestretch" (the LCD does).
- **Owner ratification item**: the panel flagged user-expectation risk on the model
  selector; the owner should confirm (or this ADR stands as the record of the
  research-faithful default).
- Users wanting S900 grit *with* stretch select S950 (12-bit chain + real stretch) —
  a discoverable, honest workaround.
- TIME SKEW remains an open research question; adding it later is a new ADR + engine,
  no breaking change.
