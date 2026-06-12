# ADR 001: DSP engine architecture — pure core library, shared cyclic engine, per-model configuration

Status: accepted
Date: 2026-06-12

## Context

We need per-model emulation of S900/S950/S1000/S1100 timestretch with era-correct
artifacts (locked), implementable as small independent backlog tasks by autonomous
agents (ORCHESTRATION phases 3–4), and testable without a DAW. The research shows one
underlying algorithm family — the cyclic two-grain splice with linear crossfades and
an integer (CLASSIC) or fractional (REVISED) input hop
(docs/research/akaizer-analysis.md §3, §4.2, §10) — with per-model differences living
mostly in parameters and the conversion/filter character chain
(docs/research/akai-manuals-specs.md §§6–7). The exact crossfade shape/overlap and the
exact step formula are not recoverable without disassembly (akaizer-analysis.md §2.4),
and INTELL internals are entirely unverified (deep-research-report.md Caveats #1).

## Options considered

All three panel lenses independently proposed a **pure C++20 JUCE-free static core
library** with per-model data tables — that part was unanimous. They diverged on
execution model, INTELL, and arithmetic guarantees:

- **Authenticity purist**: offline render-to-buffer is the canonical path; real-time
  preview/FX are thin front-ends over the identical core; INTELL not shipped at all
  at v1 (ship the documented autC helper only); CLASSIC core integer-only with
  bit-exact cross-platform goldens for the whole chain. Critique findings adopted:
  the bit-exact claim collapses once float filters/SRC are inside the "core" — scope
  it to the integer stretch path; the -18 dB/oct LPF is the *voice* filter, not a
  reconstruction stage (hard-wiring it would fabricate an always-on artifact).
- **Product designer**: real-time streaming CYCLIC as the default and the product
  (potenza proves it runs real-time, akaizer-analysis.md §5); INTELL ships at v1 as a
  background-render WSOLA approximation labeled as such. Critique findings adopted:
  v1 scope explosion vs the atomic-agent model (INTELL is the obvious stage-2 cut);
  two of its golden-test properties failed by design against its own arithmetic.
- **Pragmatic engineer**: one pure offline function, wrapped twice (offline renderer +
  streaming wrapper); INTELL deferred to v1.x; splice constants as constructor
  calibration parameters. Critique finding adopted: its text conflated the OpenMPT
  overlap scheduler (hop_in = round(C·(1−F)/T)) with the Akaizer splice model
  (step = round(C·100/T)) — two different scheduling models whose conflation yields a
  ~20% stretch error and mutually inconsistent length tests; one model must be chosen
  explicitly and all tests derived from it.

Disagreements and resolutions:
1. *Realtime-first vs offline-reference*: the hardware semantics (CLASSIC integer
   schedule, quantized length) are offline-native (akaizer-analysis.md §2.2, §9, §10);
   golden tests must pin a deterministic reference. Resolution: offline
   `CyclicEngine`/`OfflineRenderer` is the authenticity reference; `RealtimeStretcher`
   is a documented-deviation streaming wrapper over the *same* grain scheduler
   (ADR-006), with a stream/offline equivalence test. FX-first remains the product
   default (a shipping-surface statement, not a build-order constraint).
2. *INTELL at v1*: deferred to v1.1 (purist + engineer positions, plus the critique
   of the product proposal). Shipping a guess under the authentic name is the
   plausible-fake failure mode this project exists to avoid; v1 greys qual/width
   exactly as the hardware does for CYCLIC (akai-manuals-specs.md §3 p.47). The
   approximation spec is retained in dsp-engine.md §4 for v1.1, to be ADR'd as a
   labeled approximation when it ships.
3. *Scheduling model*: adopt the OpenMPT two-grain overlap scheduler
   (akaizer-analysis.md §4.2) as the reference implementation — the only fully
   specified, line-cited open arithmetic — and make overlap fraction F, fade shape,
   and hop rounding **constructor calibration parameters** (`SpliceCal`) so hardware
   calibration is data-only (dsp-engine.md §3.0).
4. *Bit-exactness*: claimed cross-platform only for the integer CLASSIC stretch path
   (transpose 0, character OFF); float stages are reference-platform-exact with
   tolerance compares elsewhere (testing-strategy.md §4).

## Decision

Single shared `CyclicEngine` + per-model `ModelSpec` configuration + orthogonal
`CharacterChain`, all in `libs/mwstime-core` (pure C++20, zero non-stdlib deps);
signal-flow placement: ingest character → stretch → transpose → playback character →
optional normalize (default OFF, ADR-006). Engines: CyclicEngine (CLASSIC default /
REVISED labeled modern), S950Engine, RepitchEngine (S900, ADR-003); IntellEngine
deferred to v1.1. Pseudocode contracts pinned to akaizer-analysis.md §4.2; every
unverifiable constant tagged (PI) and centralized in `SpliceCal` / `ModelSpec`.

## Consequences

- Agents can implement and test any engine with only the core lib + dsp-engine.md.
- JUCE never leaks below `plugin/`; a future non-JUCE frontend is cheap.
- Shared-core regressions are caught by the per-model golden matrix
  (testing-strategy.md §4), a hard gate we must maintain.
- The unknown splice constants are our (PI) choices until hardware captures exist;
  revising them is a `SpliceCal` data change + golden re-bless with an ADR/issue
  trail. Akaizer renders are secondary corroboration only, never the calibration
  target (its fidelity claim was refuted 0-3 — deep-research-report.md Finding 6;
  testing-strategy.md §7 Wave 2).
- INTELL absence at v1 must be communicated in the UI (greyed fields) and docs.
