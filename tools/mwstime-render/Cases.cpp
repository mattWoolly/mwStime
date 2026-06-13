// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Golden case-file loader (Cases.h). Reuses the CLI string<->enum helpers so a
// case file and a direct-mode invocation share one parameter vocabulary.

#include "Cases.h"

#include "Json.h"

#include <set>
#include <string>

namespace mwsrender {

namespace {

/// Reads a number-or-numeric-string field into a double.
bool readNumber(const JsonValue& v, double& out)
{
    if (v.isNumber()) { out = v.number(); return true; }
    if (v.isString())
    {
        try { out = std::stod(v.string()); return true; }
        catch (...) { return false; }
    }
    return false;
}

bool readInt(const JsonValue& v, int& out)
{
    double d{};
    if (!readNumber(v, d))
        return false;
    out = static_cast<int>(d);
    return true;
}

bool readBool(const JsonValue& v, bool& out)
{
    if (v.isBool()) { out = v.boolean(); return true; }
    if (v.isString())
    {
        const auto b = parseOnOff(v.string());
        if (!b) return false;
        out = *b;
        return true;
    }
    return false;
}

/// The recognized keys inside the "params" object — anything else is a typo.
const std::set<std::string>& knownParamKeys()
{
    static const std::set<std::string> keys = {
        "timeFactor", "cycleLen", "stretchMode", "hopMode", "transpose",
        "qual", "width", "material", "bandwidth", "fs", "character",
        "norm", "outTrim",
    };
    return keys;
}

} // namespace

CaseLoadResult loadCase(const std::string& casesJsonText, const std::string& caseId)
{
    CaseLoadResult result;

    const JsonParseResult parsed = parseJson(casesJsonText);
    if (!parsed.ok())
        return { {}, "cases file is not valid JSON: " + parsed.error };

    const JsonValue& root = parsed.value;
    if (!root.isObject())
        return { {}, "cases file root must be a JSON object" };

    if (root.has("version"))
    {
        double version{};
        if (!readNumber(root.at("version"), version) || version != 1.0)
            return { {}, "unsupported cases file version (expected 1)" };
    }

    if (!root.has("cases") || !root.at("cases").isArray())
        return { {}, "cases file must have a \"cases\" array" };

    const JsonValue* match = nullptr;
    for (const JsonValue& c : root.at("cases").array())
    {
        if (c.isObject() && c.at("id").isString() && c.at("id").string() == caseId)
        {
            match = &c;
            break;
        }
    }
    if (match == nullptr)
        return { {}, "case id not found: '" + caseId + "'" };

    CaseDef& def = result.def;
    def.id = caseId;

    if (!match->at("input").isString() || match->at("input").string().empty())
        return { {}, "case '" + caseId + "' is missing a non-empty \"input\"" };
    def.inputFile = match->at("input").string();

    if (!match->at("model").isString())
        return { {}, "case '" + caseId + "' is missing \"model\"" };
    const auto model = parseModel(match->at("model").string());
    if (!model)
        return { {}, "case '" + caseId + "' has invalid model: '"
                     + match->at("model").string() + "'" };
    def.params.model = *model;

    if (match->has("params"))
    {
        const JsonValue& p = match->at("params");
        if (!p.isObject())
            return { {}, "case '" + caseId + "': \"params\" must be an object" };

        for (const auto& [key, value] : p.object())
        {
            if (knownParamKeys().find(key) == knownParamKeys().end())
                return { {}, "case '" + caseId + "': unknown param key '" + key + "'" };

            auto domainError = [&](const std::string& expected) {
                return CaseLoadResult{ {}, "case '" + caseId + "': param '" + key
                                              + "' " + expected };
            };

            if (key == "timeFactor")
            {
                if (!readNumber(value, def.params.timeFactor)) return domainError("must be a number");
            }
            else if (key == "cycleLen")
            {
                if (!readInt(value, def.params.cycleLen)) return domainError("must be an integer");
            }
            else if (key == "transpose")
            {
                if (!readNumber(value, def.params.transpose)) return domainError("must be a number");
            }
            else if (key == "qual")
            {
                if (!readInt(value, def.params.qual)) return domainError("must be an integer");
            }
            else if (key == "width")
            {
                if (!readInt(value, def.params.width)) return domainError("must be an integer");
            }
            else if (key == "bandwidth")
            {
                if (!readNumber(value, def.params.bandwidth)) return domainError("must be a number");
            }
            else if (key == "outTrim")
            {
                if (!readNumber(value, def.params.outTrim)) return domainError("must be a number");
            }
            else if (key == "stretchMode")
            {
                if (!value.isString()) return domainError("must be a string");
                const auto sm = parseStretchMode(value.string());
                if (!sm) return domainError("must be CYCLIC|INTELL");
                def.params.stretchMode = *sm;
            }
            else if (key == "hopMode")
            {
                if (!value.isString()) return domainError("must be a string");
                const auto hm = parseHopMode(value.string());
                if (!hm) return domainError("must be CLASSIC|REVISED");
                def.params.hopMode = *hm;
            }
            else if (key == "material")
            {
                if (!value.isString()) return domainError("must be a string");
                const auto mat = parseMaterial(value.string());
                if (!mat) return domainError("must be MON1|POL2");
                def.params.material = *mat;
            }
            else if (key == "fs")
            {
                if (!value.isString()) return domainError("must be a string (\"44.1\"|\"22.05\")");
                const auto fs = parseFs(value.string());
                if (!fs) return domainError("must be 44.1|22.05");
                def.params.sampleRateSel = *fs;
            }
            else if (key == "character")
            {
                if (!readBool(value, def.params.character)) return domainError("must be a boolean");
            }
            else if (key == "norm")
            {
                if (!readBool(value, def.params.norm)) return domainError("must be a boolean");
            }
        }
    }

    if (match->has("bitDepth"))
    {
        if (!match->at("bitDepth").isString())
            return { {}, "case '" + caseId + "': \"bitDepth\" must be a string" };
        const std::string& bd = match->at("bitDepth").string();
        if (bd == "16")       def.bitDepth = CliArgs::BitDepth::Int16;
        else if (bd == "24")  def.bitDepth = CliArgs::BitDepth::Int24;
        else if (bd == "32")  def.bitDepth = CliArgs::BitDepth::Int32;
        else if (bd == "float" || bd == "float32" || bd == "f32")
            def.bitDepth = CliArgs::BitDepth::Float32;
        else
            return { {}, "case '" + caseId + "': invalid bitDepth '" + bd + "'" };
        def.bitDepthExplicit = true;
    }

    return result;
}

} // namespace mwsrender
