# Akaizer secondary cross-check — local-only corroboration procedure

Status: accepted, 2026-06-13 (task plan/backlog/026c-akaizer-crosscheck.md).

> **This is a SECONDARY, LOCAL-ONLY cross-check. It is NEVER run in CI and is NOT a
> pass/fail gate.** Akaizer's "near-exact to hardware" fidelity claim was *refuted
> 0-3* (docs/research/deep-research-report.md Finding 6). The **primary oracle for
> any "authentic" labeling is hardware captures** (task 026b; ADR-001 consequence),
> not Akaizer. This procedure exists only to *corroborate* — to flag where our
> CLASSIC engine and a locally produced Akaizer CLASSIC render disagree, so a human
> can decide whether the deviation is interesting. **Deviations are documented, not
> fixed-by-default.**

It is grounded in docs/design/testing-strategy.md §7 (Wave 2, the Akaizer-demotion
paragraph) and §8 (Akaizer cross-check explicitly excluded from CI),
docs/design/dsp-engine.md §7.3 (peak-normalize both sides), and
docs/research/akaizer-analysis.md §1/§2.1/§2.3 (closed payware; normalizes since
v1.3; CLASSIC computes an integer step length).

## Why this can never live in CI

Akaizer is **closed payware** — currently £10 on PayHip, Windows-only, no source, no
redistributable license (akaizer-analysis.md §1). The binary, its renders, and the
`research-cache/` scratch directory **cannot be committed or fetched**:

- `research-cache/` is **gitignored** (`.gitignore`: "Downloaded third-party
  sources/binaries used for research (not redistributable)"). Keep every Akaizer
  artifact under it.
- Nothing Akaizer-related appears in `.github/` or `tests/CMakeLists.txt`. The 051
  CI task forbids it; keep it that way.
- The only Akaizer-related automated test is the `akzcheck` self-test of the
  *comparison math* on synthetic data, which needs **no Akaizer binary** and is
  registered from `tools/akaizer_crosscheck/CMakeLists.txt` (not the test tree).

## Scope of the oracle: T 120–2000% only

Akaizer's author verified CLASSIC against real hardware **mainly for Time Factor
between 120% and 2000%** at any Cycle Length 20–2000 (akaizer-analysis.md §2.2).
Therefore the cross-check has **no Akaizer oracle** for:

- compression (T 25–119%),
- S950 D-TIME semantics (no Akaizer equivalent),
- anything the engine clamps out of the window (e.g. an S950 case authored at 2000%
  clamps to 999% — the tool compares using the *clamped* value).

`akaizer-crosscheck` automatically **skips** out-of-window cases and records an
explicit "no Akaizer oracle" note for each in the deviation report. Do not hand-edit
those rows into comparisons.

## What is compared (analytic, not a null test)

Three quantities, all derivable from the dsp-engine.md §3.4 CLASSIC scheduler, so the
*math* is testable without Akaizer (`akzcheck`):

1. **Splice-comb / flutter frequency** — the "metallic ring" sideband spacing,
   predicted as `modelRate / hop_out` (testing-strategy.md §3 property 8) and measured
   as the dominant peak of each **peak-normalized** render's spectrum.
2. **Stutter / grain schedule** — `hop_out = C·(1−F)`, `hop_in = round(hop_out / T)`
   (CLASSIC integer hop), and the launched-grain count. The integer rounding is the
   documented "bad timing" quirk (akaizer-analysis.md §2.2/§2.4).
3. **Schedule-derived output length** — `(G−1)·hop_out + C` with
   `G = floor((N−C)/hop_in)+1`, derived **from the scheduler, never `round(N·T)`**
   (dsp-engine.md §3.4).

This is **not a null test**: Akaizer's exact crossfade shape/length is unrecoverable
without disassembly (akaizer-analysis.md §2.4), so the two renders are *not expected
to be identical*. We compare these analytic features and document the deltas.

## Mandatory: peak-normalize both sides

Akaizer peak-normalizes its output to the source peak since **v1.3**
(akaizer-analysis.md §2.1), while mwStime's SAMPLE render is unnormalized by default
(dsp-engine.md §7.3). **Both renders must be peak-normalized before any comparison.**
`akaizer-crosscheck` does this automatically for every WAV it reads (mwStime *and*
Akaizer side). When producing mwStime renders for the comparison, leave `norm` OFF;
the tool normalizes both sides itself.

## Procedure for a QA agent with a locally licensed Akaizer

Prerequisites: a locally licensed Akaizer CLI on the QA machine, and a built tree
(`cmake --preset default && cmake --build --preset default -j 6`).

1. **Make the scratch dir** (gitignored, never committed):

   ```
   mkdir -p research-cache/akaizer/{mws,akz,reports}
   ```

2. **Render the corpus with mwStime** (CLASSIC, character OFF, norm OFF — the tool
   normalizes), one WAV per case id into `research-cache/akaizer/mws/<id>.wav`:

   ```
   for id in $(grep -o '"id"[^,]*' tests/golden/cases.json | sed 's/.*: *"//;s/"//'); do
     ./build/default/tools/mwstime-render/mwstime-render \
       --case "$id" --cases tests/golden/cases.json \
       --inputs-dir tests/golden/inputs \
       --out "research-cache/akaizer/mws/$id.wav"
   done
   ```

   (The tool itself only *reads* `--mws-dir`; out-of-window ids are skipped in the
   report, so it is fine to render the whole corpus.)

3. **Render the same inputs with Akaizer CLASSIC** into
   `research-cache/akaizer/akz/<id>.wav`, using the **same `<input> <timeFactor>
   <cycleLen> <transpose>` and the `-c` (CLASSIC) flag** that each case declares
   (akaizer-analysis.md §2.1 CLI syntax: `akaizer <file> <time> <cycle> <transpose>
   -c`). Only render the in-window (T 120–2000%) cases — Akaizer has no verified
   oracle outside that. Match the input's sample rate (Cycle Length is in *samples*,
   so rate changes the sound — akaizer-analysis.md §2.1).

