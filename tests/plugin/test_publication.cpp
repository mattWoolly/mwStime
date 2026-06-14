// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Publication stress / ThreadSanitizer test for the RCU publication protocol
// (plan/backlog/030; docs/design/architecture.md §4 ownership/publication
// protocol; docs/design/testing-strategy.md §3 item 6 — TSan over the
// render-publish/swap path and FX history reconfiguration).
//
// What is asserted (the protocol made falsifiable):
//   - N producer publish() swaps run concurrently with a fake audio thread that
//     copies the pointer once per "block" (acquire), reads every sample, and
//     hands the pointer back via retire() — no use-after-free (ASan/TSan-clean).
//   - The buffer is immutable once published: a per-buffer sentinel sample read
//     on the audio side always matches the value baked at construction (a free
//     mid-block would corrupt it; a torn publish would read a half-built ptr).
//   - The graveyard drains: after deterministic shutdown collectGarbage() frees
//     every retired pointer and the slot's final value is the last published
//     one, so nothing leaks.
//   - GraveyardFifo SPSC push/pop ordering is FIFO and reports full/empty.
//
// This translation unit is JUCE-free (Published.h depends only on the C++20
// standard library) so it compiles into the core test binary and runs under the
// `tsan` preset. Test-case names begin with "publication" so the verification
// command `ctest -R publication --no-tests=error` matches what
// catch_discover_tests registers (plan/backlog/README.md test-selection rules).

#include <catch2/catch_test_macros.hpp>

#include "Published.h"

// The test binary's single global operator-new replacement (test_butterworth.cpp)
// lets us assert the audio-thread acquire()/retire() path allocates NOTHING
// (architecture.md §4: no allocation on the audio thread).
#include "TestAllocationCounter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using mws::plugin::GraveyardFifo;
using mws::plugin::Published;

// A stand-in for RenderedSample: an immutable buffer that carries a sentinel
// equal to (id replicated across every sample). The audio thread checks that
// every sample still equals the sentinel — a use-after-free or a torn publish
// would break this invariant.
struct ImmutableBuffer {
    explicit ImmutableBuffer(std::uint64_t id, std::size_t n)
        : sentinel(id), samples(n, static_cast<float>(id))
    {
    }
    std::uint64_t sentinel;
    std::vector<float> samples;

    [[nodiscard]] bool consistent() const noexcept
    {
        const auto expected = static_cast<float>(sentinel);
        for (float s : samples)
            if (s != expected)
                return false;
        return true;
    }
};
} // namespace

TEST_CASE("publication: GraveyardFifo is FIFO and reports full/empty", "[publication]")
{
    // Capacity 4 => 3 usable slots (one always left empty).
    GraveyardFifo<ImmutableBuffer> g{ 4 };
    REQUIRE_FALSE(g.hasPending());
    REQUIRE(g.pop() == nullptr);

    auto a = std::make_shared<const ImmutableBuffer>(1, 1);
    auto b = std::make_shared<const ImmutableBuffer>(2, 1);
    auto c = std::make_shared<const ImmutableBuffer>(3, 1);
    auto d = std::make_shared<const ImmutableBuffer>(4, 1);

    REQUIRE(g.push(a));
    REQUIRE(g.push(b));
    REQUIRE(g.push(c));
    REQUIRE_FALSE(g.push(d)); // full: only 3 usable slots
    REQUIRE(g.hasPending());

    auto p1 = g.pop();
    REQUIRE(p1 != nullptr);
    REQUIRE(p1->sentinel == 1); // FIFO order
    auto p2 = g.pop();
    REQUIRE(p2->sentinel == 2);
    auto p3 = g.pop();
    REQUIRE(p3->sentinel == 3);
    REQUIRE(g.pop() == nullptr);
    REQUIRE_FALSE(g.hasPending());

    // Room again after draining.
    REQUIRE(g.push(d));
    REQUIRE(g.pop()->sentinel == 4);
}

