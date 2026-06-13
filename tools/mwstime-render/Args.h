// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Argument parsing for the mwstime-render CLI
// (task plan/backlog/025-mwstime-render-cli.md). Every render-relevant
// docs/design/dsp-engine.md §2 parameter has a flag whose default mirrors the
// §2 table; the parsed values populate a mws::engine::ParamSnapshot which the
// single clamping authority (ModelSpec::clamp) then bounds at the engine.

#pragma once

#include <optional>
#include <string>

#include "mws/engine/Params.h"

namespace mwsrender {

/// Which top-level operation the user asked for.
/// `Version` prints "<engineVersion> 0x<hash>" (machine-readable, one line) and
/// exits 0 — the bless target reads it to stamp tests/golden/blessed/MANIFEST.json
/// (task 026; the renderer is the authoritative source of the engine version).
enum class Mode { Direct, Case, Help, Version };

/// One mws::engine::ParamSnapshot value mapped from a CLI string flag, plus a
/// human error when the string is out of the documented domain.
struct ParseFieldResult {
    bool ok = true;
    std::string error;
};

/// The fully parsed command line. On `Help` only `mode` is meaningful. On
/// `Direct`/`Case` the file paths and the snapshot are populated; the snapshot
/// already carries every flag the user set (and §2 defaults for the rest).
struct CliArgs {
    Mode mode = Mode::Help;

    // Direct mode I/O.
    std::string inPath;   ///< --in
    std::string outPath;  ///< --out

    // Case mode.
    std::string caseId;     ///< --case
    std::string casesPath;  ///< --cases (default tests/golden/cases.json)
    std::string inputsDir;  ///< --inputs-dir

    /// The §2 snapshot assembled from defaults + flags (direct mode). In case
    /// mode the case file supplies the snapshot; the direct flags are ignored.
    mws::engine::ParamSnapshot params{};

    /// Output WAV bit depth (`--bit-depth`). Default 16-bit (the golden-input
    /// convention, docs/design/testing-strategy.md §4) — but the renderer
    /// itself is float32, so float output is available for lossless goldens.
    enum class BitDepth { Int16, Int24, Int32, Float32 };
    BitDepth bitDepth = BitDepth::Int16;
};

/// Result of parsing argv: `error` empty on success.
struct ArgsParseResult {
    CliArgs args;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Parses argv (excluding argv[0]). `--help`/`-h` short-circuits to Mode::Help.
/// Unknown flags, missing values, and out-of-domain enum strings are reported
/// via `error` (nonzero CLI exit). Numeric range clamping is NOT done here —
/// ModelSpec::clamp is the single authority (dsp-engine.md §2; the CLI passes
/// the raw §2-superset values through).
[[nodiscard]] ArgsParseResult parseArgs(int argc, const char* const* argv);

/// The `--help` text: documents every flag with its hardware unit
/// (acceptance criterion). Returned as a string so it is testable.
[[nodiscard]] std::string helpText();

// --- shared string<->enum helpers (also used by the case-file loader) -------

[[nodiscard]] std::optional<mws::model::ModelId> parseModel(const std::string& s);
[[nodiscard]] std::optional<mws::engine::HopMode> parseHopMode(const std::string& s);
[[nodiscard]] std::optional<mws::engine::StretchMode> parseStretchMode(const std::string& s);
[[nodiscard]] std::optional<mws::engine::Material> parseMaterial(const std::string& s);
[[nodiscard]] std::optional<mws::engine::SampleRateSel> parseFs(const std::string& s);
[[nodiscard]] std::optional<bool> parseOnOff(const std::string& s);

} // namespace mwsrender
