// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Published<T> — the ONE audited RCU publication mechanism for every
// cross-thread immutable-buffer handoff (docs/design/architecture.md §4
// ownership/publication protocol; testing-strategy.md §3.6). Used by the
// loaded sample (031), the render result (this task), and the FX history
// reconfiguration (033) so there is a single mechanism, never ad-hoc atomics
// scattered through the plugin (acceptance criterion of task 030).
//
// The protocol (architecture.md §4, verbatim):
//   - All audio buffers shared across threads are immutable once published and
//     held by std::shared_ptr<const T>.
//   - A producer thread (message / render worker / file loader) publishes a new
//     value with publish(); publication is a single atomic store of the
//     shared_ptr.
//   - The audio thread copies the pointer ONCE per block via acquire() (RCU):
//     a buffer can never be freed mid-block because the audio thread holds its
//     own shared_ptr refcount for the whole block.
//   - When the audio thread is done with its per-block copy it returns it via
//     retire(); the shared_ptr is pushed into a lock-free graveyard FIFO rather
//     than being destroyed inline. The audio thread NEVER runs a destructor that
//     could free a buffer (deallocation never on the audio thread —
//     architecture.md §4).
//   - The message thread periodically calls collectGarbage() (timer poll) to
//     drain the graveyard and let the held shared_ptrs drop their refcounts.
//     The actual free happens here, on the message thread.
//
// This header is deliberately JUCE-free and depends only on the C++20 standard
// library so the publication stress/ThreadSanitizer test (test_publication.cpp)
// builds in the JUCE-free core test binary and runs cleanly under the `tsan`
// preset.

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace mws::plugin {

/// Single-producer / single-consumer lock-free ring of std::shared_ptr<const T>
/// used as the graveyard: the audio thread (consumer-side producer of garbage)
/// pushes retired pointers, the message thread pops them and lets them die.
/// Push/pop are wait-free; the audio side never blocks and never deallocates
/// (the shared_ptr it pushes only drops its refcount when the message thread
/// later destroys the slot copy).
///
/// Capacity is a power of two; one slot is always left empty to disambiguate
/// full from empty. A full ring on push is reported via the push() return
/// value; Published sizes the ring generously so that never happens under the
/// real drain cadence.
template <typename T>
class GraveyardFifo
{
public:
    explicit GraveyardFifo(std::size_t capacityPow2)
        : slots_(capacityPow2), mask_(capacityPow2 - 1)
    {
        // capacityPow2 must be a power of two (mask_ == capacityPow2 - 1).
    }

    /// Audio thread: hand a retired pointer to the graveyard. Returns false if
    /// the ring is full (the caller must size the ring so this never happens
    /// under the real drain cadence). Wait-free; no allocation, and no
    /// destructor of T runs here (the slot only takes ownership of the moved-in
    /// pointer; the previous slot value was already popped and destroyed on the
    /// message thread).
    [[nodiscard]] bool push(std::shared_ptr<const T> ptr) noexcept
    {
        const std::size_t w = writeIdx_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & mask_;
        if (next == readIdx_.load(std::memory_order_acquire))
            return false; // full
        slots_[w] = std::move(ptr);
        writeIdx_.store(next, std::memory_order_release);
        return true;
    }

    /// Message thread: pop one retired pointer (and thereby release it when the
    /// returned value goes out of scope). Returns an empty shared_ptr when the
    /// graveyard is empty.
    [[nodiscard]] std::shared_ptr<const T> pop() noexcept
    {
        const std::size_t r = readIdx_.load(std::memory_order_relaxed);
        if (r == writeIdx_.load(std::memory_order_acquire))
            return {}; // empty
        std::shared_ptr<const T> out = std::move(slots_[r]);
        slots_[r].reset();
        readIdx_.store((r + 1) & mask_, std::memory_order_release);
        return out;
    }

    /// Message thread: true iff at least one pointer is queued.
    [[nodiscard]] bool hasPending() const noexcept
    {
        return readIdx_.load(std::memory_order_acquire)
               != writeIdx_.load(std::memory_order_acquire);
    }

private:
    std::vector<std::shared_ptr<const T>> slots_;
    std::size_t mask_;
    std::atomic<std::size_t> writeIdx_{ 0 };
    std::atomic<std::size_t> readIdx_{ 0 };
};

/// RCU publication slot for one immutable shared_ptr<const T>.
///
/// Producer side (publish / current) runs on a non-audio thread. The audio side
/// (acquire / retire) is wait-free and allocation-free and never frees a buffer.
template <typename T>
class Published
{
public:
    using Ptr = std::shared_ptr<const T>;

    /// graveyardCapacity must be a power of two and comfortably larger than the
    /// number of distinct pointers the audio thread can retire between two
    /// message-thread collectGarbage() calls. Default 1024 covers a publish
    /// storm at audio-block rate against a 30 Hz UI timer with wide margin.
    explicit Published(std::size_t graveyardCapacity = 1024)
        : graveyard_(graveyardCapacity)
    {
    }

    /// Producer thread: publish a new immutable value. The previously published
    /// pointer's refcount is decremented here, on the producer thread (NOT the
    /// audio thread). A publish never frees a buffer the audio thread is using:
    /// the audio thread always holds its own refcount for the duration of a
    /// block. Atomic store with release ordering so acquire() sees a fully
    /// constructed value.
    void publish(Ptr value) noexcept
    {
        std::atomic_store_explicit(&slot_, std::move(value),
                                   std::memory_order_release);
    }

    /// Producer / message thread: read the currently published pointer.
    [[nodiscard]] Ptr current() const noexcept
    {
        return std::atomic_load_explicit(&slot_, std::memory_order_acquire);
    }

    /// Audio thread: copy the published pointer once for this block (RCU). The
    /// returned shared_ptr keeps the buffer alive for as long as the audio
    /// thread holds it, even across a concurrent publish(). Wait-free; the
    /// only cost is a refcount increment (no allocation, no lock).
    [[nodiscard]] Ptr acquire() const noexcept
    {
        return std::atomic_load_explicit(&slot_, std::memory_order_acquire);
    }

    /// Audio thread: hand a per-block pointer back to the graveyard instead of
    /// dropping its refcount inline (which could trigger a free on the audio
    /// thread if it held the last reference). After this returns, `ptr` is
    /// empty. Wait-free. Returns false only if the graveyard ring is full —
    /// a sizing error; in that case the pointer is dropped here as a last
    /// resort (correctness preserved, RT-safety degraded — never expected with
    /// the default capacity).
    bool retire(Ptr& ptr) noexcept
    {
        if (!ptr)
            return true;
        const bool ok = graveyard_.push(std::move(ptr));
        ptr.reset(); // either already moved-from (empty) or the fallback drop
        return ok;
    }

    /// Message thread: drain the graveyard, freeing retired buffers here.
    /// Returns the number of pointers collected (for tests / diagnostics).
    std::size_t collectGarbage() noexcept
    {
        std::size_t n = 0;
        while (graveyard_.pop()) // the returned ptr dies at the end of each iter
            ++n;
        return n;
    }

    /// Message thread: true iff the graveyard still holds retired pointers.
    [[nodiscard]] bool hasGarbage() const noexcept { return graveyard_.hasPending(); }

private:
    Ptr slot_{};
    GraveyardFifo<T> graveyard_;
};

} // namespace mws::plugin
