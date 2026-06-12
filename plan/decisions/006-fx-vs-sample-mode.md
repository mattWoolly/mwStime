# ADR 006: FX mode vs SAMPLE mode — one scheduler, two front-ends, explicit causality contract

Status: accepted
Date: 2026-06-12

## Context

Locked: hybrid **FX-first** plugin. But the hardware timestretch is exclusively an
**offline render to a new named sample** (akai-manuals-specs.md §§2–4;
akaizer-analysis.md §9), and the CLASSIC mode's defining traits — integer hop
schedule, quantized output length — only have exact meaning offline
(akaizer-analysis.md §2.2, §10). A real-time mode necessarily deviates, and live
streaming has hard physics: at wall-clock t the plugin has received t seconds of
input, so **expansion (T>100%) is causal but the read position lags without bound**
(lag grows at 1−100/T per unit time), while **compression (T<100%) requires future
input and is impossible as a pure insert for every T below 100%**, regardless of any
fixed reported latency. The only zero-drift true insert is T = 100% (+ transpose).

## Options considered

Panel positions:
- **Authenticity purist**: FX = capture-then-stretch (host audio recorded into a
  machine-domain buffer, stretched playback out), "not a causal live stretcher,
  because a causal live stretcher at T≠100% is physically impossible"; no fake
  lookahead latency. Its critique found the stream-vs-rerender story internally
  contradictory and T<100% behavior undefined — fixed here by the contract table.
- **Product designer**: split semantics — true streaming insert plus a tempo-synced
  capture buffer ("stretch the last N bars"). Its critique caught that the proposal
  had the physics **inverted** (it claimed T≤100% streams and T>100% is impossible —
  the arithmetic is the opposite); the capture-buffer idea survives, applied to all
  T≠100%, and its critique also demanded the locked FX-first priority not be
  silently demoted (P7).
- **Pragmatic engineer**: streaming wrapper over the same grain scheduler with a
  bounded ring + grain-boundary resync, beat-quantized hard resync in sync mode. Its
  critique showed the ring sizing formula bounded the wrong quantity (drift is
  unbounded in time, not cycle size), that T<100% was never restricted, that latency
  ignored model-rate scaling (2000 samples @ 7.5 kHz ≈ 267 ms) and SRC group delay,
  and that "ship loaded-sample mode as the flagship" reversed the locked decision.

Engine-sharing options:
- **A. Realtime-only engine; SAMPLE mode = realtime engine rendered ahead.** Bakes
  realtime compromises into the authentic path; goldens would pin the compromise.
- **B. Offline-only (no FX at v1).** Contradicts the locked FX-first decision.
- **C. Two thin front-ends over ONE grain scheduler: `OfflineRenderer` (authentic,
  golden-tested, GO/abort/progress workflow) and `RealtimeStretcher` (streaming, with
  an explicit causality contract), plus a stream/offline equivalence test.** Chosen —
  satisfies both authenticity and FX-first, with deviations as documented product
  behavior, not silent drift.

## Decision

Option C, with the **FX causality contract** as the normative spec (architecture.md
§5.2, dsp-engine.md §3.5). FX mode remains the default `pluginMode` and a first-class
product surface (locked FX-first — explicitly NOT demoted):

| Case | FREE (no transport sync) | SYNC (`fxWindow` = 1/4…8 bars, default 1 bar (PI)) |
|---|---|---|
| T = 100% | pure delay of exactly the reported latency (null-testable, character OFF) | same |
| T > 100% | read head lags up to the 30 s (PI) history bound; on exhaustion, jump to `writePos − latency` at the next grain boundary (documented audible resync) | hard resync to the window start at every transport-aligned window boundary — "stretch the last bar" |
| T < 100% | engine-clamped to 100%; LCD shows `FX MIN 100%` (automation values preserved) | allowed: captured window plays compressed, then silence to the boundary (PI) |

Further contract terms:
- **Latency** = `ceil(2000 × hostRate / modelRate) + crossfadeLen + SRC group delay`;
  recomputed in prepareToPlay and on model/bandwidth/FS change (all non-automatable);
  cycle-length automation never changes latency. Host PDC inconsistency on
  mid-session changes is documented behavior.
- **No transport/Standalone**: FREE rules; SYNC falls back to wall-clock window at
  last-known tempo or 120 BPM (PI).
- Parameter changes apply at grain boundaries — no smoothing inside the authentic
  scheduler; **stream/offline equivalence** is a hard test (frozen params from a
  known start state ⇒ sample-identical output, testing-strategy.md §3.4b).
- S900 FX mode: varispeed rate > 1 clamps in FREE mode (same causality, ADR-003).
- **SAMPLE mode (authentic)**: load/drop → edit with live ZONE preview → GO renders
  offline on the worker thread with progress + hold-F8 abort + memory-cap refusal
  (manual-faithful workflow, akai-manuals-specs.md §3 pp.45–47) → audition/A-B →
  drag-out WAV. Renders are deterministic. **Normalization default OFF** (authentic —
  no manual documents it and OpenMPT doesn't normalize; `norm = ON` reproduces
  Akaizer v1.3 behavior as a modern opt-in; panel-revised from the earlier
  normalize-by-default draft).
- One parameter set serves both modes; FX-inapplicable controls (GO/PLAY/A-B) disable
  in FX mode.

## Consequences

- Golden character tests pin `OfflineRenderer`; FX mode gets the contract tests
  (null at T=100%, clamp/window behavior, resync placement, latency formula,
  stream/offline equivalence) instead.
- Users get an honest split: "what the hardware did" (SAMPLE) vs "what the hardware
  never could" (FX), with identical sonic character from the shared scheduler.
- The 30 s history, window set, and silence-fill compression behavior are product
  constants (PI); changing them is state-compatible but needs release notes.
- The contract must be implemented before any processor code touches FX behavior —
  it is the backlog task spec, closing the underspecification all three critiques
  flagged.
