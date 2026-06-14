# SPDX-License-Identifier: AGPL-3.0-or-later
# mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
#
# run_clap_validator.ps1 — Windows (PowerShell) equivalent of
# run_clap_validator.sh (task plan/backlog/050; testing-strategy.md §5,
# architecture.md §3). Windows has no bash by default, so the Windows CTest CLAP
# validator row invokes this .ps1. pluginval CANNOT load CLAP — CLAP needs its
# own validator.
#
# Behaviour (CTest contract — same exit codes as the .sh):
#   - exit 0  : the built .clap passed clap-validator.
#   - exit 1  : the built .clap FAILED validation, or the pinned binary's
#               checksum did not match (hard fail — never run unverified).
#   - exit 77 : the pinned clap-validator could not be fetched (offline) OR no
#               built .clap was found — a SKIP (CTest SKIP_RETURN_CODE=77), so
#               `ctest --preset windows-default` stays green offline / no build.
#
# The pinned binary is cached under build/ (never committed; build/ is gitignored).

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- pinned clap-validator release (free-audio), Windows asset --------------
$ClapValidatorVersion = '0.3.2'
$ClapValidatorUrl     = "https://github.com/free-audio/clap-validator/releases/download/$ClapValidatorVersion/clap-validator-$ClapValidatorVersion-windows.zip"
# SHA-256 of the Windows zip for the pinned tag (verified at authoring time,
# task 050 — a REAL digest, not a placeholder). A mismatch is a HARD FAIL.
$ClapValidatorSha256  = '68cdbe51c05489542b5420a870b7159b4cb352b5ce5c486f9bd8a9b49b75bdda'
$ClapValidatorExeRel  = 'clap-validator.exe'

$SkipExit = 77

function Log  ($m) { Write-Host  "run_clap_validator: $m" }
function Warn ($m) { Write-Warning "run_clap_validator: $m" }

# --- repo / build locations -------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$buildDir  = if ($env:MWS_BUILD_DIR) { $env:MWS_BUILD_DIR } else { Join-Path $repoRoot 'build\windows-default' }
$toolsCache = Join-Path $repoRoot 'build\validator-tools'
New-Item -ItemType Directory -Force -Path $toolsCache | Out-Null

# --- fetch the pinned clap-validator (skip cleanly if offline) --------------
$destDir = Join-Path $toolsCache "clap-validator-$ClapValidatorVersion"
$exe     = Join-Path $destDir $ClapValidatorExeRel

function Get-ClapValidator {
    if (Test-Path $exe) {
        Log "using cached clap-validator $ClapValidatorVersion ($exe)"
        return 0
    }
    $zip = Join-Path $toolsCache "clap-validator-$ClapValidatorVersion.zip"
    Log "fetching pinned clap-validator $ClapValidatorVersion ..."
    try {
        Invoke-WebRequest -Uri $ClapValidatorUrl -OutFile $zip -UseBasicParsing
    } catch {
        Warn "could not download clap-validator ($ClapValidatorUrl) — treating as OFFLINE SKIP."
        if (Test-Path $zip) { Remove-Item $zip -Force }
        return 1
    }
    $got = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLower()
    if ($got -ne $ClapValidatorSha256.ToLower()) {
        Warn "clap-validator checksum mismatch! expected $ClapValidatorSha256, got $got."
        Warn "refusing to run an unverified binary."
        Remove-Item $zip -Force
        return 2
    }
    if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    try {
        Expand-Archive -Path $zip -DestinationPath $destDir -Force
    } catch {
        Warn "failed to extract clap-validator — treating as SKIP."
        Remove-Item $zip -Force
        return 1
    }
    Remove-Item $zip -Force
    if (-not (Test-Path $exe)) {
        Warn "clap-validator executable not found after extract ($exe) — SKIP."
        return 1
    }
    Log "fetched clap-validator $ClapValidatorVersion."
    return 0
}

$rc = Get-ClapValidator
if ($rc -eq 2) { exit 1 }          # checksum mismatch is a hard failure
if ($rc -ne 0) {
    Warn "clap-validator unavailable — SKIPPING (exit $SkipExit)."
    exit $SkipExit
}

# --- locate the built .clap -------------------------------------------------
# JUCE/clap-juce-extensions writes it under
# <build>\plugin\mwStime_artefacts\<config>\CLAP\mwStime.clap.
$clap = $null
if (Test-Path $buildDir) {
    $clap = Get-ChildItem -Path $buildDir -Recurse -Filter '*.clap' -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName | Sort-Object | Select-Object -First 1
}
if (-not $clap) {
    Warn "no built .clap found under $buildDir."
    Warn "build the plugin first: cmake --build --preset windows-default"
    Warn "nothing to validate — SKIPPING (exit $SkipExit)."
    exit $SkipExit
}

# --- validate ---------------------------------------------------------------
Log "validating: $clap"
& $exe validate $clap
if ($LASTEXITCODE -eq 0) {
    Log "PASS: $clap"
    exit 0
}
Warn "FAIL: $clap did not pass clap-validator $ClapValidatorVersion."
exit 1
