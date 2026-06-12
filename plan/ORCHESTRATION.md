# mwStime — Orchestration Plan

**Project:** A cross-platform audio plugin (VST3/AU/CLAP/LV2) that faithfully emulates the
timestretch of the Akai S-series samplers (S900/S950/S1000/S1100, plus any models research
justifies adding), with a skeuomorphic S-series UI and per-model character switching.

**Decisions locked with the project owner (2026-06-12):**

| Decision | Choice |
|---|---|
| Plugin type | Hybrid, **FX-first**: timestretch effect on loaded/incoming audio, plus sample audition/re-pitch playback |
| Framework | **JUCE 8** (CMake, FetchContent), released **open-source under AGPLv3** |
| Authenticity | **Authentic DSP core** (era-correct artifacts, per-model bit depth/filter character) + **modern UX** (real-time preview, host tempo sync, drag-and-drop) |
| Platforms | macOS + Linux required; Windows third goal |
| Budget | Full treatment: deep research, parallel agent fleets, adversarial QA |
| UI | Looks like an S-series sampler (grey chassis, green/amber LCD, soft keys, jog wheel) |
| Models | S900, S950, S1000, S1100 minimum; add others only if research shows distinct sonic benefit |
| Repo workflow | Everything lands in github.com/mattWoolly/mwStime in real time. Research/plan → main directly. Dev tasks → branch + PR + agent review → merge. |
| CI | GitHub Actions added **late** (it slows iteration, esp. Windows); local build/test until then |

## Phases

1. **Research** (docs/research/) — deep-research workflow: Akai timestretch algorithm,
   per-model hardware character, prior art (Akaizer etc.), and 90s/00s jungle/IDM/breakbeat
   usage. Adversarially verified, cited.
2. **Scoping & architecture** (docs/design/) — agent panel proposes DSP + plugin + UI
   architecture; team recommendation wins. Output: design docs + ADRs in plan/decisions/.
3. **Backlog** (plan/backlog/) — atomic task files, each independently executable by a
   single dev agent with minimal context. See plan/backlog/README.md for the task format.
4. **Development** — specialty agents pull backlog tasks in dependency order. Each task:
   branch → implement (TDD where it fits) → local build + tests pass → push → PR → review
   agent → merge. Parallel tasks use git worktrees.
5. **QA** — adversarial QA fleet: pluginval, DSP regression renders, null/character tests
   per model, UI checks, host smoke tests.
6. **CI** — GitHub Actions for macOS/Linux (+ Windows last): build, pluginval, unit tests,
   release artifacts.

## Operating rules for agents

- Tasks are defined in plan/backlog/NNN-*.md. Do exactly the task's scope; no scope creep.
- Never commit directly to main during the dev phase; always branch + PR.
- Branch naming: task/NNN-short-slug. PR title: "NNN: <task title>".
- Local verification before any PR: configure + build + ctest must pass.
- DSP claims must trace to docs/research/ or be marked as a deliberate deviation in an ADR.
