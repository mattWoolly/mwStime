# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_pluginval.ps1 — Windows (PowerShell) equivalent of run_pluginval.sh
# (task plan/backlog/050; testing-strategy.md §5, architecture.md §3). Windows
# has no bash by default, so the Windows CTest validator rows invoke this .ps1
# (task 050: "POSIX-isms in scripts -> provide .ps1 or python equivalents").
#
# Validates the built VST3 with a PINNED pluginval release at strictness 10.
# pluginval covers only VST3/AU; on Windows only VST3 is built (AU is macOS-only;
# CLAP uses run_clap_validator.ps1), so this validates the VST3.
#
# Behaviour (CTest contract — same exit codes as the .sh):
#   - exit 0  : the located VST3 artifact(s) passed strictness 10.
#   - exit 1  : a located artifact FAILED validation, or the pinned binary's
#               checksum did not match (hard fail — never run unverified).
#   - exit 77 : the pinned pluginval could not be fetched (offline) OR no built
#               VST3 artifact was found — a SKIP (CTest SKIP_RETURN_CODE=77), so
#               `ctest --preset windows-default` stays green offline / without a
#               build.
#
# The pinned binary is cached under build/ (never committed; build/ is gitignored).

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- pinned pluginval release (Tracktion), Windows asset --------------------
$PluginvalVersion = 'v1.0.4'
$PluginvalUrl     = "https://github.com/Tracktion/pluginval/releases/download/$PluginvalVersion/pluginval_Windows.zip"
# SHA-256 of pluginval_Windows.zip for the pinned tag (verified at authoring
# time, task 050 — a REAL digest, not a placeholder). A mismatch is a HARD FAIL
# (the script refuses to run an unverified binary), never a silent skip.
$PluginvalSha256  = 'c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab'
$PluginvalExeRel  = 'pluginval.exe'

$Strictness = 10
$Repeat     = 3
$SkipExit   = 77

function Log  ($m) { Write-Host  "run_pluginval: $m" }
function Warn ($m) { Write-Warning "run_pluginval: $m" }

# --- repo / build locations -------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$buildDir  = if ($env:MWS_BUILD_DIR) { $env:MWS_BUILD_DIR } else { Join-Path $repoRoot 'build\windows-default' }
$toolsCache = Join-Path $repoRoot 'build\validator-tools'
New-Item -ItemType Directory -Force -Path $toolsCache | Out-Null

# Tracked, LOUD waiver (mirrors run_pluginval.sh — task 048/048c): pluginval's
# "Plugin state restoration" sub-test intermittently flags an automatable
# AudioParameterBool as "not restored" (standard JUCE getValue() behaviour on a
# bool param). Proven NOT a state bug (tests/plugin/test_state.cpp round-trips
# every parameter). Disable ONLY that sub-test via the official --disabled-tests
# file; everything else in strictness 10 runs unwaived on every artifact.
$disabledTests = Join-Path $scriptDir 'pluginval-disabled-tests.txt'

# --- fetch the pinned pluginval (skip cleanly if offline) -------------------
$destDir = Join-Path $toolsCache "pluginval-$PluginvalVersion"
$exe     = Join-Path $destDir $PluginvalExeRel

function Get-Pluginval {
    if (Test-Path $exe) {
        Log "using cached pluginval $PluginvalVersion ($exe)"
        return 0
    }
    $zip = Join-Path $toolsCache "pluginval-$PluginvalVersion.zip"
    Log "fetching pinned pluginval $PluginvalVersion ..."
    try {
        Invoke-WebRequest -Uri $PluginvalUrl -OutFile $zip -UseBasicParsing
    } catch {
        Warn "could not download pluginval ($PluginvalUrl) — treating as OFFLINE SKIP."
        if (Test-Path $zip) { Remove-Item $zip -Force }
        return 1
    }
    $got = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
    if ($got -ne $PluginvalSha256.ToLower()) {
        Warn "pluginval checksum mismatch! expected $PluginvalSha256, got $got."
        Warn "refusing to run an unverified binary."
        Remove-Item $zip -Force
        return 2
    }
    if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    try {
        Expand-Archive -Path $zip -DestinationPath $destDir -Force
    } catch {
        Warn "failed to unzip pluginval — treating as SKIP."
        Remove-Item $zip -Force
        return 1
    }
    Remove-Item $zip -Force
    if (-not (Test-Path $exe)) {
        Warn "pluginval executable not found after unzip ($exe) — SKIP."
        return 1
    }
    Log "fetched pluginval $PluginvalVersion."
    return 0
}

$rc = Get-Pluginval
if ($rc -eq 2) { exit 1 }          # checksum mismatch is a hard failure
if ($rc -ne 0) {
    Warn "pluginval unavailable — SKIPPING (exit $SkipExit)."
    exit $SkipExit
}

# --- locate the built VST3 artifact(s) --------------------------------------
# JUCE writes them under <build>\plugin\mwStime_artefacts\<config>\VST3\*.vst3.
# @(...) forces array context so .Count is valid even for 0 or 1 match under
# Set-StrictMode.
$targets = @()
if (Test-Path $buildDir) {
    $targets = @(Get-ChildItem -Path $buildDir -Recurse -Directory -Filter '*.vst3' -ErrorAction SilentlyContinue |
               Select-Object -ExpandProperty FullName | Sort-Object -Unique)
}
if ($targets.Count -eq 0) {
    Warn "no built VST3 artifact found under $buildDir."
    Warn "build the plugin first: cmake --build --preset windows-default"
    Warn "nothing to validate — SKIPPING (exit $SkipExit)."
    exit $SkipExit
}

# --- validate each artifact at strictness 10 --------------------------------
$extra = @()
if (Test-Path $disabledTests) {
    Warn "WAIVER (tracked, follow-up plan/backlog/048c): disabling pluginval's"
    Warn "  'Plugin state restoration' sub-test on ALL artifacts — known JUCE"
    Warn "  AudioParameterBool / pluginval flake (see $disabledTests);"
    Warn "  every other strictness-$Strictness test runs unwaived."
    $extra = @('--disabled-tests', $disabledTests)
}

$failed = 0
$validated = 0
foreach ($target in $targets) {
    Log "validating: $target"
    & $exe --strictness-level $Strictness --validate-in-process `
        --repeat $Repeat --randomise @extra --validate $target
    if ($LASTEXITCODE -eq 0) {
        Log "PASS: $target"
        $validated++
    } else {
        Warn "FAIL: $target"
        $failed++
    }
}

if ($failed -ne 0) {
    Warn "$failed artifact(s) FAILED pluginval (strictness $Strictness)."
    exit 1
}
Log "all $validated artifact(s) PASSED pluginval (strictness $Strictness)."
exit 0
