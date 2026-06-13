// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// The plugin test binary's single global operator-new replacement, mirroring the
// core test binary's owner (tests/unit/test_butterworth.cpp). It lets the
// audio-thread no-allocation assertions in tests/plugin/ (task 034
// SamplePlayer / ZONE-preview paths) snapshot mwsTestGlobalAllocationCount around
// the calls under test. There must be EXACTLY ONE such replacement per binary;
// this is it for mwstime_plugin_tests. Counting is branch-free and always on.

#include "TestAllocationCounter.h"

#include <atomic>
#include <cstdlib>
#include <new>

std::atomic<std::size_t> mwsTestGlobalAllocationCount{ 0 };

void* operator new(std::size_t size)
{
    mwsTestGlobalAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size != 0 ? size : 1))
        return p;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
