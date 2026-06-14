---
id: 052
title: README + user-facing docs — what it is, authenticity boundary, building, manual
status: in-review
depends-on: [046, 048, 049]
component: docs
estimated-size: S
---

## Objective
Public-facing documentation: a README that explains the plugin, the per-model
authenticity story (including "the S900 had no timestretch" and "INTELL arrives in
v1.1"), install/build instructions, and a short user manual mapped to the UI flows.

## Context
Read first:
- plan/ORCHESTRATION.md (project statement, AGPLv3, format list)
- docs/design/architecture.md §9 (authenticity vs modern-UX boundary table — the
  honest-marketing source), §3 (formats)
- plan/decisions/003 (S900 honesty — docs/UI must communicate it), 004 (S3000 v1.1),
  006 (FX vs SAMPLE split: "what the hardware did" vs "what it never could")
- docs/design/ui-design.md §6 (flows → manual sections)
- docs/BUILDING.md (created in 049; 050 adds the Windows section later) — link,
  don't duplicate

## Scope
- Rewrite `README.md`: what/why, model table (S900 varispeed honesty, S950/S1000/
  S1100, S3000 "coming in v1.1"), FX vs SAMPLE modes and the causality contract in
  user terms (latency, FX MIN 100% clamp, SYNC windows), formats + host notes
  (Logic = music effect aumf), screenshots (vector UI, macOS), AGPLv3 notice,
  build-from-source quickstart linking docs/BUILDING.md, credits/citations pointing
  at docs/research/.
- `docs/MANUAL.md`: the four ui-design §6 flows as user instructions (load, set
  stretch incl. autC and SYNC, render/audition/export, FX mode), parameter reference
  generated from the dsp-engine §2 table (hardware units), troubleshooting (PDC on
  model switch, mono-sum on S900/S950, NOT ENOUGH MEMORY cap).
- Authenticity statement section: what is research-pinned vs (PI)-invented vs
  deliberately deviated (ADR-006), and that "authentic" labeling is gated on hardware
  captures (testing-strategy §7 Wave 2) — honest framing, no overclaim.

## Out of scope
- Developer/architecture docs (already in docs/design/).
- Release notes/changelog automation.

## Acceptance criteria
- [ ] README accurate against the shipped v1 behavior (spot-check every claim).
- [ ] S900 no-timestretch and INTELL-deferred stated plainly.
- [ ] All links resolve; markdownlint passes (pinned invocation below).
- [ ] Build instructions verified by following them verbatim on a clean clone.

## Verification commands
```
npx --yes markdownlint-cli2@0.13.0 README.md docs/MANUAL.md
# follow README quickstart verbatim from a clean clone:
git clone . /tmp/mwstime-readme-check && cd /tmp/mwstime-readme-check
cmake --preset default && cmake --build --preset default && ctest --preset default
```
