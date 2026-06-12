# Contributing

Stub — expanded as the project grows. Workflow rules live in
`plan/ORCHESTRATION.md` and `plan/backlog/README.md`.

## License headers (AGPLv3)

mwStime is licensed AGPL-3.0-or-later (see `LICENSE`). Every C/C++ source file
under `libs/`, `plugin/`, `tools/`, and `tests/` (`.h`, `.hpp`, `.cpp`, `.cc`,
`.mm`) must start with this two-line header:

```cpp
// SPDX-License-Identifier: AGPL-3.0-or-later
// mwStime — Akai S-series timestretch emulation. Copyright (C) 2026 mattWoolly.
```

CMake and shell files use the same two lines with `#` comments.

The convention is enforced by `tools/check_license_headers.sh`, registered as
the `license_headers` ctest — it fails when any committed source file lacks
the SPDX line within its first three lines, so every task inherits the check
automatically via `ctest --preset default`.

## Build / test

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```
