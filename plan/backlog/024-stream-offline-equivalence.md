---
id: 024
title: Stream/offline equivalence test — one core, two front-ends, falsifiable
status: done
depends-on: [020, 022]
component: qa
estimated-size: S
---

## Objective
The hard test that the FX path and the authentic path share one scheduler: with
parameters frozen from a known start state, RealtimeStretcher's streamed output over a
window equals OfflineRenderer's render of the same history, sample-for-sample.

## Context
Read first:
- docs/design/testing-strategy.md §3 item 4b (the property statement)
- docs/design/architecture.md §4.1 (the equivalence demand), §5.2
- plan/decisions/006-fx-vs-sample-mode.md ("stream/offline equivalence is a hard test")
- docs/design/dsp-engine.md §3.5 (equivalence test contract paragraph — comparable
  region, params frozen, changes only at grain boundaries)
- OfflineRenderer (020), RealtimeStretcher (022)

This task is pure tests; if equivalence fails, the fix belongs in 022 (the streaming
front-end must conform to the offline reference, never the reverse — goldens pin the
offline path).

## Scope
- `tests/unit/test_stream_offline_equivalence.cpp`:
  - construct a known start state: feed K samples of deterministic input into
    RealtimeStretcher (FREE, T=300%, CLASSIC, character OFF), freeze params,
  - render the same K-sample history through OfflineRenderer with identical
    ParamSnapshot,
  - align by the reported latency/known read-head position and assert sample-for-sample
    equality over the comparable region (define the region explicitly in the test,
    per dsp-engine §3.5),
  - repeat for REVISED hop mode and for one S950 case,
  - repeat with character ON at matching host==model rate (tolerance ≤ 1e-6 if float
    stages are involved; exact for the integer path),
  - **host-rate matrix / model-rate invariance** (dsp-engine §3.5: "the sound matches
    the offline render at any host rate"): repeat the CLASSIC character-ON case at
    44.1/48/96 kHz host rates — streamed output equals the offline render at each
    rate (tolerance ≤ 1e-6 for the SRC stages),
  - **stereo over the FX pass** (testing-strategy §3.5 render/FX requirement): one
    equivalence case with stereo input of differing channel content — stream equals
    offline per channel and the hop schedule is identical across channels.
- Document the alignment derivation in test comments (this is the trickiest part —
  future agents must not "fix" failures by loosening alignment).

## Out of scope
- Any production behavior change beyond conformance fixes in RealtimeStretcher
  (coordinate: if a real fix is needed, it stays inside this task's PR only if small;
  otherwise file a blocker).

## Acceptance criteria
- [ ] Test exists, tag `[equivalence]`, passes for CLASSIC, REVISED, S950 cases.
- [ ] Comparable-region definition is explicit and cited in comments.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R equivalence --no-tests=error
```
