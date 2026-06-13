---
id: 025
title: mwstime-render CLI — wav in, wav out, all params
status: done
depends-on: [003, 020]
component: engine
estimated-size: M
---

## Objective
`tools/mwstime-render`: a JUCE-free command-line renderer over OfflineRenderer +
WavIo — the driver for golden tests and the agent debugging tool (DSP work never
requires a host).

## Context
Read first:
- docs/design/architecture.md §2 (tools layer rules), §8 (tools/mwstime-render/
  layout)
- docs/design/testing-strategy.md §4 (runner contract: `mwstime-render --case <id>`
  reads tests/golden/cases.json)
- docs/design/dsp-engine.md §2 (parameter names/units — CLI flags mirror these)
- WavIo (003), OfflineRenderer (020), ModelSpec/ParamSnapshot (009 via 020)

## Scope
- `tools/mwstime-render/CMakeLists.txt` + `main.cpp` (+ small arg/JSON helpers; a
  minimal vendored or hand-rolled JSON reader is fine — no heavyweight deps):
  - direct mode: `mwstime-render --in in.wav --out out.wav --model s1100
    --time-factor 300 --cycle-len 1000 --hop-mode classic --transpose 0
    --character on --norm off --material pol2 --bandwidth 19.2 --fs 44.1`
    (every dsp-engine §2 param relevant to a render has a flag; defaults per table),
  - case mode: `mwstime-render --case <id> --cases tests/golden/cases.json
    --inputs-dir ... --out ...` (schema defined here, populated in task 026),
  - prints RenderInfo (achieved length/ratio, monoSummed, engine version) to stdout;
    nonzero exit + stderr message on refusal (NotEnoughMemory) or bad args,
  - deterministic: identical invocation ⇒ bit-identical output file.
- CTest smoke test: render a generated sine through one S1000 case, assert exit 0 and
  the output parses via WavIo with the schedule-derived length.
- Enable the `tools/` add_subdirectory left placeholder in task 001.

## Out of scope
- The golden corpus, comparer, bless target (task 026).
- Any JUCE dependency (hard rule, architecture.md §2).

## Acceptance criteria
- [ ] `mwstime-render --help` documents every flag with hardware units.
- [ ] Direct-mode render of a test WAV succeeds for all four models.
- [ ] CTest smoke test `[rendercli]` passes; binary links only mwstime_core.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R rendercli --no-tests=error
./build/default/tools/mwstime-render/mwstime-render --help
```