4. **Run the cross-check** and write a deviation report:

   ```
   ./build/default/tools/akaizer_crosscheck/akaizer-crosscheck \
     --cases tests/golden/cases.json \
     --inputs-dir tests/golden/inputs \
     --mws-dir research-cache/akaizer/mws \
     --akaizer-dir research-cache/akaizer/akz \
     --out research-cache/akaizer/reports/$(date +%Y%m%d)-deviation.md
   ```

   Omit `--mws-dir`/`--akaizer-dir` to get the **analytic-only** report (schedule +
   predicted flutter, no renders) — useful as a sanity check before producing audio.

5. **Read the deviation report.** It is a Markdown table (see below). For each
   in-window case it lists the analytic schedule, the predicted flutter, and — when
   both renders are present — the measured flutter (mwStime vs Akaizer, with the
   deviation in cents) and the output length (with the frame delta). Skipped cases
   show the "no Akaizer oracle" reason.

## Where deviation reports go

- Generated reports live under `research-cache/akaizer/reports/` (**gitignored**).
- To act on a deviation, **attach the Markdown table to a GitHub issue** (it pastes
  directly). A deviation is a *discussion starter*, not a regression: tag it
  `akaizer-crosscheck`, summarize the case + the delta, and let a human judge it
  against the hardware-capture oracle (task 026b). **Never re-bless a golden or change
  the engine to chase an Akaizer delta** — that would make Akaizer a calibration
  target, which it explicitly is not (ADR-001 consequence; testing-strategy.md §7).

## Deviation report format

`akaizer-crosscheck` emits one Markdown table; columns:

| column | meaning |
|---|---|
| `case` | case id from `cases.json` |
| `T%`, `C` | clamped time factor (percent) and cycle length (samples) |
| `inWindow` | `yes` if T 120–2000%; `no` rows carry the "no Akaizer oracle" note |
| `hop_out`, `hop_in`, `grains`, `schedLen` | analytic CLASSIC schedule (dsp-engine §3.4) |
| `predFlutterHz` | predicted splice-comb spacing = `modelRate / hop_out` |
| `mwsFlutterHz`, `akzFlutterHz` | measured dominant peak of each *normalized* render |
| `flutterΔcents` | `1200·log2(akz/mws)` — flutter-frequency deviation |
| `mwsLen`, `akzLen`, `lenΔframes` | measured output lengths and their delta |
| `note` | `measured`, `analytic only`, or the skip reason |

A trailing line summarizes how many cases were compared vs skipped.

## The `akzcheck` self-test (the only automated part)

The comparison math (window filter, CLASSIC schedule, peak-normalization, flutter
prediction + measurement) is unit-tested on **synthetic** data and needs no Akaizer
binary:

```
ctest --preset default -R akzcheck --no-tests=error
```

This is the only Akaizer-related ctest. It is **not** in the test tree's CMake and
**not** reachable from CI; the real cross-check (steps 1–5) is run by hand by a QA
agent against a local Akaizer copy.
