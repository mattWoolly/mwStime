---
id: 031
title: FileLoader thread — decode WAV/AIFF/FLAC to SourceSample, publish via RCU
status: done
depends-on: [027, 030]
component: plugin
estimated-size: M
---

## Objective
Off-thread file loading: dropped/chosen audio decodes on a file-loader thread into an
immutable `SourceSample` (float32, original sample rate kept) published through the
task-030 protocol, with hardware-idiom error reporting hooks.

## Context
Read first:
- docs/design/architecture.md §4 (FILE LOADER THREAD box), §5.1 (SourceSample in the
  data flow)
- docs/design/ui-design.md §6.1 (load flow + error idiom; WAV/AIFF/FLAC, no MP3 at v1)
- Published<T>/threading (030)

## Scope
- `plugin/src/FileLoader.{h,cpp}`:
  - juce::AudioFormatManager limited to WAV/AIFF/FLAC; decode to float32 keeping the
    file's sample rate; capture name, length, channels, content hash (for state's
    sourceFile field, architecture.md §6),
  - runs on its own thread (juce::Thread or ThreadPool job); cancellation-safe if a
    new load supersedes an in-flight one,
  - publishes `std::shared_ptr<const SourceSample>` via `Published<T>`,
  - typed error results (unsupported format, read failure) posted to the UI FIFO —
    LCD wording is UI-layer (ui-design §6.1),
  - extension hook: "sample changed" notification that task 032 uses to pre-encode
    the FLAC state blob.
- Tests (`tests/plugin/test_fileloader.cpp`): load a generated WAV and FLAC (write
  fixtures with JUCE writers in-test); correct rate/length/hash; unsupported
  extension yields the typed error; superseding load wins deterministically.

## Out of scope
- Drag-and-drop UI target (task 043 WaveformView) and file-chooser UI.
- FLAC state-blob pre-encode itself (task 032).

## Acceptance criteria
- [ ] Tests pass; decode never happens on the message thread (assert thread in debug).
- [ ] Publication uses the task-030 mechanism (no new atomics).

## Verification commands
```
cmake --preset default
cmake --build --preset default
ctest --preset default -R fileloader --no-tests=error
```