TEST_CASE("publication: retire never frees, collectGarbage frees on message thread",
          "[publication]")
{
    Published<ImmutableBuffer> pub{ 8 };

    // Track live buffers via a custom deleter so we can prove the free does NOT
    // happen at retire() and DOES happen at collectGarbage().
    std::atomic<int> liveCount{ 0 };
    auto makeTracked = [&liveCount](std::uint64_t id) {
        liveCount.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<const ImmutableBuffer>(
            new ImmutableBuffer(id, 16),
            [&liveCount](const ImmutableBuffer* p) {
                liveCount.fetch_sub(1, std::memory_order_relaxed);
                delete p;
            });
    };

    pub.publish(makeTracked(10)); // first published; refcount held by slot
    REQUIRE(liveCount.load() == 1);

    // Audio thread acquires a copy, then a new value is published.
    auto held = pub.acquire();
    REQUIRE(held->sentinel == 10);
    pub.publish(makeTracked(11)); // slot drops its ref to #10, but `held` keeps it
    REQUIRE(liveCount.load() == 2); // #10 (held by audio) + #11 (slot)

    // Audio thread retires its copy. The pointer must NOT be freed here.
    REQUIRE(pub.retire(held));
    REQUIRE(held == nullptr);        // moved into the graveyard
    REQUIRE(liveCount.load() == 2);  // still alive: sitting in the graveyard
    REQUIRE(pub.hasGarbage());

    // Message thread drains: NOW #10 is freed.
    const std::size_t collected = pub.collectGarbage();
    REQUIRE(collected == 1);
    REQUIRE_FALSE(pub.hasGarbage());
    REQUIRE(liveCount.load() == 1);  // only #11 (still published) remains
}

TEST_CASE("publication: audio-thread acquire/retire allocates nothing", "[publication]")
{
    // The audio-thread path is acquire() (one shared_ptr copy = refcount inc)
    // then retire() (push into the PRE-allocated graveyard ring). Neither may
    // allocate (architecture.md §4: no allocation on the audio thread).
    Published<ImmutableBuffer> pub{ 64 };
    pub.publish(std::make_shared<const ImmutableBuffer>(7, 128));

    // Warm up so any first-touch lazy allocation is out of the way.
    {
        auto warm = pub.acquire();
        (void) pub.retire(warm);
        pub.collectGarbage();
    }

    const auto before = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);
    for (int block = 0; block < 1000; ++block)
    {
        auto buf = pub.acquire();   // refcount inc only
        REQUIRE(buf->sentinel == 7);
        REQUIRE(pub.retire(buf));   // push into the preallocated ring
        // Drain on the "message thread" between blocks to keep the ring from
        // filling — collectGarbage may free (allowed; it is message-thread).
        pub.collectGarbage();
    }
    const auto after = mwsTestGlobalAllocationCount.load(std::memory_order_relaxed);

    // acquire()+retire() across 1000 blocks must not allocate. (collectGarbage
    // frees but never allocates either, so the net delta is exactly zero.)
    REQUIRE(after == before);
}

