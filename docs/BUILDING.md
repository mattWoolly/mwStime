<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly. -->

# Building mwStime

mwStime is **JUCE 8 + CMake (FetchContent)**, AGPLv3. The pure DSP core
(`libs/mwstime-core`) is JUCE-free and builds with any C++20 compiler; only the
`plugin/` layer pulls JUCE. CMake ≥ 3.25.

The single verification command per platform is the CMake **preset** trio
(`configure → build → ctest`). Presets are committed in `CMakePresets.json`:

| Platform | Configure / build / test preset | Generator | Formats built |
|---|---|---|---|
| macOS (reference) | `default` | platform default | VST3, AU, LV2, CLAP, Standalone |
| Linux x86_64 | `linux-default` | Ninja | VST3, LV2, CLAP, Standalone (no AU) |
| Windows x64 (MSVC) | `windows-default` | Ninja + MSVC | VST3, CLAP, Standalone (no AU, no LV2) |

JUCE, Catch2 and clap-juce-extensions are fetched once via FetchContent. Share
the download cache across checkouts with `-DFETCHCONTENT_BASE_DIR=...` (any
writable dir), e.g. `~/.cache/mwstime-fc`.

---

## macOS

Prerequisites: Xcode command-line tools + CMake (`brew install cmake`).

```sh
cmake --preset default
cmake --build --preset default -j 6
ctest --preset default
```

macOS is the **reference platform**: the golden renders in
`tests/golden/blessed/` are blessed here and every golden case (integer CLASSIC
*and* float-stage) must match bit-exactly (`golden_compare --tol 0`).

Format validators (auval / pluginval / clap-validator) self-skip when their
tools are unavailable; run only the validators with:

```sh
ctest --preset default -L validators --no-tests=error
```

---

## Linux (x86_64, GCC or Clang)

### 1. Toolchain + build prerequisites

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config ca-certificates curl unzip
```

### 2. JUCE Linux runtime/build dependencies

JUCE's Linux back end needs ALSA, X11 (+ extensions), freetype, fontconfig and
curl development headers. **WebKit/GTK are NOT required** — the plugin builds
with `JUCE_WEB_BROWSER=0` / `JUCE_USE_CURL=0`, so the heavy `webkit2gtk` stack
is intentionally omitted.

```sh
sudo apt-get install -y \
    libasound2-dev \
    libx11-dev libxext-dev libxinerama-dev libxrandr-dev \
    libxcursor-dev libxcomposite-dev \
    libfreetype-dev libfontconfig1-dev \
    libcurl4-openssl-dev
