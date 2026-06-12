// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
//
// Explicit per-version state migrations (task 029, architecture.md §6).

#include "Migrations.h"

#include "StateTree.h"

namespace mws::plugin::state {

namespace {

/// v0 → v1. Version 0 is any pre-versioned tree (no stateVersion property,
/// e.g. a dev build's state from before this scheme existed). Known fields
/// are kept as-is; missing fields are left for ensureDefaults; the version
/// stamp is added.
juce::ValueTree migrateV0ToV1(juce::ValueTree tree)
{
    tree.setProperty(id::stateVersion, 1, nullptr);
    return tree;
}

} // namespace

juce::ValueTree migrate(juce::ValueTree stateTree, int fromVersion)
{
    // Future version (newer build's state loaded into this one): we cannot
    // know the schema, so fall back to safe defaults and flag it.
    if (fromVersion > kStateVersion)
    {
        auto fallback = createDefault();
        fallback.setProperty(id::unknownVersionFallback, true, nullptr);
        return fallback;
    }

    // Explicit upgrade chain. Each step bumps exactly one version.
    if (fromVersion < 1)
        stateTree = migrateV0ToV1(std::move(stateTree));

    // fromVersion == kStateVersion (current, 1): identity.
    return stateTree;
}

} // namespace mws::plugin::state
