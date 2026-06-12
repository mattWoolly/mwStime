// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// dsp-engine.md §2 parameter table, row by row. Ranges are the fixed
// supersets; hardware-unit strings match the LCD (ui-design.md §3); the
// engine clamps per ModelSpec (architecture.md §6) — never here.

#include "state/Parameters.h"

#include <cmath>

namespace mws::plugin {

namespace {

using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

// All IDs carry version hint 1 (JUCE 8 / AU parameter-identity requirement).
juce::ParameterID pid(const char* id) { return { id, 1 }; }

// --- LCD string conversions (hardware units, dsp-engine.md §2) -------------

// TIME FACTOR: the LCD shows integer percent ("300%") when the value sits on
// a whole percent (always true in CLASSIC, which coerces at the engine);
// REVISED's 0.01 steps show two decimals ("150.50%").
juce::String timeFactorToText(float v, int)
{
    const double r = std::round(static_cast<double>(v) * 100.0) / 100.0;
    if (std::abs(r - std::round(r)) < 1.0e-9)
        return juce::String(static_cast<int>(std::llround(r))) + "%";
    return juce::String(r, 2) + "%";
}

float timeFactorFromText(const juce::String& text)
{
    return text.trim().upToFirstOccurrenceOf("%", false, false).getFloatValue();
}

// TRANSPOSE: signed semitones in 1-cent steps — "+12.00 st" / "-7.50 st" /
// "0.00 st".
juce::String transposeToText(float v, int)
{
    // +0.0 canonicalizes IEEE -0.0 (range snapping can leave a tiny negative
    // residue at the 0 default) so the LCD never shows "-0.00 st".
    const double r = std::round(static_cast<double>(v) * 100.0) / 100.0 + 0.0;
    const juce::String sign = r > 0.0 ? "+" : "";
    return sign + juce::String(r, 2) + " st";
}

float transposeFromText(const juce::String& text)
{
    return text.trim().upToFirstOccurrenceOf("st", false, true).trim().getFloatValue();
}

// BANDWIDTH: one decimal, no space — "19.2kHz".
juce::String bandwidthToText(float v, int)
{
    return juce::String(static_cast<double>(v), 1) + "kHz";
}

float bandwidthFromText(const juce::String& text)
{
    return text.trim().upToFirstOccurrenceOf("kHz", false, true).getFloatValue();
}

// OUTPUT trim: signed, one decimal, "kHz"-style spacing — "+6.0dB" / "0.0dB".
juce::String outTrimToText(float v, int)
{
    const double r = std::round(static_cast<double>(v) * 10.0) / 10.0 + 0.0; // +0.0: no "-0.0dB"
    const juce::String sign = r > 0.0 ? "+" : "";
    return sign + juce::String(r, 1) + "dB";
}

float outTrimFromText(const juce::String& text)
{
    return text.trim().upToFirstOccurrenceOf("dB", false, true).getFloatValue();
}

// ON/OFF switches render as the LCD words, not "1"/"0".
juce::String onOffToText(bool v, int) { return v ? "ON" : "OFF"; }

bool onOffFromText(const juce::String& text)
{
    const auto t = text.trim();
    return t.equalsIgnoreCase("ON") || t == "1" || t.equalsIgnoreCase("true");
}

// ---------------------------------------------------------------------------

std::atomic<float>* rawPtr(juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    auto* p = apvts.getRawParameterValue(id);
    jassert(p != nullptr); // layout/id mismatch is a programming error
    return p;
}

template <typename Enum>
Enum toEnum(const std::atomic<float>* raw) noexcept
{
    return static_cast<Enum>(static_cast<std::uint8_t>(
        std::lround(raw->load(std::memory_order_relaxed))));
}

} // namespace

Layout createParameterLayout()
{
    using juce::AudioParameterBool;
    using juce::AudioParameterChoice;
    using juce::AudioParameterFloat;
    using juce::AudioParameterInt;
    using juce::NormalisableRange;

    const auto nonAutomatableChoice =
        juce::AudioParameterChoiceAttributes().withAutomatable(false);
    const auto nonAutomatableBool =
        juce::AudioParameterBoolAttributes().withAutomatable(false);

    Layout layout;

    // MODEL — non-automatable. Choice order == mws::model::kAllModels.
    // S3000 slot is reserved/UI-constrained until v1.1 (ADR-004) but present
    // now so the host-visible range never changes (architecture.md §6).
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::model), "MODEL",
        juce::StringArray{ "S900", "S950", "S1000", "S1100", "S3000" },
        2, // S1000 default (locked + ADR-004)
        nonAutomatableChoice));

    // MODE — FX-first (locked). Non-automatable.
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::pluginMode), "MODE",
        juce::StringArray{ "FX", "SAMPLE" }, 0, nonAutomatableChoice));

    // TIME FACTOR — superset 25.00–2000.00 %, step 0.01 (CLASSIC integer
    // coercion happens at the engine), default 100 [MAN §5 example screen].
    layout.add(std::make_unique<AudioParameterFloat>(
        pid(paramid::timeFactor), "TIME FACTOR",
        NormalisableRange<float>(25.0f, 2000.0f, 0.01f), 100.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction(timeFactorToText)
            .withValueFromStringFunction(timeFactorFromText)));

    // CYCLE LENGTH / D-TIME — 20–2000 samples at model rate, default 1000
    // (Akaizer convention, ADR-001). LCD shows the bare number ("1000").
    layout.add(std::make_unique<AudioParameterInt>(
        pid(paramid::cycleLen), "CYCLE LENGTH", 20, 2000, 1000,
        juce::AudioParameterIntAttributes().withLabel("samples")));

    // STRETCH MODE — INTELL is present but reserved (greyed/UI-constrained at
    // v1; selecting it is prevented at the UI layer — value reserved so the
    // range is fixed for v1.1).
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::stretchMode), "STRETCH MODE",
        juce::StringArray{ "CYCLIC", "INTELL" }, 0));

    // TIMING — CLASSIC default (hardware-faithful hop arithmetic).
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::hopMode), "TIMING",
        juce::StringArray{ "CLASSIC", "REVISED" }, 0));

    // TRANSPOSE — ±24.00 st in 1-cent steps, default 0.
    layout.add(std::make_unique<AudioParameterFloat>(
        pid(paramid::transpose), "TRANSPOSE",
        NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("st")
            .withStringFromValueFunction(transposeToText)
            .withValueFromStringFunction(transposeFromText)));

    // QUAL — 1–99 default 10 [MAN §5 screen]. INTELL only; inert/greyed at
    // v1 but stored so presets/state round-trip it.
    layout.add(std::make_unique<AudioParameterInt>(
        pid(paramid::qual), "QUAL", 1, 99, 10));

    // WIDTH — 1–99 default 10 [MAN §5 screen]. INTELL only; inert at v1.
    layout.add(std::make_unique<AudioParameterInt>(
        pid(paramid::width), "WIDTH", 1, 99, 10));

    // MON1/POL2 — S950 material switch, POL2 default (breaks/loops).
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::material), "MON1/POL2",
        juce::StringArray{ "MON1", "POL2" }, 1));

    // autC / AUTO-D — momentary trigger (dsp-engine §2 trigger row): fires
    // auto cycle detection and self-resets once the engine has consumed it
    // (F2 soft key writes it — task 045b). Not part of ParamSnapshot.
    layout.add(std::make_unique<AudioParameterBool>(
        pid(paramid::autoCycle), "autC / AUTO-D", false,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction(onOffToText)
            .withValueFromStringFunction(onOffFromText)));

    // BANDWIDTH — superset 3.0–19.2 kHz (S950), step 0.1, default max; the
    // S900 engine-clamps to 16.0. Non-automatable GLOBALLY (documented
    // deviation from the §2 "in FX mode" wording): it changes reported
    // latency (§7.4), and an automatable-in-SAMPLE-mode flag cannot change
    // per mode anyway — hosts cache parameter info (architecture.md §6).
    layout.add(std::make_unique<AudioParameterFloat>(
        pid(paramid::bandwidth), "BANDWIDTH",
        NormalisableRange<float>(3.0f, 19.2f, 0.1f), 19.2f,
        juce::AudioParameterFloatAttributes()
            .withLabel("kHz")
            .withAutomatable(false)
            .withStringFromValueFunction(bandwidthToText)
            .withValueFromStringFunction(bandwidthFromText)));

    // FS — S1000/S1100 model rate. Non-automatable.
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::sampleRateSel), "FS",
        juce::StringArray{ "44.1kHz", "22.05kHz" }, 0, nonAutomatableChoice));

    // CHARACTER — modern-UX bypass of the §8 chain, default ON.
    layout.add(std::make_unique<AudioParameterBool>(
        pid(paramid::character), "CHARACTER", true,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction(onOffToText)
            .withValueFromStringFunction(onOffFromText)));

    // NORM — default OFF (authentic; panel-revised from ON).
    layout.add(std::make_unique<AudioParameterBool>(
        pid(paramid::norm), "NORM", false,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction(onOffToText)
            .withValueFromStringFunction(onOffFromText)));

    // SYNC — FX-mode host tempo sync, default OFF.
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::tempoSync), "SYNC",
        juce::StringArray{ "OFF", "HOST" }, 0));

    // WINDOW — FX capture/resync window (ADR-006), default 1 bar. Choice
    // order == mws::engine::FxWindow.
    layout.add(std::make_unique<AudioParameterChoice>(
        pid(paramid::fxWindow), "WINDOW",
        juce::StringArray{ "1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS", "4 BARS",
                           "8 BARS", "FREE" },
        2));

    // OUTPUT — -24…+12 dB trim, step 0.1, default 0 (PI; no per-model trim
    // offsets — §2 outTrim row).
    layout.add(std::make_unique<AudioParameterFloat>(
        pid(paramid::outTrim), "OUTPUT",
        NormalisableRange<float>(-24.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withLabel("dB")
            .withStringFromValueFunction(outTrimToText)
            .withValueFromStringFunction(outTrimFromText)));

    // embedAudio — non-automatable (dsp-engine.md §2 prose set), default ON
    // for files ≤ 16 MB encoded (architecture.md §6).
    layout.add(std::make_unique<AudioParameterBool>(
        pid(paramid::embedAudio), "EMBED AUDIO", true,
        juce::AudioParameterBoolAttributes()
            .withAutomatable(false)
            .withStringFromValueFunction(onOffToText)
            .withValueFromStringFunction(onOffFromText)));

    return layout;
}

