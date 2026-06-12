# ADR 004: Fifth model — S3000 confirmed, shipped as a v1.1 fast-follow

Status: accepted
Date: 2026-06-12

## Context

Locked: "S900, S950, S1000, S1100 minimum; add others only if research shows distinct
sonic benefit." Candidates surfaced by research: S2800/S3000/S3200 family (1992),
S2000 (1995), S01, S5000/S6000, MPC line. Separately: does the fifth model ship at v1
or after?

## Options considered

Panel positions — unanimous **that** the S3000/S3200 clears the bar, split 2-1 on
**when**:
- **Authenticity purist**: yes, but **v1.1 fast-follow** — "the locked scope is four
  models, and the architecture makes it a data-plus-one-filter task later, not a
  redesign. Write the ADR now reserving the fifth faceplate slot."
- **Product designer**: yes, **at v1** — "engine cost is near zero … five faceplate
  themes, one engine, two character paths"; the Playford lineage is "the marketing
  copy writing itself." Its critique, however, listed S3000 among the obvious
  stage-2 cuts in a v1 scope that "cannot be decomposed into atomic single-agent
  tasks without a long serial dependency chain" (P5).
- **Pragmatic engineer**: yes, **v1.1** — "it adds zero v1 sound value beyond the
  filter" and the locked scope is four models.

Evidence that the bar is cleared (all panel lenses agree, docs/research/
akai-manuals-specs.md §5, §7; deep-research-report.md Finding 11):
- distinct voice filter: digital moving LPF **-12 dB/oct with resonance** vs the
  S1000/S1100's -18 dB/oct non-resonant — a manual-stated character change;
- distinct conversion path: 16-bit ADC 64× oversampling, **28-bit internal
  accumulation**, 18-bit 8× oversampled DACs;
- the only manual documenting concrete stretch defaults (cycle 1000, time 100%,
  qual 10, width 10) and a frank artifact description ("echo or 'flam' effect");
- cultural relevance: Rob Playford's Goldie-era chain ended at the S3200.

Other candidates: S2000 (same engine, less distinct character, weaker cultural pull —
dominated), MPC60/S6000 (different families, no algorithm evidence gathered —
deep-research-report.md open question 3).

Disagreement resolution (v1 vs v1.1): per the synthesis tiebreakers — (a) research
evidence is timing-neutral; (b) **implementability by small autonomous agents favors
deferral** (the v1 surface already carries four models, two engine variants, the FX
contract, and the format matrix; every critique flagged scope as a top risk);
(c) the locked decision sets a four-model *minimum* and a bar for additions — it does
not require the addition at v1. Resolution: **v1.1**, with the slot reserved now so
deferral costs nothing.

## Decision

The fifth model is the **S3000** (representing the S2800/S3000/S3200 family),
shipping in **v1.1**. At v1: `ModelId::S3000` and a fifth faceplate badge position
are reserved (data slots, no behavior). At v1.1: `ModelSpec` + character-chain delta
(28-bit accumulation ⇒ no intermediate quantize; -12 dB/oct resonant SVF, resonance
fixed at minimum) + `FaceplateSpec` + the documented factory-default preset/golden
case. Spec: docs/design/dsp-engine.md §1, §8.3.

## Consequences

- v1 golden matrix and faceplate count stay at four models; v1.1 adds one of each
  (bounded, data-driven cost — ADR-001 architecture makes models data, not code).
- The S3200/S2800 are represented by the S3000 badge; we do not claim per-sibling
  differences (the manual covers all three identically).
- The S3000 "factory default" preset (the only manual-printed defaults anywhere)
  arrives with the model at v1.1.
- Resonant-filter user exposure is deferred further; enabling it later is additive.
