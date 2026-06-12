# ADR 005: UI rendering approach — pure vector JUCE drawing, data-driven faceplates

Status: accepted
Date: 2026-06-12

## Context

Locked: skeuomorphic S-series look (grey chassis, green/amber LCD, soft keys, jog
wheel), per-model faceplate switching, AGPLv3 open-source repo, macOS + Linux first.
Four models at v1 (+S3000 slot reserved, ADR-004) each need a faceplate variant; the
UI must be resizable, HiDPI-clean, and — critically — buildable and reviewable by
autonomous agents with no human designer.

## Options considered

Panel positions — **unanimous** on pure procedural vector rendering; nuances differed:
- **Authenticity purist**: fully procedural vector, layout descriptor + LookAndFeel
  per model, including both hardware display paradigms (S950 2-line fluorescent AND
  S1000 graphic LCD + F1–F8 + jog) plus workflow theater (countdown, hold-F8 abort,
  NAME entry, keypad as v1.1 decoration). Its critique trimmed this: the
  dual-paradigm UI + theater was a scope-explosion item; NAME/keypad have no product
  function in a plugin.
- **Product designer**: "100% vector/procedural — agents can build this, PNG
  pipelines they cannot"; LCD as the hero component with glow/dot-grid; skip the
  numeric pad; one component tree, themes as a token set.
- **Pragmatic engineer**: one parametric faceplate component + per-model
  `SkinDescriptor` data — geometry never forks; LCD page contents transcribed from
  the manuals' printed screens so "design" is transcription, not invention; golden
  screenshot tests. Its critique corrected the screenshot claim: JUCE AA/font
  rasterization differs across platforms — screenshot gates are reference-platform
  (software renderer) only.

Option space:
- **A. Bitmap/photoreal skins.** Photo-derived art of real Akai units is a rights
  risk in an AGPL repo; assets explode across models/scales; agents can't diff
  images; resizing blurs. Rejected.
- **B. Pure vector `juce::Graphics`/`Path` in a custom LookAndFeel, static layers
  cached per scale, faceplates driven by a `FaceplateSpec` struct; bespoke 5×7 LCD
  pixel font in code.** Resolution-independent, tiny repo, clean-room, data-driven
  variation, reviewable in diffs. Chosen.
- **C. WebView UI (JUCE 8).** Linux WebView fragility, host-window quirks. Rejected.
- **D. OpenGL renderer.** Unneeded for a mostly-static faceplate; Linux driver tax.
  Rejected for v1.

Scope rulings from the critiques, adopted: no numeric keypad at v1 (dead UI in a
DAW; double-click fields for direct entry); no NAME-entry theater (display-only
"new sample" line); both LCD layout classes (S950 2-line, S1000 page) ARE kept — they
are data variants of one `LcdDisplay` component, not two component trees, so the
atomicity objection doesn't apply.

## Decision

Option B, as specified in docs/design/ui-design.md: vector `SeriesLookAndFeel`,
cached static layers, `FaceplateSpec`-driven model variants over ONE parametric
faceplate component (geometry never forks per model), code-defined LCD pixel font,
fixed-aspect resizable 0.6×–2.0× from a 1000×380 base, no OpenGL attach at v1.
UI golden screenshots gate on the reference platform only (macOS arm64, software
renderer); Linux uses tolerance comparison.

## Consequences

- All art is C++ — drawing tasks are normal reviewable backlog items.
- A future photoreal skin would be a separate LookAndFeel, not a rewrite.
- We accept "stylized, unmistakably S-series" rather than photo-identical; trademark
  caution: model wordmarks rendered as plain text descriptors, no Akai logo.
- The S3000 faceplate at v1.1 is one `FaceplateSpec` row (ADR-004).
