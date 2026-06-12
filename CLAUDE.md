# CLAUDE.md — Skyrim-like Engine Prototype

> Guidance for any Claude Code instance working in this repository.
> Read this fully before writing code. When a decision conflicts with this
> file, this file wins unless the human says otherwise. When something here is
> ambiguous for the task at hand, ask before inventing an architecture.

---

## 1. What this project is

A **custom game engine prototype** to validate the systems of a Skyrim-like,
open-world, moddable RPG. The author is a solo developer (professional C++ /
graphics programmer). The engine is built in **C++ with a graphics-API
abstraction (RHI)**, OpenGL first, Vulkan later.

The project is built **2D top-down first**, then transitions to **3D low-poly**
with a stylized "Breath of the Wild"-like look. The game *logic* must run
unchanged across that transition — the renderer is decoupled from gameplay.

This is a prototype to test **game-design concepts**, not a shipping engine.
The biggest risk is in the gameplay/world/modding systems, **not** in the
renderer. Bias effort accordingly: do **not** gold-plate the rendering layer
while core systems are missing.

---

## 2. Non-negotiable architecture invariants

These are the load-bearing decisions. Do not break them without explicit
approval.

1. **Renderer is behind an RHI.** No gameplay, world, or UI code calls a
   graphics API directly. Everything goes through the `rhi::` interface.
   Backends live in `rhi/backends/`. Only OpenGL 4.6 (DSA + bindless) is
   implemented for now; the interface is designed around *explicit* concepts
   (command buffers, pipeline state objects, bind groups / descriptor sets,
   render passes) so a Vulkan backend can be added without touching callers.
   Backend selection is a **runtime** decision (virtual `rhi::Device`
   interface + factory, chosen at startup from config/availability) — distinct
   from OS selection, which is compile-time (§3.1).
   **Do not write a Vulkan backend yet.**

2. **Data model = Forms vs References.**
   - A **Form** is a *definition* (what an "iron sword" IS). Forms are data,
     reflected, serializable, and moddable.
   - A **Reference** is a *runtime instance* of a Form placed in the world,
     with instance-level overrides (transform, enabled state, owner, count,
     enchantment, etc.). References are ECS entities.
   Gameplay creates/queries References; it never mutates Forms at runtime.

3. **Reflection is the keystone.** Every Form type and component registers its
   fields through the reflection system. This single mechanism powers:
   serialization, the modding patch system, save games, and editor property
   panels. Add fields via reflection macros/registration — never with ad-hoc
   per-type serialization code.

4. **One layering mechanism for mods AND saves.** Base game data, every mod,
   and the save file are all **layers of field-level patches** applied in a
   deterministic order (last-writer-wins per field). See §5. Do not build a
   separate save system; a save is a runtime patch layer.

5. **Stable identity by GUID, not by load order.** Plugins and assets are
   identified by stable GUIDs. Runtime uses compact handles resolved at load.
   Never encode load-order position into a persistent ID (this is the Skyrim
   FormID fragility we are deliberately avoiding).

6. **Gameplay is renderer-, dimension-, and platform-agnostic.** Game systems
   must not assume 2D or 3D, OpenGL or Vulkan, or a specific OS. The 2D→3D move
   is a renderer/asset concern, not a gameplay rewrite.

7. **Entity instantiation = per-category C++ spawner.** A resolved Form +
   Reference becomes an ECS entity via a C++ spawner keyed on form *category*
   (actor, item, container, static, door…), kept in one place, wiring the
   mandatory components and applying field values **through reflection**.
   Data-driven behavioral richness comes from the simplified GAS layer
   (§6: attributes, effects, abilities, tags) and from scripts — **not** from
   modders composing arbitrary new component types (this matches Skyrim: fixed
   core record types, extended by effects and scripts). The spawner may later
   attach *optional* data-declared components as a bounded extension point; keep
   it isolated so promotion is a local change.

