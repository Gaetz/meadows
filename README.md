# Meadows

A custom C++20 game engine prototype for a **Skyrim-like, open-world,
moddable RPG** — built 2D-first, now growing its 3D gameplay demo. The
lasting asset is the **simulation layer**: a reflected, field-level-patch
data model where the base game, every mod and (eventually) the save file
are the same thing — ordered layers of plugins.

- **Everything is data.** Items, NPCs, spells, schedules, weather, whole
  villages: reflected Forms in `.toml` plugins, resolved field-by-field,
  last writer wins. Two mods editing different fields of the same NPC
  never conflict.
- **Every tool writes plugins.** The in-game Game DB editor and dev
  console export their edits as ordinary mod files.
- **The sim runs headless.** 229+ doctests exercise the whole gameplay
  model without a renderer.
- **Custom GL 4.6 renderer** (Breath-of-the-Wild-style landscape: streamed
  terrain, day/night, weather, CSM, water, volumetric light) behind a
  Vulkan-shaped RHI.

## Documentation

| For | Read |
|---|---|
| **Players & modders** | [`userdoc/README.md`](userdoc/README.md) — the modding hub (plugins, load order, effects, schedules, UI, tools) |
| **Where the project is** | [`docs/MEADOWS-PLAN.md`](docs/MEADOWS-PLAN.md) — the demo roadmap & acted decisions |
| Architecture contract | [`docs/HORIZONTAL-PASS.md`](docs/HORIZONTAL-PASS.md) — every system's seams & how to fill them |
| 3D renderer | [`docs/RENDERING.md`](docs/RENDERING.md) — the graphics stack: architecture, lighting/GI, performance, lessons, roadmap (brick journals in docs/archive/) |
| Engine/agent guidance | [`CLAUDE.md`](CLAUDE.md) — invariants, phase history, per-phase journals in `docs/PHASE-*.md` |
| Stats design | [`docs/STATS.md`](docs/STATS.md) — the character-stats reference |

## Building

Requirements: CMake ≥ 3.25, a C++20 compiler (MSVC 2022 / clang / gcc).
Dependencies are pinned and fetched by CPM at configure time — no
per-machine setup.

```
cmake -S . -B build
cmake --build build
./build/tests/Debug/meadows-tests     # headless test suite
./build/game/Debug/true-adventurer    # the demo scenes (menu top-left)
```

Linux (Fedora/Debian) and Windows are both first-class targets (§3.1 of
CLAUDE.md).

## Demo scenes (`true-adventurer`)

`Landscape (3D)` — the BotW-like landscape (fly: WASD + LMB mouselook) ·
`Combat arena` — the 2D combat loop (typed damage, statuses, hit cues) ·
`Game DB (editor)` — browse/edit every record, manage load order, dev
console · `UI (RmlUi)` — the game-UI seam · plus the earlier system demos.
