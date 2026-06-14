---
id: 054
title: Gate INTELL — grey + non-automatable stretchMode so INTELL is unreachable (no plausible-fake CYCLIC audio)
status: done
depends-on: [041, 045b]
component: plugin
estimated-size: S
---

## Objective
Selecting INTELL is genuinely impossible at v1, as `dsp-engine.md` §4 and ADR-001 require:
the `stretchMode` LCD field is greyed (cursor skips it), the underlying parameter is
non-automatable, and no jog/host path can move it off CYCLIC. This closes the
"plausible-fake" authenticity defect where INTELL was user-selectable and the engine
silently produced CYCLIC audio under the INTELL name.

## Context
QA finding F1 (HIGH) + F3 (medium) in `docs/QA-REPORT.md`. INTELL is currently editable
via the jog wheel AND host-automatable, and the engine renders it as plain CYCLIC.

Verified facts (read these first):
- `libs/mwstime-core/src/engine/OfflineRenderer.cpp:321-345` — the S1000/S1100/`default:`
  branch calls `cyclic_.render(...)` and never reads `params.stretchMode`. Rendering
  `--stretch-mode CYCLIC` vs `--stretch-mode INTELL` produces byte-identical output
  (confirmed: same md5). The renderer fallback is fine AS LONG AS INTELL is unreachable —
  the fix belongs at the UI/param layer, not (necessarily) here.
- `plugin/ui/LcdPageModel.cpp:183` — the `stretchMode` field is pushed with `editable=true`
  and NO `Greyed` style (`:180` uses default `Normal`); contrast qual/width at `:181-187`
  (`editable=false` + Greyed + `kIntellOnlyHint`). `toLcd()` (`:88-91`) prints "INTELL".
- `plugin/src/state/Parameters.cpp:154-156` — `AudioParameterChoice {"CYCLIC","INTELL"}`
  created with NO attributes, i.e. no `withAutomatable(false)` (unlike model `:128`,
  pluginMode `:133`, sampleRateSel `:212`, embedAudio `:256`). The `:151-153` comment
  claims "selecting it is prevented at the UI layer" — currently false.
- `plugin/ui/LcdFieldEditor.cpp:208-219` — `nudgeParam` jogs the choice with
  `jlimit(0,num-1,...)`, so one detent moves index 0→1 (INTELL), unguarded.
- `plugin/ui/LcdFieldEditor.cpp:96-99` — `fieldEditable` just returns `f.editable`; greying
  the field is sufficient to make the cursor skip it.
- Reachability is proven by `tests/plugin/test_parameters.cpp:372,394` (sets
  stretchMode=1, asserts `StretchMode::Intell` with no coercion) and the cursor-traversal
  spec `tests/plugin/test_editor_wiring.cpp:156-158` (cursor visits index 4 = StretchMode).

Design / ADR (the contract being violated — no PI/ADR carve-out covers this):
- `docs/design/dsp-engine.md` §4 L239-240 ("At v1 the `stretchMode` field shows INTELL
  greyed") and §2 table L57 ("INTELL greyed until v1.1").
- ADR-001 `plan/decisions/001-dsp-engine-architecture.md` res.#2 L53-56 ("Shipping a guess
  under the authentic name is the plausible-fake failure mode this project exists to
  avoid") and L89 ("INTELL absence at v1 must be communicated in the UI (greyed fields)").

## Scope
- Grey the `stretchMode` LCD field: in `plugin/ui/LcdPageModel.cpp` push it with
  `editable=false` and the `Greyed` cell style (matching qual/width), with the
  INTELL-only hint, so the cursor skips it and the value cannot be jogged.
- Make the parameter non-automatable: in `plugin/src/state/Parameters.cpp` create the
  `stretchMode` `AudioParameterChoice` with `.withAutomatable(false)`; add `stretchMode`
  to the non-automatable set.
- Defense-in-depth (choose at least one so a stale state/automation cannot reach the
  engine as INTELL at v1): either coerce `stretchMode` to CYCLIC in the ParamSnapshot/
  engine entry, OR keep the OfflineRenderer branch but add an explicit `jassert`/clamp so
  INTELL can never silently render as CYCLIC unnoticed. Update the now-true
  `Parameters.cpp:151-153` comment.

## Out of scope
- Implementing an actual INTELL engine (deferred to v1.1 per ADR-001 / task scope).
- Changing CYCLIC/REVISED scheduling.
- The CHARACTER-automatable fix (F4) — separate follow-up.

## Acceptance criteria
- [ ] The `stretchMode` LCD field renders greyed and the cursor traversal does NOT land
      on it; a test asserts the field is non-editable / cursor-skipped (extend
      `tests/plugin/test_lcdpagemodel.cpp` and/or `test_editor_wiring.cpp` so the greyed
      set is exactly {stretchMode, qual, width}).
- [ ] `stretchMode` is in the non-automatable set; the
      `tests/plugin/test_parameters.cpp` "non-automatable set is exactly …" test is
      updated to include it and passes.
- [ ] No jog/host/automation path can set the snapshot to `StretchMode::Intell` (update
      `test_parameters.cpp:372,394` to assert coercion-to-CYCLIC, or remove the
      now-impossible path); a test proves an INTELL render path cannot emit silently-CYCLIC
      audio.
- [ ] `cmake --build --preset default` clean; full ctest green.

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R "parameters|lcdpagemodel|editor_wiring" --no-tests=error
ctest --preset default --no-tests=error
```