8. **Per-entity runtime state lives in reflected C++ components, not loose Lua
   tables.** Scripts are shared, stateless modules; `self` is an entity handle.
   Persistent script state goes in a reflected component (e.g. `ScriptVars`) so
   it serializes/diffs through the §5 patch layer for free. A non-persistent Lua
   scratch table is allowed for transient state only. Latent waits use ability
   tasks / a coroutine pool keyed by entity, not per-entity Lua environments. A
   Lua metatable proxy over the component may provide `self.x` ergonomics.

9. **Attributes mutate only through GameplayEffects.** Nothing sets an
   attribute's value directly — all changes flow through the effect pipeline
   (§6). This keeps mutation auditable, stackable, and reversible, and makes
   saves cheap (persist `BaseValue`s + active durational effects; derive
   `CurrentValue`s on load).

---

## 3. Tech stack

Use these unless there is a concrete reason not to (then ask first).

| Concern            | Choice                                  | Notes |
|--------------------|-----------------------------------------|-------|
| Build              | CMake + CPM.cmake (FetchContent)         | Deps pinned in CMake, fetched at build time, identical on Fedora/Debian/Windows, no per-machine bootstrap. vcpkg (manifest) / Conan also work cross-platform. |
| Window / input     | SDL3                                     | Gamepad, events, optional audio. |
| Math               | GLM                                      | Don't reinvent. |
| GPU                | OpenGL 4.6 (DSA, bindless) behind RHI    | Vulkan later, same interface. |
| Mesh / model       | glTF 2.0 via cgltf                       | Skinning, anims, PBR built in. |
| Textures           | stb_image + KTX2 (Basis Universal)       | Compressed for runtime. |
| Physics            | Jolt                                     | 3D phase. 2D phase uses simple custom collision. |
| Navmesh (3D)       | Recast / Detour                          | 2D phase uses grid A*. |
| Scripting          | Lua via sol2                             | AngelScript / WASM as future options. |
| Dev UI / editor    | Dear ImGui                               | |
| Game UI            | RmlUi (or custom)                        | Defer until needed. |
| Audio              | miniaudio                                | |
| Logging            | spdlog                                   | |
| Reflection         | Custom (macros/codegen)                  | The keystone — see §2.3. |

**On-disk formats:** authoring/mod records in a **human-readable text format**
(TOML or a small custom DSL) so mods diff cleanly under version control (this
serves the "git-like" intent); a **binary cooked** format for runtime load.
Provide a text↔binary tool.

### 3.1 Platform abstraction & build portability

Target **Linux (Fedora/Debian) and Windows from day one**; both must stay
buildable at all times. macOS optional later.

- **Dependencies:** CPM.cmake (FetchContent-based). Declare and version-pin
  each dep in CMake; it is fetched/built as part of the build, identically on
  every OS, with no separate tool to bootstrap per machine. (vcpkg manifest
  mode and Conan are also cross-platform; CPM is chosen for least friction.)
- **Platform layer lives only in `engine/platform/`.** Pattern: one clean
  `.hpp` interface per concern, one `.cpp` per platform **selected by CMake**
  (via `target_sources` per platform). Use subdirectories: `common/`
  (cross-platform impl), `posix/` (shared Linux/macOS), `linux/`, `win32/`.
  No other part of the engine may contain per-OS source files.
- **Headers stay platform-clean.** Never `#include <windows.h>`, `<X11/...>`,
  or Wayland headers in any `.hpp`. Hide native types (HWND, surfaces, fds)
  behind a **pimpl** or an opaque handle, so the interface is identical on all
  platforms.
- **SDL3 IS the platform layer** for window, input, gamepad, timers,
  clipboard, DPI. So most of `platform/` is a single cross-platform `.cpp`.
  Reserve the per-OS `.cpp` pattern for what SDL does not cover: known-folder
  paths, native file dialogs, crash/minidump handling, high-precision timing
  quirks. Do **not** reimplement what SDL provides. Keep the interface clean so
  SDL could be swapped out later.
