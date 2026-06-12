// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.

#pragma once

#include <cassert>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace mws::core {

/// Non-owning view over one channel of float32 audio: pointer + length.
/// `BasicAudioView<float>` is mutable, `BasicAudioView<const float>` is the
/// const variant; the mutable view converts implicitly to the const one
/// (docs/design/architecture.md §2.1). No accessor allocates.
template <typename SampleType>
class BasicAudioView
{
    static_assert(std::is_same_v<std::remove_const_t<SampleType>, float>,
                  "mws::core audio is float32 (docs/design/dsp-engine.md §3)");

public:
    constexpr BasicAudioView() noexcept = default;

    constexpr BasicAudioView(SampleType* data, std::size_t numFrames) noexcept
        : data_(data), size_(numFrames)
    {
    }

    /// Mutable -> const conversion (never the reverse).
    template <typename OtherSampleType>
        requires(std::is_const_v<SampleType>
                 && std::is_same_v<std::remove_const_t<OtherSampleType>,
                                   std::remove_const_t<SampleType>>
                 && !std::is_const_v<OtherSampleType>)
    constexpr BasicAudioView(BasicAudioView<OtherSampleType> other) noexcept
        : data_(other.data()), size_(other.size())
    {
    }

    [[nodiscard]] constexpr SampleType* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr std::size_t numFrames() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] constexpr SampleType& operator[](std::size_t frame) const noexcept
    {
        assert(frame < size_);
        return data_[frame];
    }

    [[nodiscard]] constexpr SampleType* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr SampleType* end() const noexcept { return data_ + size_; }

private:
    SampleType* data_ = nullptr;
    std::size_t size_ = 0;
};

using AudioView = BasicAudioView<float>;
using ConstAudioView = BasicAudioView<const float>;

/// Owning float32 audio container: channels × frames, each channel a
/// contiguous run (channel c starts at frame index c * numFrames()). The
/// common audio container for every core module (architecture.md §2.1).
/// Allocation happens only in construction and explicit resize(); every
/// accessor is allocation-free.
class AudioBuffer
{
public:
    AudioBuffer() = default;

    /// Allocates channels × frames samples, zero-initialised.
    AudioBuffer(std::size_t numChannels, std::size_t numFrames)
        : channels_(numChannels),
          frames_(numFrames),
          samples_(numChannels * numFrames, 0.0f)
    {
    }

    [[nodiscard]] std::size_t numChannels() const noexcept { return channels_; }
    [[nodiscard]] std::size_t numFrames() const noexcept { return frames_; }

    /// Mutable view over one channel. No allocation.
    [[nodiscard]] AudioView channel(std::size_t index) noexcept
    {
        assert(index < channels_);
        return { samples_.data() + index * frames_, frames_ };
    }

    /// Const view over one channel. No allocation.
    [[nodiscard]] ConstAudioView channel(std::size_t index) const noexcept
    {
        assert(index < channels_);
        return { samples_.data() + index * frames_, frames_ };
    }

    /// Reshapes to channels × frames and zero-initialises ALL content
    /// (previous samples are discarded). Invalidates outstanding views.
    /// `sampleRate` is preserved. The only allocating call besides the ctor.
    void resize(std::size_t numChannels, std::size_t numFrames)
    {
        channels_ = numChannels;
        frames_ = numFrames;
        samples_.assign(numChannels * numFrames, 0.0f);
    }

    /// Sample rate in Hz this audio is meant to play at; 0.0 = unset.
    /// Plain field by design (plan/backlog/002-core-buffer.md).
    double sampleRate = 0.0;

private:
    std::size_t channels_ = 0;
    std::size_t frames_ = 0;
    std::vector<float> samples_;
};

} // namespace mws::core
