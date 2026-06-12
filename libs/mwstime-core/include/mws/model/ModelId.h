// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// ModelId — the emulated Akai S-series models (docs/design/dsp-engine.md §1,
// docs/design/architecture.md §2.1). Models are data, not code: everything
// model-specific hangs off this id via ModelSpec.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mws::model {

/// The emulated sampler models. Order is fixed and serialized — never reorder.
enum class ModelId : std::uint8_t {
    S900,   ///< 1986 — RepitchEngine, no timestretch (ADR-003)
    S950,   ///< 1988 — S950Engine, stretch to 999%
    S1000,  ///< 1988 — CyclicEngine (CYCLIC); INTELL deferred to v1.1
    S1100,  ///< 1990 — same engine as S1000, 20-bit-DAC dither delta
    S3000,  ///< reserved, v1.1 — id + faceplate slot only, NO behavior (ADR-004)
};

/// Number of ModelId values (including the reserved S3000 slot).
inline constexpr std::size_t kModelCount = 5;

/// All model ids in declaration order, for table-driven iteration.
inline constexpr std::array<ModelId, kModelCount> kAllModels{
    ModelId::S900, ModelId::S950, ModelId::S1000, ModelId::S1100, ModelId::S3000,
};

/// Display/badge name for a model.
[[nodiscard]] constexpr std::string_view toString(ModelId id) noexcept
{
    switch (id)
    {
        case ModelId::S900:  return "S900";
        case ModelId::S950:  return "S950";
        case ModelId::S1000: return "S1000";
        case ModelId::S1100: return "S1100";
        case ModelId::S3000: return "S3000";
    }
    return "S????";  // unreachable for valid ids
}

} // namespace mws::model