- **Selection model — keep these straight:**
  - **OS / platform → compile-time.** One binary per OS; CMake picks the `.cpp`.
  - **RHI backend (GL/Vulkan) → runtime.** One binary holds both; a virtual
    `rhi::Device` interface + factory chooses at startup. This is what
    "switch between OpenGL and Vulkan" requires — never make it compile-time.

---

## 4. Directory layout

```
/engine
  /platform     # cross-platform layer (mostly SDL3). ONE .hpp per concern,
    /common     #   per-OS .cpp selected by CMake. common/ = cross-platform impl
    /posix      #   shared Linux/macOS impl
    /linux      #   Linux-only impl (only what SDL doesn't cover)
    /win32      #   Windows-only impl (only what SDL doesn't cover)
  /core         # logging, asserts, allocators, job system, math aliases
  /reflect      # reflection system (field registration, type info)
  /rhi          # graphics abstraction (interface)
    /backends
      /gl        # OpenGL 4.6 backend (only backend for now)
  /render       # renderer built on RHI: 2d sprite renderer, later 3d pipeline
  /assets       # asset DB (GUID), loaders (gltf, ktx2), cooked cache
  /ecs          # entity-component-system
/data
  /forms        # Form/record system, schema, reflection-driven serialize
  /plugins      # plugin/mod loader, layered field-level patch resolution
  /save         # save = runtime patch layer (reuses /data/plugins machinery)
/world
  /worldspace   # worldspace -> cell grid -> references hierarchy
  /streaming    # async cell load/unload, LOD, transitions, persistence
  /scene        # scene representation shared by 2D and 3D
/gameplay
  /ability      # simplified GAS: attributes, attribute sets, gameplay effects,
                #   abilities, gameplay tags, ability-system component
  /actors       # actor templates, leveling, perks (perks = passive effects)
  /combat       # expressed via abilities + effects (damage = effect on Health)
  /inventory    # items, containers, equipment, ownership
  /ai           # navmesh, pathfinding, AI packages, perception
  /magic        # spells = abilities; magic effects = gameplay effects
  /faction      # faction relations table; membership via gameplay tags
/script         # Lua VM integration, event dispatch, bindings
/quest          # quest state machines, aliases, dialogue, condition evaluator
/ui             # imgui dev UI; game UI later
/editor         # in-engine ImGui editor (forms, cells, refs, quests)
/tools          # text<->binary cooker, asset importers, mod validator
/game           # the actual prototype game data + entry point
/tests
```

Keep `gameplay/`, `world/`, `quest/`, `script/` free of any `rhi/`,
`render/`, or backend includes.

---

## 5. The data & modding model (read carefully — easy to get wrong)

This is the heart of moddability. Get it right early.

- A **plugin** (base game, mod, or save) is an ordered set of **records**.
- A **record** either **creates** a new Form (new GUID) or **patches** an
  existing one. Patches are **field-level**: a record carries only the fields
  it changes.
- **Resolution**: load plugins in their declared order; for each Form, apply
  patches in order; **last writer wins per field**. Two mods editing different
  fields of the same NPC therefore do **not** conflict. Conflicts (same field,
  multiple writers) are detectable and reportable (this is what a future
  conflict-resolution tool / editor surfaces).
- **Assets** layer the same way: a virtual file system where later plugins'
  loose/archived files override earlier ones, by GUID.
- **References** (placed instances) are records too: a plugin can add, move,
  disable, or patch a reference's instance fields.
- **Save games** are a runtime patch layer applied *after* all plugins:
  picked-up items, moved/killed references, quest stages, journal, actor state.
  Loading a save = re-resolving with that final layer on top. This is why
  there is **no separate save subsystem** — reuse the plugin resolver.

Do **not** implement record-level (whole-object) overrides as the primary
mechanism. Field-level patching is the design.

---

## 6. Gameplay systems — simplified GAS

The stats/combat/magic/buff layer is a **simplified Gameplay Ability System**,
inspired by Unreal's GAS but stripped to single-player needs. It is a
principled version of what Skyrim already does (actor values, magic effects,
spells, perks). **Single-player only: no network prediction or replication** —
that removes most of GAS's complexity. Everything below is **data, therefore
moddable**: it layers through the §5 patch system like any other Form.

