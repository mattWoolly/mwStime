// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include "mws/core/Buffer.h"

#include <cstddef>

namespace mws::core {

/// Offline arbitrary-ratio resamplers (docs/design/architecture.md §2.1).
///
/// `ratio` is outputRate / inputRate everywhere (> 1 upsamples, < 1
/// downsamples). Output length is ceil(inputFrames * ratio); content inside it
/// is delayed by `groupDelaySamples(ratio)` OUTPUT samples — callers that need
/// alignment (e.g. the FX latency formula, docs/design/dsp-engine.md §3.5/§7.4)
/// add that figure, they do not guess.
///
/// Both resamplers are deterministic (architecture.md §7): precomputed kernel
/// table, double-precision phase arithmetic (output position is derived from
/// the output index by one division — no drifting accumulator), no
/// platform-dependent math shortcuts.

/// Quality path: 16-tap Kaiser(beta=8) windowed-sinc resampler **(PI)** —
/// ingest SRC, S1000/S1100 transpose (dsp-engine.md §7.2), decimate-to-host
/// (§8.1/§8.2). When downsampling, the kernel is time-stretched by 1/ratio so
/// its cutoff lands on the NEW Nyquist (anti-aliasing); tap count grows
/// accordingly — offline use only.
class SincResampler
{
public:
    /// Kernel taps at ratio >= 1 (stretched to ~kTaps/ratio when ratio < 1).
    static constexpr std::size_t kTaps = 16;

    /// Resamples one channel into a fresh 1-channel AudioBuffer of
    /// ceil(input.size() * ratio) frames. `ratio` must be > 0.
    /// `sampleRate` on the result is left 0.0 (the caller knows the rates).
    [[nodiscard]] static AudioBuffer resample(ConstAudioView input, double ratio);

    /// Group delay in OUTPUT samples: kTaps/2 * max(ratio, 1). Integer at
    /// ratio 1.0 by construction (the kernel center sits on a whole input
    /// sample), which is what makes the identity test exact. Feeds the FX
    /// latency formula (dsp-engine.md §7.4).
    [[nodiscard]] static double groupDelaySamples(double ratio) noexcept;
};

/// Cheap path: 2-point linear interpolation, same API shape. No
/// anti-aliasing — callers pick it deliberately. Zero group delay (the
/// interpolator is centered on the read position).
class LinearResampler
{
public:
    /// Resamples one channel into a fresh 1-channel AudioBuffer of
    /// ceil(input.size() * ratio) frames. `ratio` must be > 0.
    /// Exact pass-through at ratio 1.0.
    [[nodiscard]] static AudioBuffer resample(ConstAudioView input, double ratio);

    /// Always 0.0 — kept as a function so both resamplers expose the same
    /// latency-reporting shape.
    [[nodiscard]] static double groupDelaySamples(double ratio) noexcept;
};

} // namespace mws::core