```

(Older distros: `libfreetype6-dev` instead of `libfreetype-dev`.)

### 3. Configure, build, test

```sh
cmake --preset linux-default -DFETCHCONTENT_BASE_DIR="$HOME/.cache/mwstime-fc"
cmake --build --preset linux-default -j 6
ctest --preset linux-default
```

`linux-default` uses the same build type (`RelWithDebInfo`) and the same FP
discipline as `default` (no `-ffast-math`; `-ffp-contract=off` on the core).
The format validators self-skip when their tools are absent, so the goldens and
unit/property tests still gate. Run just the validators with:

```sh
ctest --preset linux-default -L validators --no-tests=error
```

### 4. Golden tolerance policy off-reference

Goldens are blessed bit-exact on macOS arm64 (the reference). On Linux the
comparison is keyed by case **and** platform (testing-strategy.md §4):

- **Integer CLASSIC stretch-only** cases (`"policy": "exact"` in
  `tests/golden/cases.json`) stay **bit-exact on every platform** — the path is
  integer-only by construction (architecture.md §2), so any drift is a hard
  failure.
- **Float-stage** cases (filters, SRC, transpose, REVISED — `"policy":
  "tolerance"`) compare with **max abs ≤ 1e-6** off-reference (vs `0` on the
  reference platform). On any mismatch the comparer prints diagnostics (max/RMS
  diff, first divergent sample, splice-comb peak, 1/3-octave table) to the
  CTest log so failures are debuggable without a DAW.

### 5. Format-validator tooling (Linux)

These are run by `ctest --preset linux-default -L validators` and each
**self-skips** (CTest exit 77) when its tool is missing — installed tools are
always run (never "optional"):

| Format | Tool(s) | Install |
|---|---|---|
| VST3 | pluginval (pinned `pluginval_Linux.zip`, auto-downloaded) | (downloaded into `build/`) |
| CLAP | clap-validator (pinned Linux tarball, auto-downloaded) | (downloaded into `build/`) |
| LV2 | lv2lint + lv2_validate/sord_validate + **Carla load smoke** | see below |

```sh
sudo apt-get install -y lv2lint lilv-utils sordi carla xvfb
```

- `lilv-utils` provides `lv2_validate`; `sordi`/`sord` provides `sord_validate`
  (the RDF/Turtle validator the LV2 checks fall back to).
- `carla` provides the **required** headless load smoke (`carla-single`); it is
  a gate, not optional, and self-skips only when Carla is not installed.
- `xvfb` provides `xvfb-run`; the validator scripts wrap every GUI-touching
  step in `xvfb-run -a` automatically when `$DISPLAY` is unset (headless boxes).

**Pinned checksums:** pluginval/clap-validator pin a per-OS URL **and** SHA-256
in their scripts. The Linux digests are placeholders until first blessed on a
Linux box — set `MWS_PLUGINVAL_LINUX_SHA256` / `MWS_CLAP_VALIDATOR_LINUX_SHA256`
(or edit the pin) with the real digest. A checksum mismatch is a **hard
failure** (the scripts refuse to run an unverified binary), never a silent skip.

---

## Windows (x64, MSVC)

Windows is the **third platform goal** (ORCHESTRATION.md). The Windows v1 format
set is **VST3 + CLAP + Standalone** — AU is macOS-only and LV2 is dropped on
Windows (there is no Windows LV2 validator wired, so building it would yield an
unchecked artifact; architecture.md §3, ADR-002). It is off-reference, so the
golden tolerance policy below applies exactly as on Linux.

### 1. Toolchain

- **Visual Studio 2022** with the *Desktop development with C++* workload (gives
  the MSVC `cl`/`link` toolchain), or the standalone *Build Tools for Visual
  Studio 2022*.
- **CMake ≥ 3.25** and **Ninja** (both ship with the VS *C++ CMake tools for
  Windows* component; or install separately and put them on `PATH`).
- **Git** (for the FetchContent clones of JUCE / Catch2 / clap-juce-extensions).

No other dependencies: the JUCE Windows back end uses only system frameworks, and
the plugin builds with `JUCE_WEB_BROWSER=0` / `JUCE_USE_CURL=0`.

### 2. Configure, build, test

The `windows-default` preset uses **Ninja + MSVC (`cl`)**, so run it from a
**x64 Native Tools Command Prompt for VS 2022** (which puts `cl`/`link` on
`PATH`) — or use Visual Studio's built-in CMake integration, which sets up the
environment automatically.

```bat
cmake --preset windows-default -DFETCHCONTENT_BASE_DIR="%USERPROFILE%\.cache\mwstime-fc"
cmake --build --preset windows-default -j 6
ctest --preset windows-default
```

`windows-default` uses the same build type (`RelWithDebInfo`) and the same FP
discipline as `default`: no `-ffast-math`; the deterministic core pins
**`/fp:precise`** on `mwstime_core` (MSVC's default, made explicit so the
integer-CLASSIC bit-exactness on a third compiler never silently depends on a
toolchain default — `/fp:precise` disables FMA contraction and the `/fp:fast`
reorderings). The format validators self-skip when their tools are absent, so the
goldens and unit/property tests still gate. Run just the validators with:

```bat
ctest --preset windows-default -L validators --no-tests=error
```

### 3. Golden tolerance policy off-reference

Identical to Linux (testing-strategy.md §4): goldens are blessed bit-exact on
macOS arm64 (the reference). On Windows the comparison is keyed by case **and**
platform:

- **Integer CLASSIC stretch-only** cases (`"policy": "exact"` in
  `tests/golden/cases.json`) stay **bit-exact** — the path is integer-only by
  construction (architecture.md §2), validating the fixed-point discipline on a
  third compiler (MSVC). Any drift is a hard failure.
- **Float-stage** cases (`"policy": "tolerance"`) compare with **max abs ≤ 1e-6**
  off-reference. The golden runner is `tests/golden/RunGoldenCase.cmake`, invoked
  via `cmake -P` (cross-platform — no bash needed on Windows, unlike the
  `tools/run_golden_case.sh` shell wrapper which the macOS/Linux flow can still
  use manually).

### 4. Format-validator tooling (Windows)

`ctest --preset windows-default -L validators` runs two validators; each
**self-skips** (CTest exit 77) when its pinned tool can't be fetched (offline) or
no built artifact is present:

| Format | Tool | Pinned Windows asset |
|---|---|---|
| VST3 | pluginval (strictness 10) | `pluginval_Windows.zip` (auto-downloaded into `build/`) |
| CLAP | clap-validator | `clap-validator-<ver>-windows.zip` (auto-downloaded into `build/`) |

Windows has no bash, so these run **PowerShell** ports —
`tests/plugin/run_pluginval.ps1` and `tests/plugin/run_clap_validator.ps1` —
invoked by CTest via `powershell.exe -File` (or `pwsh` if present). They mirror
the `.sh` behaviour exactly: pin a URL **and** a verified SHA-256 (a mismatch is
a **hard failure**, never a silent skip), cache the binary under `build/`, and
honour `MWS_BUILD_DIR`. Unlike the Linux digests, the **Windows digests are real,
verified pins** (set when this preset was brought up). AU/auval and the LV2
checks do not apply on Windows.

---

## Cross-platform bit-exactness scope

Only the **integer CLASSIC stretch path** (transpose 0, character OFF) is
claimed bit-exact across platforms (architecture.md §2; testing-strategy.md §4).
This holds on macOS arm64, Linux x86_64 and Windows x64 because all three are
little-endian and `mws::core::WavIo` reads/writes WAV with explicit
little-endian byte helpers (no host-endianness assumptions). The discipline is
pinned per compiler — `-ffp-contract=off` on Clang/GCC, `/fp:precise` on MSVC —
so the integer path validates identically on three compilers. Float stages are
reference-platform-exact and tolerance-compared elsewhere.