- **Attributes** — named float values with a `BaseValue` / `CurrentValue` split
  (e.g. Health, MaxHealth, Stamina, Magicka, ArmorRating, CarryWeight). Grouped
  into **AttributeSets** (reflected components). Clamping and derived values live
  in change hooks (pre/post). `CurrentValue` = `BaseValue` + active modifiers.
- **GameplayEffects (GE)** — the **only** way to change an attribute (§2.9).
  Declarative, data-driven modifiers (add / multiply / override) applied as
  instant (permanent `BaseValue` change), duration, infinite, or periodic.
  Effects carry/grant **tags** and can grant abilities. Buffs, debuffs, damage,
  healing, regen, poisons, enchantments are all effects.
- **GameplayAbilities (GA)** — activatable actions (attack, cast, sprint, power
  attack). Costs and cooldowns are themselves effects. Activation/blocking
  conditions use **tags**. Latent/async steps use ability tasks (this is where
  `wait(…)` lives — not per-entity Lua coroutines). Authored as data; custom
  logic in Lua where needed.
- **GameplayTags** — hierarchical string tags (`Status.Burning`,
  `State.InCombat`, `Faction.CityGuard`). One vocabulary for state, effect
  immunities, ability requirements, and the **condition evaluator** used across
  dialogue, quests, AI, and perks.
- **AbilitySystem component** — one per actor (player and NPCs alike), owning
  its AttributeSets, active effects, granted abilities, and owned tags.

**Moddability.** Attributes, effects, abilities, and tags are Forms. A mod adds
a new attribute set, a new effect, a new ability, retunes modifier magnitudes,
or adds tags — purely in data, layered by load order. This data-driven richness
is what lets entity *composition* stay a simple C++ spawner (§2.7) instead of a
data-defined component system.

**Persistence.** Per §2.9, persist `BaseValue`s and the set of active durational
effects; recompute `CurrentValue`s on load. Instant effects are already baked
into `BaseValue`. Keeps saves small and the layering invariant (§2.4) intact.

**Keep it simple first.** Minimal modifier pipeline (add/multiply/override +
clamp). Add aggregators, attribute capture, or custom execution calculations
only when a concrete case needs them. Do **not** port Unreal's prediction,
replication, or full C++ ability-class hierarchy.

### 6.1 Factions

Faction **membership** is expressed as **gameplay tags** on the entity
(`Faction.CityGuard`), reusing the tag system and condition evaluator.
Faction-to-faction **relations** (ally / neutral / enemy + reaction values) live
in a single reflected, moddable **relations table** (data, layered like any
Form). Add a thin `Faction` component only if per-entity faction state beyond
membership is needed (individual reputation, crime/bounty). Do not build a
bespoke faction subsystem parallel to tags.

---

## 7. Rendering notes

- **2D phase:** instanced sprite/quad renderer, top-down camera, tilemap or
  free placement. Enough to exercise world, streaming, combat, AI, UI.
- **3D phase (BotW-like look), implement in this rough order:**
  1. Clustered forward (forward+) rendering.
  2. Cascaded shadow maps for the sun (soft).
  3. Sky + atmosphere (analytic sky model or gradient) driven by time-of-day.
  4. Hemisphere/SH ambient + SSAO.
  5. Height/volumetric fog.
  6. Bloom + filmic tonemap + color-grade LUT.
  7. Time-of-day + weather system feeding sun/sky/fog (and AI schedules).
- The "BotW look" is mostly **art direction + soft GI feel + atmosphere**, not
  exotic tech. Flat-ish albedo, lighting does the work. Keep it stylized.
- **Asset residency at spawn (both 2D and 3D).** Render components
  (`SpriteRender`, later mesh components) hold an **asset handle**, never pixels
  or vertex data. Streaming may spawn an entity *before* its asset is
  GPU-resident, because asset resolution (VFS → load → upload via RHI) is async.
  Therefore: a missing-but-pending asset renders a **placeholder** and the
  upload happens on a background load → main-thread-upload path. **Never block
  entity spawn or the frame on an asset load.** The handle transparently starts
  pointing at the real GPU resource once resident.

