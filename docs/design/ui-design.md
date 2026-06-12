# mwStime — UI Design Specification (S-series skeuomorphic)

Status: accepted (panel synthesis), 2026-06-12. Implements ADR-005; locked decision:
"Looks like an S-series sampler (grey chassis, green/amber LCD, soft keys, jog wheel)"
(plan/ORCHESTRATION.md).

Sources: hardware UI structure and parameter pages from
`docs/research/akai-manuals-specs.md` [MAN]; LCD page contents mirror the manuals'
printed screens (so "design" is transcription, not invention — panel point). All
colors/dimensions are **stylistic pragmatic inventions** evoking the period hardware —
we do not claim Pantone-accurate reproduction, and we use no photographic assets
(clean-room vector art only; safe for AGPLv3 distribution).

Panel consensus: 100% procedural vector rendering (all three lenses independently
chose it), ONE parametric faceplate component restyled per model by a data descriptor
(no per-model component forks — the atomic-agent constraint), no numeric keypad at v1,
LCD as the hero component.

---

## 1. Layout

Base canvas 1000 × 380 px (rack-unit proportions ≈ 19" × 3U). ASCII mockup
(S1000-family layout shown):

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ ◉ POWER   mwStime           S 1 0 0 0   MIDI STEREO DIGITAL SAMPLER     [≡ menu] │
│ ┌────────────────────────────────────────────────────┐  ┌──────────────────────┐ │
│ │ ████████████████████  LCD  ████████████████████████│  │   MODEL SELECTOR     │ │
│ │  TIME-STRETCH        sample: AMEN_165              │  │ ┌──────────────────┐ │ │
│ │  stretch zone:      0  to:  131071                 │  │ │ S900  S950 S1000 │ │ │
│ │  Cycle length:   1000   time factor:  300%         │  │ │ S1100  (·v1.1·)  │ │ │
│ │  stretch mode: CYCLIC   qual: -- width: --  (grey) │  │ └──────────────────┘ │ │
│ │  new sample: AMEN_165*ST          mem:  7%         │  │  MODE  ◉ FX ○ SAMPLE │ │
│ └────────────────────────────────────────────────────┘  │  TIMING ◉ CLASSIC    │ │
│  [F1]   [F2]   [F3]   [F4]   [F5]   [F6]   [F7]  [F8]   │         ○ REVISED    │ │
│  TIME   autC   ZONE   GO    PLAY   A/B    SYNC  ABORT   │  CHARACTER [ON]      │ │
│ ┌──────────────────────────────┐   ┌───────────┐        │  WINDOW [1 BAR] (FX) │ │
│ │  WAVEFORM / DROP SAMPLE HERE │   │  JOG       │       │  OUTPUT   (knob)     │ │
│ │  ▁▂▅█▆▃▂▁▂▇█▅▂▁ ▷ play head  │   │  WHEEL ◯   │       └──────────────────────┘ │
│ └──────────────────────────────┘   │ (+/- ring) │   ┌─────────────────────────┐  │
│  ENT  ◄  ►  ▲  ▼   (cursor keys)   └───────────┘    │ ▦ floppy slot (easter   │  │
│                                                     │   egg: drag-out render) │  │
└──────────────────────────────────────────────────────────────────────────────────┘
```

Regions:
1. **Header strip** — power LED, product name, model wordmark (changes per model),
   hamburger menu (about, scale, manual).
2. **LCD** — the centerpiece; renders the authentic page text for the active model
   (S950 shows the 2-line Page-14 STRETCH layout [MAN §2 p.30]; S1000-family shows the
   multi-line TIME-STRETCH page [MAN §3, §5 example screen]). `qual`/`width` render
   greyed with "INTELL only" at v1 (INTELL deferred — dsp-engine.md §4); the
   hardware itself greys them in CYCLIC mode [MAN §3 p.47], so the UI documents the
   gap authentically.
3. **Soft key bar F1–F8** — functions relabel per page/model (hardware behavior). Key
   assignments above are the TIME page set; ABORT honors "hold F8 to abort" [MAN §3
   p.47] (hold ≥ 600 ms (PI)).
4. **Model selector + plugin-level controls** — model buttons (four at v1; a fifth
   blank badge position is reserved for the S3000, ADR-004), FX/SAMPLE, TIMING
   (CLASSIC/REVISED), CHARACTER on/off, FX WINDOW selector, output trim knob.
5. **Waveform / drop zone** — modern UX: drag-and-drop in, drag-out of renders,
   stretch-zone handles (maps to "stretch zone / to" [MAN §3]).
6. **Jog wheel + cursor/ENT keys** — value entry: cursor keys move the LCD field
   cursor; jog wheel edits the focused field (drag-rotate or mouse-wheel); ENT
   commits/triggers. **No numeric keypad** (panel-unanimous v1 cut — dead UI in a
   DAW); double-click any LCD field for direct text entry instead.

## 2. Components (JUCE)

| Component | Class | Notes |
|---|---|---|
| Faceplate | `Faceplate : Component` | draws chassis from `FaceplateSpec`; cached to `juce::Image` per scale (redrawn only on resize/model change) |
| LCD | `LcdDisplay : Component` | character-cell grid (40×6 cells (PI); S950 page uses a 40×2 region to match its hardware display class), custom 5×7 pixel font rendered as `juce::Path` glyphs; backlight glow via radial gradient + subtle scanline overlay; all text content driven by an `LcdPageModel` (pure C++ struct, unit-testable without GUI) |
| Soft keys | `SoftKeyBar` | 8 `DrawableButton`s, rounded-rect caps, labels from active page model |
| Jog wheel | `JogWheel : Slider` (rotary, endless) | concentric ring + dimple, velocity-sensitive; fine mode with Shift |
| Model selector | `ModelSelector` | latching buttons styled as rack-badge tabs (4 at v1 + reserved slot); switching cross-fades faceplate spec over 150 ms (PI) |
| Waveform | `WaveformView` | cached peaks (own implementation reading the core buffer), zone handles, render-overlay (original vs stretched A/B); live input scope in FX mode |
| Cursor/ENT keys | part of `SoftKeyBar` cluster | keyboard focus support: arrows/Enter mirror them |

`LcdPageModel` is the single source of truth for what the LCD shows; the editor maps
APVTS values → hardware-unit strings (dsp-engine.md §2 table), including engine-clamp
feedback (`999%` cap on S950, `FX MIN 100%` clamp notice in FX FREE mode — ADR-006).
This keeps "LCD shows authentic units" testable headlessly.

## 3. Per-model faceplate variations (`FaceplateSpec`)

One struct drives everything — adding a model is data, not code (this is how the S3000
arrives at v1.1 without UI rework):

```cpp
struct FaceplateSpec {
  ModelId id;
  juce::Colour chassis, chassisEdge, legend, accent;
  juce::Colour lcdBack, lcdInk, lcdGlow;
  const char* wordmark;          // e.g. "S950 MIDI DIGITAL SAMPLER"
  LcdLayout   lcdLayout;         // S950_2LINE or S1000_PAGE
  bool        showsModeRow;      // CYCLIC/INTELL row (false on S900/S950)
  bool        showsQualWidth;    // greyed at v1 (INTELL deferred); false on S900/S950
  ParamVisibility visibility;    // which fields the LCD page exposes (per dsp-engine §2)
};
```

Geometry of chassis/keys/wheel **never forks per model** — only palette, wordmark, LCD
layout class, and page content change (panel: single parametric component, themes as
data).

Palette table (all **(PI)**, styled after period hardware):

| Model | Chassis | Legend | Accent | LCD back / ink | Vibe |
|---|---|---|---|---|---|
| S900 | near-black charcoal `#26282B` | off-white `#E8E4D8` | red-orange `#D84B20` | dark olive `#2E3324` / amber `#FFB02E` | 1986; LCD shows the varispeed page + "S900: NO TIMESTRETCH" notice (ADR-003) |
| S950 | charcoal `#2C2E31` | off-white | amber `#E09520` | olive `#333826` / amber `#FFB02E` | 2-line LCD, "PAGE 14 STRETCH" layout [MAN §2] |
| S1000 | warm light grey `#B9B6AE` | dark grey `#2A2A2A` | teal-green `#1F7A6B` | deep green `#1A3A24` / green `#5CFF7A` | the canonical grey + green look (locked decision) |
| S1100 | mid grey `#9FA0A0` | dark grey | blue `#27557E` | deep green / green | adds AES/SMPTE badge row text |
| S3000 *(v1.1)* | light grey `#C4C2BC` | dark grey | violet-grey `#5A5470` | deep green / green | wider LCD bezel, "qual/width" defaults visible |

## 4. Rendering approach (ADR-005)

- **Pure vector**: everything drawn with `juce::Graphics`/`Path`/gradients in a custom
  `SeriesLookAndFeel`. No PNG/JPEG skin assets. Rationale: infinitely resizable, tiny
  repo, no photo-derived copyright risk under AGPLv3, agent-reviewable in diffs, and
  faceplate variations stay data-driven.
- **Caching**: static layers (chassis, screws, legends) rendered once per
  scale/model into a `juce::Image`; dynamic layers (LCD text, LEDs, wheel angle,
  waveform) repaint on top. Target: full repaint < 2 ms at 1.0 scale (PI budget).
- LCD pixel font: bespoke 5×7 glyph set defined as bit patterns in a header
  (`LcdFont.h`), drawn as rounded-rect "pixels" — period-correct look, no font-file
  licensing.
- OpenGL attach is NOT used at v1 (Linux driver variance; software renderer is enough
  for this UI).
- Screenshot regression: vector rendering is deterministic only with the software
  renderer **on one reference platform** (CoreGraphics vs Linux AA/rasterization
  differ — panel critique); UI golden screenshots are macOS-arm64-only gates,
  Linux gets a tolerance compare (testing-strategy.md).

## 5. Sizing / resizability

- Base 1000 × 380; `setResizable(true, true)` with `ResizableCornerComponent`,
  fixed aspect ratio, limits 0.6×–2.0×; scale persisted in plugin state
  (architecture.md §6). Rendering is resolution-independent (vector), so HiDPI is free.

## 6. Interaction flows

### 6.1 Load sample (SAMPLE mode)
1. Drag file onto waveform region (or F-menu → load). FileLoader decodes off-thread
   (WAV/AIFF/FLAC; no MP3 at v1).
2. LCD top line shows sample name; waveform draws; stretch zone defaults to full
   length ("stretch zone / to", [MAN §3]).
3. Errors (unsupported format) shown as an LCD message line — hardware-style
   (e.g. `** WRONG DISK **`-flavored wording (PI)).

### 6.2 Set stretch parameters
1. Cursor keys / click focus an LCD field; jog wheel or direct typing edits it.
2. `timeFactor` edits update the LCD's computed new-length/memory readout live
   ("the length and time of the new stretched sample … are displayed", [MAN §3 p.47]);
   in CLASSIC the *achieved* (schedule-quantized) length is shown, never the
   requested one (the "bad timing" is displayed honestly — dsp-engine.md §3.2).
3. F2 `autC` runs auto cycle detection (dsp-engine.md §7.1) and writes the result
   into the cycle-length field; on S950 the same key reads `AUTO-D` [MAN §2].
4. SYNC (F7): user sets source BPM (typed or tap; filename `_174bpm` auto-guess,
   always overridable); LCD shows e.g. `174.0 -> 87.0 = 200%` (formula:
   dsp-engine.md §2 `tempoSync`).

### 6.3 Audition / render (SAMPLE mode)
1. F3 `ZONE` loops the selected zone through the stretcher at current settings —
   CYCLIC only, exactly as documented [MAN §3 p.46]; implemented as a real-time
   preview through `RealtimeStretcher`.
2. F4 `GO` requests an offline render; LCD shows progress + remaining-time line
   (hardware behavior [MAN §3 p.47]); holding F8 aborts; a render exceeding the
   memory cap is refused with `** NOT ENOUGH MEMORY **` (architecture.md §5.1).
3. F5 `PLAY` plays the render; F6 `A/B` toggles original/stretched (S2000-family
   audition behavior [MAN §5]). MIDI notes also trigger playback (root C3, chromatic
   repitch, monophonic at v1 — architecture.md §4.2).
4. Drag the waveform out to the host/desktop to export the render as WAV.

### 6.4 FX mode (default mode — FX-first, locked)
1. MODE switch to FX: waveform region becomes a live input scope; GO/PLAY/A-B soft
   keys grey out; SYNC (F7) toggles host-tempo-synced time factor; WINDOW selector
   active.
2. Engine follows the ADR-006 causality contract: T<100% in FREE mode shows the
   `FX MIN 100%` clamp on the LCD; SYNC mode shows the window and resync behavior.
   Parameter changes apply at grain boundaries (no smoothing inside the authentic
   scheduler — dsp-engine.md §3.5).
3. LCD shows a live "TIME-STRETCH (REALTIME)" page — clearly non-authentic wording so
   users know this mode exceeds hardware capability.

### 6.5 Model switch
1. Selecting a model swaps `FaceplateSpec` + `ModelSpec`: palette cross-fade, LCD
   re-layout, out-of-range params clamped **at the engine** (host-visible parameter
   ranges never change — architecture.md §6) with the pre-clamp value remembered and
   restored when switching back (PI). S950/S900 selection shows the mono-sum notice.
2. Audio: model switch in FX mode cross-fades engines over one block (PI) and
   triggers a latency re-report (ADR-006 — documented PDC-updating action).

## 7. Accessibility / keyboard

All controls focusable; arrows + Enter mirror cursor/ENT; jog wheel responds to mouse
wheel and Up/Down. Parameter tooltips show both hardware units and host-normalized
value. JUCE accessibility handlers enabled (screen-reader names = LCD field labels).
