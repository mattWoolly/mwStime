# ADR 002: Plugin formats at v1

Status: accepted
Date: 2026-06-12

## Context

plan/ORCHESTRATION.md names VST3/AU/CLAP/LV2 in the project statement, locks JUCE 8 +
AGPLv3, and requires macOS + Linux with Windows third. JUCE 8 natively exports VST3,
AU, LV2 and Standalone; CLAP requires the free-software `clap-juce-extensions` shim
(unofficial, can lag JUCE releases). AAX requires a closed Avid agreement. The plugin
also wants MIDI input (audition/repitch in SAMPLE mode) while being an audio effect —
a known format-category minefield (AU `aufx` receives no MIDI in Logic; `aumf` is
required).

## Options considered

Panel positions:
- **Authenticity purist**: all four (VST3 + AU + LV2 + CLAP) at v1 — the AGPLv3/Linux
  audience expects LV2 + CLAP; clap-juce-extensions is "near-free". Its critique
  flagged that "pluginval at max strictness on every format" is impossible (pluginval
  loads only VST3/AU) and that "near-free" understates the shim's maintenance cost.
- **Product designer**: all four + Standalone at v1, AAX never. Its critique raised
  the MIDI-effect category problem (P2) — unaddressed in the proposal — and the
  missing CLAP/LV2 validators (P11).
- **Pragmatic engineer**: VST3 + AU + CLAP at v1, **LV2 deferred to v1.1** (JUCE's
  LV2 export is its flakiest; Linux is served day one by VST3 + CLAP). Its critique
  also caught the pluginval/CLAP error.

Full option space:
- **A. VST3 + AU only at v1.** Smallest matrix, but undercuts the locked Linux-first
  goal and the project statement's explicit format list.
- **B. All four (VST3, AU, LV2, CLAP) + Standalone at v1.** The locked project
  statement names all four; integration cost is one `FORMATS` entry plus one CMake
  module; AGPLv3 is compatible with all (VST3 GPLv3 option, CLAP MIT, LV2 ISC). Cost:
  a four-format QA matrix — absorbed by the corrected per-format validator plan.
- **C. Defer LV2 to v1.1 (engineer's position).** Pro: avoids JUCE's least-mature
  exporter blocking v1. Con: Ardour (a named Linux smoke-test host) is LV2-native,
  the project statement names LV2, and the validator cost (lv2lint) is small.
- **D. CLAP-first without JUCE wrappers.** Rejected: contradicts the locked JUCE 8
  decision.

Disagreement recorded: LV2 at v1 (purist, product) vs v1.1 (engineer). Resolution:
v1 — the locked project statement lists LV2, the Linux requirement is first-class,
and the risk is bounded: LV2 is validated by lv2lint/lv2_validate + Ardour smoke, and
if the JUCE LV2 exporter proves broken late, dropping LV2 from a release is a
one-line change (the reverse — adding it under pressure — is not).

## Decision

v1 ships **VST3 (macOS/Linux), AU v2 (macOS, exported as `aumf` music effect so MIDI
routes in Logic — architecture.md §7), LV2 (Linux + macOS), CLAP (all), and the
Standalone app**, built from one `juce_add_plugin` target plus
`clap_juce_extensions_plugin` (pinned tag; bumps are explicit backlog tasks). Windows
builds of the same formats come third, per the locked platform order. AUv3 and AAX are
out of scope for v1.

Validation plan (corrected — pluginval covers VST3/AU only): pluginval strictness 10
(VST3/AU) + `auval -v aumf` + **clap-validator** (CLAP) + **lv2lint/lv2_validate**
(LV2), per testing-strategy.md §5.

## Consequences

- One codebase, five artifacts; release scripts and (later) CI matrix over them with
  three distinct validators, xvfb on headless Linux.
- MIDI behavior in hosts that refuse MIDI-to-effects degrades gracefully (UI-triggered
  audition always works; MIDI is an enhancement — architecture.md §7).
- clap-juce-extensions version risk is owned explicitly (pinned, task-gated bumps).
- AAX users are unserved unless a future ADR revisits licensing.
