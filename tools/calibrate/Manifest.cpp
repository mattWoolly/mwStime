// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// The calibration-manifest loader implementation (task 026b). See Manifest.h.

#include "Manifest.h"

#include <string>

#include "Cases.h" // mwstime_render_lib: the shared cases.json schema + loader
#include "Json.h"  // mwstime_render_lib: the shared hand-rolled JSON reader

namespace mwscal {

ManifestLoadResult loadManifest(const std::string& manifestJsonText)
{
    ManifestLoadResult result;

    const mwsrender::JsonParseResult parsed = mwsrender::parseJson(manifestJsonText);
    if (!parsed.ok())
    {
        result.error = "manifest JSON parse error: " + parsed.error;
        return result;
    }
    const mwsrender::JsonValue& root = parsed.value;
    if (!root.isObject() || !root.at("cases").isArray())
    {
        result.error = "manifest must be an object with a \"cases\" array";
        return result;
    }
    const auto& cases = root.at("cases").array();
    if (cases.empty())
    {
        result.error = "manifest has no cases";
        return result;
    }

    for (const mwsrender::JsonValue& caseVal : cases)
    {
        if (!caseVal.isObject() || !caseVal.at("id").isString())
        {
            result.error = "every manifest case needs a string \"id\"";
            return result;
        }
        const std::string id = caseVal.at("id").string();

        // The disjoint-set tag (harness-only; the render loader ignores it).
        CaseSet set = CaseSet::Calibration;
        if (caseVal.has("set"))
        {
            const mwsrender::JsonValue& sv = caseVal.at("set");
            if (!sv.isString())
            {
                result.error = "case '" + id + "': \"set\" must be a string";
                return result;
            }
            if (sv.string() == "calibration")
                set = CaseSet::Calibration;
            else if (sv.string() == "validation")
                set = CaseSet::Validation;
            else
            {
                result.error = "case '" + id + "': \"set\" must be "
                               "\"calibration\" or \"validation\"";
                return result;
            }
        }

        // Reuse the shared render loader for the parameter mapping/validation
        // (same domains as the golden harness; unknown params keys rejected).
        const mwsrender::CaseLoadResult loaded =
            mwsrender::loadCase(manifestJsonText, id);
        if (!loaded.ok())
        {
            result.error = "case '" + id + "': " + loaded.error;
            return result;
        }

        CalCase c;
        c.id = loaded.def.id;
        c.inputFile = loaded.def.inputFile;
        c.params = loaded.def.params;
        c.set = set;
        result.cases.push_back(std::move(c));
    }

    return result;
}

} // namespace mwscal
