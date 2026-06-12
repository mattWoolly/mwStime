// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// State migrations (task 029, architecture.md §6): explicit per-version
// functions. Adding a schema version means bumping state::kStateVersion AND
// adding one migrateVNToVN+1 function here — nothing implicit.

#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace mws::plugin::state {

/// Migrates a non-parameter state tree from `fromVersion` up to the current
/// kStateVersion by chaining the explicit per-version functions.
///
/// - fromVersion == kStateVersion: identity (tree returned as-is).
/// - fromVersion <  kStateVersion: each migrateVNToVN+1 applied in order
///   (version 0 = a pre-versioned tree carrying no stateVersion property).
/// - fromVersion >  kStateVersion (a future build's state): safe defaults,
///   with id::unknownVersionFallback set so the UI can surface it.
///
/// The returned tree always carries stateVersion == kStateVersion.
[[nodiscard]] juce::ValueTree migrate(juce::ValueTree stateTree, int fromVersion);

} // namespace mws::plugin::state
