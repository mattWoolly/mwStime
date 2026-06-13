// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Shared access to the test binary's global allocation counter. The ONE
// global operator-new/delete replacement lives in test_butterworth.cpp (task
// 005 — first owner); every allocation-freedom test (Butterworth setCutoff,
// RealtimeStretcher process, task 022) snapshots this counter around the
// calls under test. Counting is branch-free and always on.

#pragma once

#include <atomic>
#include <cstddef>

/// Defined in test_butterworth.cpp next to the operator-new replacement.
extern std::atomic<std::size_t> mwsTestGlobalAllocationCount;
