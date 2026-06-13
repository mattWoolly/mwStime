// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// The golden-test case-file schema and loader (docs/design/testing-strategy.md
// §4: the runner contract is `mwstime-render --case <id>` reading
// tests/golden/cases.json). The schema is DEFINED here (task 025); the corpus
// itself is populated by task 026, so this module ships with no committed cases.
//
// cases.json schema (read-only, parsed by Json.h):
//
//   {
//     "version": 1,
//     "cases": [
//       {
//         "id": "s1100_jungle_amen_300",   // unique; output named <model>_<case>
//         "input": "breakslice.wav",       // file inside --inputs-dir
//         "model": "s1100",                 // S900|S950|S1000|S1100
//         "params": {                       // any subset of the §2 render flags;
//           "timeFactor": 300,              // omitted fields take the §2 default
//           "cycleLen": 1000,
//           "stretchMode": "cyclic",        // cyclic|intell
//           "hopMode": "classic",           // classic|revised
//           "transpose": 0,
//           "qual": 20,                      // inert at v1, round-tripped
//           "width": 10,
//           "material": "pol2",             // mon1|pol2
//           "bandwidth": 19.2,              // kHz
//           "fs": "44.1",                   // 44.1|22.05
//           "character": true,
//           "norm": false,
//           "outTrim": 0
//         },
//         "bitDepth": "float"               // optional: 16|24|32|float (default 16)
//       }
//     ]
//   }
//
// Numbers may be JSON numbers or strings; enums are strings (same domains as the
// CLI flags). Unknown keys inside "params" are rejected so a typo never silently
// renders the wrong sound.

#pragma once

#include <string>

#include "Args.h"
#include "mws/engine/Params.h"

namespace mwsrender {

/// A single resolved golden case.
struct CaseDef {
    std::string id;                      ///< case id (from the file)
    std::string inputFile;               ///< input WAV filename (relative to --inputs-dir)
    mws::engine::ParamSnapshot params{}; ///< §2 defaults overlaid with the case's params
    CliArgs::BitDepth bitDepth = CliArgs::BitDepth::Int16;
    bool bitDepthExplicit = false;       ///< true if the case set "bitDepth"
};

/// Result of loading one case from a cases file: `error` empty on success.
struct CaseLoadResult {
    CaseDef def;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Loads case `caseId` from the JSON text of a cases file. Validates the schema
/// version, the enum/number domains, and rejects unknown "params" keys. The
/// returned snapshot starts from the §2 defaults and overlays only the keys the
/// case provides (so a case round-trips inert fields). Engine-range clamping is
/// the renderer's job (ModelSpec::clamp), not this loader's.
[[nodiscard]] CaseLoadResult loadCase(const std::string& casesJsonText,
                                      const std::string& caseId);

} // namespace mwsrender