TEST_CASE("publication: concurrent publish vs per-block acquire/retire is race-free",
          "[publication][tsan]")
{
    // Sized for the stress: a busy producer can swap thousands of times while
    // the audio thread retires one pointer per block. 4096-slot graveyard
    // drained periodically by a message thread leaves wide margin.
    Published<ImmutableBuffer> pub{ 4096 };

    constexpr std::uint64_t kPublishes = 50000;
    constexpr std::size_t kBufferFrames = 64;

    // Publish an initial value so the audio thread always has something.
    pub.publish(std::make_shared<const ImmutableBuffer>(0, kBufferFrames));

    std::atomic<bool> producerDone{ false };
    std::atomic<std::uint64_t> blocksProcessed{ 0 };
    std::atomic<std::uint64_t> consistencyFailures{ 0 };
    // Catch2's REQUIRE is not thread-safe; record retire failures on the worker
    // and assert after join (the in-thread REQUIRE aborted under CI contention —
    // task 051). Same safe pattern already used for consistencyFailures.
    std::atomic<std::uint64_t> retireFailures{ 0 };

    // Producer: rapid immutable publishes, exactly the render-worker swap path.
    std::thread producer([&] {
        for (std::uint64_t id = 1; id <= kPublishes; ++id)
            pub.publish(std::make_shared<const ImmutableBuffer>(id, kBufferFrames));
        producerDone.store(true, std::memory_order_release);
    });

    // Fake audio thread: copy the pointer ONCE per block (RCU), read every
    // sample (would trip ASan/TSan on a use-after-free or torn read), then hand
    // the pointer back to the graveyard. Never allocates, never frees.
    std::thread audio([&] {
        while (!producerDone.load(std::memory_order_acquire))
        {
            for (int block = 0; block < 256; ++block)
            {
                auto buf = pub.acquire(); // one copy per block
                if (buf)
                {
                    if (!buf->consistent())
                        consistencyFailures.fetch_add(1, std::memory_order_relaxed);
                    blocksProcessed.fetch_add(1, std::memory_order_relaxed);
                }
                // Yield-and-retry on a momentarily-full graveyard (collector
                // behind under load); the buffer is never stranded (it is held by
                // `buf`). Bounded so a permanent stall still fails. See the
                // matching note in the "many producers' swaps" test below.
                for (int attempt = 0; !pub.retire(buf); ++attempt)
                {
                    if (attempt >= 1'000'000)
                    {
                        retireFailures.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        }
    });

    // Message thread: drain the graveyard concurrently (the real timer poll).
    std::thread collector([&] {
        while (!producerDone.load(std::memory_order_acquire))
            pub.collectGarbage();
    });

    producer.join();
    audio.join();
    collector.join();

    // Deterministic shutdown: retire whatever the audio thread held last via a
    // final acquire/retire, then drain everything so nothing leaks.
    auto tail = pub.acquire();
    REQUIRE(pub.retire(tail));
    pub.collectGarbage();

    REQUIRE(consistencyFailures.load() == 0); // no torn publish / use-after-free
    REQUIRE(retireFailures.load() == 0);       // graveyard never overflowed
    REQUIRE(blocksProcessed.load() > 0);       // the audio thread actually ran
    REQUIRE_FALSE(pub.hasGarbage());           // graveyard fully drained
    REQUIRE(pub.current()->sentinel == kPublishes); // last publish stuck
}

TEST_CASE("publication: many producers' swaps never strand a buffer", "[publication][tsan]")
{
    // The render result, the loaded sample, and FX history reconfig all share
    // ONE Published<T> mechanism (acceptance criterion). Here we stress a single
    // slot under one producer + one audio consumer but with a SMALL graveyard
    // drained on its own thread, proving the steady-state never strands the
    // currently published buffer and the final state is clean.
    Published<ImmutableBuffer> pub{ 256 };

    std::atomic<int> liveCount{ 0 };
    auto makeTracked = [&liveCount](std::uint64_t id) {
        liveCount.fetch_add(1, std::memory_order_relaxed);
        return std::shared_ptr<const ImmutableBuffer>(
            new ImmutableBuffer(id, 32),
            [&liveCount](const ImmutableBuffer* p) {
                liveCount.fetch_sub(1, std::memory_order_relaxed);
                delete p;
            });
    };

    pub.publish(makeTracked(0));

    constexpr std::uint64_t kPublishes = 20000;
    std::atomic<bool> producerDone{ false };

    // Catch2's REQUIRE is NOT thread-safe — asserting from a spawned thread is
    // UB (it surfaced as a "fatal error condition" abort under CI contention, and
    // pub.acquire() can legitimately return null transiently mid-swap, so the
    // old `REQUIRE(buf->consistent())` also dereferenced null). Mirror the safe
    // pattern the first test in this file already uses: record failures into
    // atomic counters on the worker threads and REQUIRE on the main thread after
    // join. The stress is unchanged (20000 publishes, concurrent acquire/retire/
    // collect). (Hardened by the first CI run — task 051.)
    std::atomic<std::uint64_t> consistencyFailures{ 0 };
    std::atomic<std::uint64_t> retireFailures{ 0 };

    std::thread producer([&] {
        for (std::uint64_t id = 1; id <= kPublishes; ++id)
            pub.publish(makeTracked(id));
        producerDone.store(true, std::memory_order_release);
    });

    std::thread audio([&] {
        while (!producerDone.load(std::memory_order_acquire))
        {
            auto buf = pub.acquire();
            if (buf && !buf->consistent())
                consistencyFailures.fetch_add(1, std::memory_order_relaxed);
            // retire() returns false only when the graveyard ring is momentarily
            // FULL (the collector thread is behind) — the buffer is NOT stranded,
            // it is still owned by `buf`, exactly as the real audio thread holds
            // it until the next opportunity. Yield and retry until the collector
            // drains a slot (bounded so a genuine permanent stall still fails).
            // The original in-thread `REQUIRE(retire(...))` assumed the collector
            // always keeps up, which a contended CI runner does not guarantee.
            for (int attempt = 0; !pub.retire(buf); ++attempt)
            {
                if (attempt >= 1'000'000)
                {
                    retireFailures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
            }
        }
    });

    std::thread collector([&] {
        while (!producerDone.load(std::memory_order_acquire))
            pub.collectGarbage();
    });

    producer.join();
    audio.join();
    collector.join();

    REQUIRE(consistencyFailures.load() == 0);
    REQUIRE(retireFailures.load() == 0);

    // Shutdown drain.
    auto tail = pub.acquire();
    REQUIRE(pub.retire(tail));
    pub.collectGarbage();

    REQUIRE_FALSE(pub.hasGarbage());
    REQUIRE(pub.current()->sentinel == kPublishes);

    // Exactly one buffer is still alive: the one held by the slot. Dropping the
    // slot frees it (message-thread destruction at Published teardown).
    REQUIRE(liveCount.load() == 1);
}