Parameters::Parameters(juce::AudioProcessorValueTreeState& apvts)
    : model_(rawPtr(apvts, paramid::model)),
      pluginMode_(rawPtr(apvts, paramid::pluginMode)),
      timeFactor_(rawPtr(apvts, paramid::timeFactor)),
      cycleLen_(rawPtr(apvts, paramid::cycleLen)),
      stretchMode_(rawPtr(apvts, paramid::stretchMode)),
      hopMode_(rawPtr(apvts, paramid::hopMode)),
      transpose_(rawPtr(apvts, paramid::transpose)),
      qual_(rawPtr(apvts, paramid::qual)),
      width_(rawPtr(apvts, paramid::width)),
      material_(rawPtr(apvts, paramid::material)),
      autoCycle_(rawPtr(apvts, paramid::autoCycle)),
      bandwidth_(rawPtr(apvts, paramid::bandwidth)),
      sampleRateSel_(rawPtr(apvts, paramid::sampleRateSel)),
      character_(rawPtr(apvts, paramid::character)),
      norm_(rawPtr(apvts, paramid::norm)),
      tempoSync_(rawPtr(apvts, paramid::tempoSync)),
      fxWindow_(rawPtr(apvts, paramid::fxWindow)),
      outTrim_(rawPtr(apvts, paramid::outTrim))
{
}