---

## 8. Coding conventions

- **C++20.** RAII everywhere; no raw owning pointers. `std::unique_ptr` for
  ownership, raw pointers/refs for non-owning access, handles for resources
  that cross subsystem boundaries (GPU resources, forms, refs).
- **No exceptions in hot paths**; use `std::expected`/result types or status
  codes for recoverable errors. `assert`/`ENGINE_ASSERT` for invariants.
- **Data-oriented** in the ECS hot loops; OOP is fine for tooling/editor.
- Subsystems expose narrow interfaces; no global mutable singletons except a
  small explicit `Engine` context passed by reference.
- Names in **English** (identifiers, comments, log messages), regardless of
  the human's working language.
- Every new Form type / component **registers its reflection** in the same
  file it's declared.
- Determinism: route all gameplay randomness through the engine RNG (seeded,
  serialized) so saves and replays are reproducible.
- Tests for the data model, plugin resolver, and save layering are
  **mandatory** — these are the systems where silent bugs are most expensive.

---

## 9. Implementation roadmap (phased)

Work top-to-bottom. **Mark the current phase** below and keep it updated.
Do not jump ahead to 3D rendering before the 2D-phase systems work.

- **Phase 0 — Foundations:** CMake+CPM.cmake, platform (SDL3 window/input),
  core (logging, job system, math), RHI interface + GL backend, 2D sprite
  renderer, ImGui integration.
- **Phase 1 — Data model:** reflection system, Form/record types, GUID asset
  DB, plugin loader + **field-level patch resolution**, text↔binary cooker.
  *(This phase de-risks the whole project. Do it well.)*
- **Phase 2 — ECS + world model:** entities/components, worldspace→cell→
  reference hierarchy, shared scene representation, populate a 2D world.
- **Phase 3 — Gameplay in 2D:** player controller, 2D collision/triggers,
  inventory/items, **simplified GAS (attributes + gameplay effects + tags)**,
  combat expressed as abilities/effects, basic AI (grid A* + simple packages),
  factions (tags + relations table), perception.
- **Phase 4 — Scripting, abilities & quests:** Lua (sol2) VM, event dispatch,
  scripts on forms/refs, **gameplay abilities**, tag-based **condition
  evaluator**, quest state machines + aliases, dialogue trees.
- **Phase 5 — Streaming & persistence:** cell grid, async load/unload, LOD,
  interior/exterior transitions, **save = runtime patch layer** reusing the
  Phase-1 resolver.
- **Phase 6 — 3D transition:** glTF meshes/materials/skinning, 3D camera,
  scene→3D, low-poly pipeline, keep gameplay untouched.
- **Phase 7 — BotW lighting:** the §7 list.
- **Phase 8 — Physics/anim/audio/nav:** Jolt, blend trees + foot IK,
  miniaudio, Recast/Detour 3D navmesh.
- **Phase 9 — Editor & Vulkan:** in-engine ImGui editor (forms, cells, refs,
  quests, conflict view); Vulkan RHI backend **only when a real need exists**.

> **CURRENT PHASE: 1** — update this line as work progresses.
> Phase 0 done (2026-06-12): CMake+CPM, SDL3 window/input, logging, job
> system, RHI interface + GL 4.6 backend, instanced sprite renderer, ImGui.

---

## 10. When in doubt

- If a task touches the **data model, plugin resolution, or saves**, re-read §5
  and prefer asking over guessing — these invariants are expensive to change
  later.
- If a feature can be tested in the **2D phase**, build it there first.
- If you're about to make the **renderer fancier** while core systems are
  incomplete, stop and reprioritize.
- Never set an attribute directly — apply a **GameplayEffect** (§2.9, §6).
- Prefer the **simplest thing that exercises the concept**; this is a prototype
  for a solo developer, not a production engine.
