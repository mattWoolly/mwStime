---
id: 058
title: Fix Linux build break — make std::atomic<LcdSnapshot> always lock-free (latent from 055)
status: done
depends-on: []
component: plugin
estimated-size: S
---

## Objective
The `mwStime` plugin builds on Linux/GCC x86_64 again. `std::atomic<FxEngine::LcdSnapshot>`
is lock-free on every supported 64-bit target, so the `static_assert` at
`plugin/src/FxEngine.h:549` passes on Linux/GCC as well as macOS/clang.

## Context
A fresh full CI run (after the 056/057 fixes) finally built task 055's code on Linux and
the **required Linux job failed to compile**:
```
plugin/src/FxEngine.h:549:45: error: static assertion failed:
  FxEngine LCD snapshot must be lock-free to keep processBlock lock-free (task 055)
  static_assert(std::atomic<LcdSnapshot>::is_always_lock_free, ...);
```
Root cause (latent bug from task 055, not from 056/057):
- `plugin/src/FxEngine.h:155-160` defines `struct LcdSnapshot { double modelRate; bool
  clampActive; bool monoSummed; };` → 8 + 1 + 1 padded to **16 bytes** (double's 8-byte
  alignment).
- `plugin/src/FxEngine.h:548` holds it as `std::atomic<LcdSnapshot> lcd_`.
- On macOS/clang-arm64 a 16-byte atomic is lock-free (`is_always_lock_free == true`), so the
  assert passed locally and in the macOS CI job. On Linux/GCC x86_64, `is_always_lock_free`
  is **false** for a 16-byte type (no guaranteed lock-free 128-bit atomic without `-mcx16`,
  and GCC reports it conservatively), so the assert fails the build.
- It slipped through because the post-055 on-main CI runs were all auto-cancelled by the
  workflow's concurrency group before the Linux job completed, and local sweeps were
  macOS-only.
Consumers (must keep working): the accessors at `FxEngine.h:355-358` (`modelRate()` returns
`double`), `:365-368` (`clampActive()`), `:378-381` (`monoSummed()`); the equality/null-test
path at `:474-478` `(s.modelRate() < hostRate_) || (hostRate_ < s.modelRate())`; the
publisher `computeLcd` at `:390-396` (`s.modelRate = engine.stretcher.modelRate()`).
Design contract: `architecture.md` §4 (processBlock lock-free; cross-thread handoffs atomic).

## Scope
- Make `std::atomic<LcdSnapshot>` **always** lock-free on all 64-bit targets. Preferred:
  shrink `LcdSnapshot` to ≤ 8 bytes with ≤ 8-byte alignment so the atomic is a 64-bit
  atomic (always lock-free on x86-64 AND arm64). Concretely, change `modelRate` from
  `double` to `float` (sample rates and the model rate are integers ≤ 2^24, exactly
  representable in `float`; the two bools then pack into 8 bytes total). Keep the public
  accessor `modelRate()` returning `double` (implicit widen) so no caller changes.
- If float precision is judged unacceptable for the modelRate equality/null-test path,
  the fallback is three independent lock-free atomics (`std::atomic<double> modelRate`,
  `std::atomic<bool> clampActive/monoSummed`) read separately in the accessors — but the
  ≤8-byte single-snapshot approach is preferred (keeps one coherent atomic read).
- Add a portable guard so this can't regress without a Linux machine:
  `static_assert(sizeof(LcdSnapshot) <= 8, ...)` alongside the existing lock-free assert
  (an 8-byte naturally-aligned type is always lock-free on every supported target).
- Update the comments at `FxEngine.h:155-160` / `:546-549` to explain the size constraint.

## Out of scope
- Any change to the atomic publish/adopt protocol or the `active_` handoff (055 is otherwise
  correct and TSan-clean).
- The CI concurrency-group behavior (separate; note it but do not change here).

## Acceptance criteria
- [ ] `static_assert(std::atomic<LcdSnapshot>::is_always_lock_free)` and a new
      `static_assert(sizeof(LcdSnapshot) <= 8)` both hold.
- [ ] The model-rate equality/null-test path still treats modelRate == hostRate as equal for
      all supported sample rates (44100/22050/48000/96000/192000 and the S950 variable clock)
      — add/extend a test asserting the null path holds (e.g. FX T=100%, character OFF).
- [ ] `[tsan]` FX tests still pass (`ctest --preset tsan -L tsan`), 2/2.
- [ ] Full macOS build + ctest green; no regression.
- [ ] (CI will confirm) the Linux `linux-default` build compiles — the ≤8-byte guarantee
      makes the lock-free assert portable, so this is provable without a Linux host.

## Verification commands
```
cmake --preset default -DFETCHCONTENT_BASE_DIR=$HOME/.cache/mwstime-fc
cmake --build --preset default -j 6
ctest --preset default -R "fx|enginehost|rtstretch|stream" --no-tests=error
ctest --preset default
ctest --preset tsan -L tsan --no-tests=error
```