mws::engine::ParamSnapshot Parameters::makeSnapshot() const noexcept
{
    using namespace mws::engine;

    // Raw values are the *unnormalized* parameter values (JUCE stores choice
    // indices / plain ints / floats in the raw atomics). Plain relaxed loads,
    // POD writes — no allocation, no locks, no ValueTree (architecture.md §4).
    ParamSnapshot s;
    s.timeFactor    = static_cast<double>(timeFactor_->load(std::memory_order_relaxed));
    s.cycleLen      = static_cast<int>(std::lround(cycleLen_->load(std::memory_order_relaxed)));
    s.stretchMode   = toEnum<StretchMode>(stretchMode_);
    s.hopMode       = toEnum<HopMode>(hopMode_);
    s.transpose     = static_cast<double>(transpose_->load(std::memory_order_relaxed));
    s.qual          = static_cast<int>(std::lround(qual_->load(std::memory_order_relaxed)));
    s.width         = static_cast<int>(std::lround(width_->load(std::memory_order_relaxed)));
    s.material      = toEnum<Material>(material_);
    s.bandwidth     = static_cast<double>(bandwidth_->load(std::memory_order_relaxed));
    s.sampleRateSel = toEnum<SampleRateSel>(sampleRateSel_);
    s.character     = character_->load(std::memory_order_relaxed) >= 0.5f;
    s.norm          = norm_->load(std::memory_order_relaxed) >= 0.5f;
    s.tempoSync     = toEnum<TempoSync>(tempoSync_);
    s.fxWindow      = toEnum<FxWindow>(fxWindow_);
    s.outTrim       = static_cast<double>(outTrim_->load(std::memory_order_relaxed));
    s.model         = toEnum<model::ModelId>(model_);
    s.pluginMode    = toEnum<PluginMode>(pluginMode_);
    return s;
}

bool Parameters::autoCyclePending() const noexcept
{
    return autoCycle_->load(std::memory_order_relaxed) >= 0.5f;
}

} // namespace mws::plugin
