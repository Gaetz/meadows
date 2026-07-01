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

**The lasting asset is the simulation layer, not the renderer.** The gameplay
model (flecs ECS, GAS, data-model, modding layer, Lua scripting) must compile
and run without any renderer — the headless test suite is the proof. The
graphical frontend is a replaceable shell. The 2D OpenGL renderer is a working
prototype; **Godot (via GDExtension)** is the target long-term frontend,
validated by a 2D→Godot port test (Phase 8.5) before committing to the 3D phase.
See `docs/SIMULATION-AND-PRESENTATION.md`.

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

10. **Simulation runs headless.** `gameplay/`, `world/`, `data/`, `script/`
    have zero dependency on `engine/platform/`, `engine/rhi/`, or
    `engine/render/`. They compile and run without SDL, OpenGL, or any renderer.
    The test suite (181+ headless tests) enforces this. Any violation is a
    coupling that must be removed before the Phase 8.5 Godot port.

---

## 3. Tech stack

Use these unless there is a concrete reason not to (then ask first).

| Concern            | Choice                                  | Notes |
|--------------------|-----------------------------------------|-------|
| Build              | CMake + CPM.cmake (FetchContent)         | Deps pinned in CMake, fetched at build time, identical on Fedora/Debian/Windows, no per-machine bootstrap. vcpkg (manifest) / Conan also work cross-platform. |
| Window / input     | SDL3                                     | Gamepad, events, optional audio. |
| Math               | GLM                                      | Don't reinvent. |
| GPU                | OpenGL 4.6 (DSA, bindless) behind RHI    | 2D prototype renderer. Long-term frontend = **Godot via GDExtension** (validated by Phase 8.5). Vulkan backend not planned unless the custom-renderer path is chosen post-validation. |
| Mesh / model       | glTF 2.0 via cgltf                       | Skinning, anims, PBR built in. |
| Textures           | stb_image + KTX2 (Basis Universal)       | Compressed for runtime. |
| ECS                | flecs (pinned v4.1.5)                     | Runtime only (lib `meadows-ecs`); data model stays flecs-free. Used directly in systems, not behind a façade. Our reflection — not flecs meta — is the keystone (§2.3). See `docs/PHASE-2.md`. |
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

### 5.1 Synthesis / conflict-arbitration patches (design decided, build later)

Skyrim needs Wrye Bash / Synthesis because its overrides are whole-record;
our field-level layering removes that class of conflict natively. The only
real conflict left is two plugins writing the *same field* when the user
wants A's value for one field and B's for another — unexpressible by load
order alone. The decided design:

- **A synthesis patch is an ordinary plugin loaded last.** No special
  format, no new engine mechanism. A tool reads the resolver's conflict
  report, lets the user pick a winner (or custom value) per conflicted
  field, and emits a normal plugin (via the existing TomlWriter) carrying
  only the arbitrated fields, with the arbitrated mods in `dependencies`.
- **Provenance for regeneration**: the generated plugin records where each
  choice came from (comment/metadata, e.g. `# from: sharper-swords`) so the
  tool can re-apply choices after a modlist update and flag stale picks.
- **Programmatic patchers** ("all weapons 10% lighter, mods included") are
  functions `resolved FormDatabase -> patch plugin`; scriptable in Lua once
  Phase 4 lands. Output is again an ordinary plugin.
- Prerequisite when building the tool: extend `FieldConflict` to carry each
  writer's *value*, not just its name (small resolver change).
- Invariant to protect: never add a parallel resolution mechanism — the
  answer to "force this value" is always "one more layer" (§2.4).

Target: Phase 14 editor conflict view, or a CLI subcommand if the need bites
earlier.

---

## 6. Gameplay systems — simplified GAS

The stats/combat/magic/buff layer is a **simplified Gameplay Ability System**,
inspired by Unreal's GAS but stripped to single-player needs. It is a
principled version of what Skyrim already does (actor values, magic effects,
spells, perks). **Single-player only: no network prediction or replication** —
that removes most of GAS's complexity. Everything below is **data, therefore
moddable**: it layers through the §5 patch system like any other Form.

> The concrete character-stats design built on this GAS (attributes, the hidden
> **Resonance** stat + Harmony, derived secondary stats, typed damage, posture,
> survival, injuries) is specified in **`docs/STATS.md`** — the canonical
> reference. Read it before touching `gameplay/stats/`.

- **Attributes** — named float values with a `BaseValue` / `CurrentValue` split
  (e.g. Health, MaxHealth, Energy, Essence, ArmorRating, CarryWeight). Grouped
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

> **Modder reference:** `docs/MODDING-EFFECTS.md` — complete `EffectForm` /
> `AbilityForm` field guide, attribute list, TOML plugin format, and recipes for
> all use cases (damage, buffs, DoT, drugs, afflictions, buildup, abilities). No C++
> required.

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

> **Conditional:** The 3D rendering roadmap below describes the **custom renderer
> path**. Whether it is taken depends on the Phase 8.5 Godot port validation.
> If Godot is confirmed as the 3D frontend, Phases 11–14 are superseded by a
> Godot-based 3D integration and the list below becomes a long-term / custom-
> runtime-only reference. See `docs/SIMULATION-AND-PRESENTATION.md`.

- **2D phase:** instanced sprite/quad renderer, top-down camera, tilemap or
  free placement. Enough to exercise world, streaming, combat, AI, UI.
- **3D phase (BotW-like look — custom renderer path), implement in this rough order:**
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
- **Phase 5 — Multithreading architecture:** the first point where async is
  truly forced (it precedes streaming). Define the thread model and the
  **JobSystem-vs-flecs-scheduler boundary**, plus the async asset-residency
  path (§7: background decode → main-thread GPU upload, never block spawn or the
  frame). Decided with the dev (full rationale below); load-bearing:
  - **Decouple ≠ thread.** The sim produces a **render snapshot** each frame
    (an *extract* phase): a self-owning POD packet of draw data (resolved
    texture handles, no live pointers into the ECS world or asset internals).
    The renderer reads **only** this packet — it has no access to the `World`.
    `submitScene`'s sprite list already *is* this packet; formalize it as the
    contractual boundary. **Strict** decoupling: the packet is passed by value,
    so where its consumer runs (same thread, a render thread, a pipeline of
    frames) is a **scheduling policy, not an architectural invariant**.
  - **Same thread is the default, not a law.** Keeping sim+render on the main
    thread stays the default (a render thread buys ~nothing for an instanced 2D
    renderer and would only cost state double-buffering + a frame of latency).
    The strict snapshot is what keeps a render thread *cheap to add later*.
  - **GL caveat.** The GL context is thread-affine: *if* a render thread is
    ever introduced it becomes *the* GL thread, and asset uploads migrate to it
    (uploads are GL calls). Keep uploads **behind the RHI** so this stays a
    backend detail (and so a Vulkan backend can use parallel command recording
    + a transfer queue instead — the render-thread model is a GL-era optim).
  - **JobSystem owns task parallelism** (I/O, asset decode, per-cell
    resolution); **flecs systems stay single-threaded** until profiling
    justifies parallel systems (likely Phase 12/13). These are **orthogonal
    axes** — heterogeneous-task parallelism (JobSystem) vs. intra-system
    parallelism over entities (flecs scheduler); do not conflate them.
  - **Main owns the ECS world and the GPU; a worker touches neither.** Workers
    produce results into a **non-blocking completion queue** (MPSC, drained at a
    fixed point each frame) — never `JobSystem::wait()` on the frame thread.
    The main thread applies results in a **deterministic order** (e.g. sort by
    GUID) so completion order never perturbs the engine RNG or saves (§8).
  - Phase-5 scope = the **seam** only: the strict render snapshot, the
    non-blocking completion queue, and one asset-residency path (decode in a
    worker → upload on main → flip handle to resident). **No render thread.**
    Real cell streaming stays Phase 8. Everything is single-threaded until this
    phase; gameplay randomness stays on the engine RNG (§8) so saves/replays
    remain reproducible.
- **Phase 6 — Character stats: vertical slice (2D):** the load-bearing core of
  the stats design (`docs/STATS.md`) — 9 attributes → 3 primary stats
  (Health/Energy/Essence), **Resonance** (hidden signed stat, Onyx/Amber/Garnet
  spheres) + **Harmony** cascade, derived secondary stats via **C++ calculators +
  data constants** (§2.7/§6) with per-stat **override/offset** for non-humanoids,
  a typed-damage → armor/resistance → health+posture pipeline with posture/stagger,
  a minimal game clock (timescale), one survival→resonance loop, and an ImGui
  `StatsScene` to drive it. The new derived-attribute machinery generalizes the
  GAS (multiple AttributeSets; `recomputeCurrent` runs a derived pass). Tested in
  2D before streaming/3D.
- **Phase 7 — Character stats: permanent-status & resonance mechanics (2D):**
  **value-first** scope (decided with the dev) — the foundations + the novel /
  risky systems, not the whole `docs/STATS.md` tail. **Foundations:** a seeded
  engine RNG (`core::Rng`, §8), a `StatsTuningForm` (Phase-6 constants → moddable
  data, §5), a stat-bearing **item/equipment** model (weapons with typed attack +
  scaling, armor/clothing per slot, consumables), and **rest/sleep** recovery.
  **Novel mechanics:** status **buildup** (poison/bleed/…), **body-part injuries**
  + **diseases/psychoses** (the permanent-status system, gated by resonance-
  resistance, rolled via RNG), and **drugs + harmony break**. All bolt onto the
  Phase 6 machinery. **Deferred to a later stats pass:** the full secondary stat
  list, the full combat-state machine (critical-weakness/shaken/dismember — needs
  a real-time combat loop), temperature/clothing survival, social + faction-by-
  location reputation, encumbrance/movement, the erudition curve. Still 2D. Brick
  journal: `docs/PHASE-7.md`; design: `docs/STATS.md`.
- **Phase 8 — Combat 2D dynamique :** scène jouable style Zelda 2D — joueur
  contrôlable avec attaque/esquive, ennemis actifs (IA chase + attaque au
  contact), boucle de combat réelle (hits mutuels, posture, stagger, mort).
  PNJ repos (aubergiste : récupération santé/énergie/essence) et marchand
  (achat/vente d'équipement). Objectif : exercer tous les systèmes de stats
  (Phases 6-7) en situation dynamique réelle, pas seulement via l'ImGui.
  Toujours 2D. Brick journal : `docs/PHASE-8.md`.
- **Phase 8.5 — Godot port validation:** Port the 2D combat prototype to Godot
  via GDExtension (§2.10). Deliverables: a C++ GDExtension bridge exposing the
  flecs World to Godot; a generic `EntityView` node (`Node2D`) configured by
  projection from ECS state; input → `push_intent` flow; the headless sim
  running unchanged. **Goal:** validate that the sim/presentation boundary is
  real and coupling is zero. If easy → Godot becomes the 3D frontend (Phases
  11+ reframed). If coupling is found → fix it here, before the 3D investment.
  Ref: `docs/SIMULATION-AND-PRESENTATION.md`.
- **Phase 9 — Stats avancées (passe complète) :** tout ce qui a été différé
  de Phase 7 et qui nécessite la boucle de combat Phase 8. **Machine d'état de
  combat :** shaken (rupture posture rapide), critical weakness (posture=0 →
  5s, ouvre les critiques), démembrement, saignée/bleed-out, mort → inconscient
  → stabilisation. **Stats offensives dérivées :** attack (5 + force), crit
  damage (1.5 + dex/2), attack speed (95% + 1%/célérité), armor/resistance
  penetration, status damage scaling. **Stats sociales :** beauté, prestige,
  menace, suspicion, discrétion, érudition, réputation de faction. **Stats
  utilitaires :** encombrement (50 + force·10 + constitution·2), saut, escalade,
  nage, apnée. **Blessures avancées :** plaies ouvertes/infectées, traitement
  par items (compresses, herbes, bandages, splintes). Brick journal :
  `docs/PHASE-9.md`; design : `docs/STATS.md`.
- **Phase 10 — Streaming & persistence:** cell grid, async load/unload, LOD,
  interior/exterior transitions, **save = runtime patch layer** reusing the
  Phase-1 resolver.
- **Phase 11 — 3D frontend (conditional on Phase 8.5 outcome):**
  *Godot path (expected):* load glTF assets via Godot, 3D scene from ECS state
  via the GDExtension bridge, BotW-like look via Godot's renderer; gameplay
  untouched.
  *Custom renderer path (if Godot rejected):* glTF meshes/materials/skinning,
  3D camera, scene→3D, low-poly pipeline behind the existing RHI.
- **Phase 12–14 — Lighting / physics / audio / editor (conditional):**
  *Godot path:* use Godot's built-in rendering features (lighting, shadows,
  navmesh, audio, animation); build the in-engine editor as a Godot UI calling
  back into the C++ data layer via GDExtension.
  *Custom renderer path:* §7 roadmap (clustered forward, CSM, Jolt, Recast,
  miniaudio, Vulkan). Decision deferred to Phase 8.5 outcome.

> **CURRENT PHASE: 8** — update this line as work progresses.
> Phase 8: Combat 2D dynamique — scène jouable Zelda-like, boucle de combat
> réelle (joueur + ennemis actifs), PNJ repos/marchand. Brick journal :
> `docs/PHASE-8.md`. Design stats : `docs/STATS.md`.
>
> Phase 0 done (2026-06-12): CMake+CPM, SDL3 window/input, logging, job system,
> RHI interface + GL 4.6 backend, instanced sprite renderer, ImGui.
>
> Phase 1 done (2026-06-12): reflection, Forms, field-level plugin resolver,
> text↔binary cooker, GUID asset DB. **Full brick-by-brick detail and the
> non-obvious decisions live in `docs/PHASE-1.md`** — read it before touching
> the data/modding model.
>
> **Phase 2 done** (2026-06-13, 54 tests green). ECS + world model. Full rationale
> + brick journal in `docs/PHASE-2.md` — **read it before touching ecs/world**.
> Key: flecs confined to `meadows-ecs` (meadows + meadows-data stay flecs-free);
> Reference = a `ReferenceForm` (place/move/disable = field patches); cells =
> ephemeral flecs entities, never persisted; GameplayTags ≠ flecs tags.
>
> **Phase 3 done** (2026-06-14, 94 tests / 716 assertions green). Gameplay 2D +
> GAS core. Full rationale + brick journal in `docs/PHASE-3.md` — **read it
> before touching gameplay/GAS**. Key: GAS core inseparable (Attributes + Effects
> + Tags + Abilities built together); damage = transient meta-attribute +
> PostExecute → Health; effect pipeline is flat linear (not a node-graph); lib
> `meadows-gameplay`.
>
> **Phase 4 done** (2026-06-14, 108 tests / 783 assertions green). Scripting,
> abilities, conditions, quests, dialogue. Full rationale + brick journal in
> `docs/PHASE-4.md`. Key: ONE shared Lua VM, scripts stateless, `self` = entity
> handle; latent `wait()` = coroutines + central scheduler; conditions =
> structured clauses + Lua escape; quests/dialogue = decomposed records linked by
> id; libs `meadows-script` + `meadows-narrative`.
>
> **Phase 5 done** (2026-06-15). Multithreading seam. Full brick journal + crash
> postmortem in `docs/PHASE-5.md` — **read it before touching `game/SceneSubmit`,
> `engine/core/`, or `game/TextureCache`**. Key: strict render snapshot
> (`extractScene` → `RenderSnapshot` → `submitSnapshot`; renderer has NO `World`
> access); non-blocking MPSC `ConcurrentQueue`; async texture residency + loading
> gate. **Build lesson: after a shared-type layout change, do a clean rebuild**
> (ninja header-dep miss → stale-obj heap corruption).
>
> **Phase 6 done** (2026-06-15). Character stats vertical slice. Full brick journal
> in `docs/PHASE-6.md`; design in `docs/STATS.md`. Key: derived-attribute two-pass
> `recomputeCurrent` (`DerivedStats`); primary maxima from BASE (Resonance doesn't
> move the max), secondary stats from CURRENT; Resonance + Harmony; typed-damage
> pipeline; `f64` added to reflection (appended last — binary ordinals stable).
>
> **Phase 7 done** (2026-06-17 → 176 tests / 5087 assertions green after post-phase).
> Permanent-status & resonance mechanics. Brick journal in `docs/PHASE-7.md`; design
> in `docs/STATS.md`. Deliverables: seeded engine RNG (`core::Rng`, §8),
> `StatsTuningForm` (Phase-6 constants → moddable data, §5), stat-bearing
> items/equipment (weapons with typed attack + scaling, armor/clothing per slot,
> consumables), rest/sleep recovery, status buildup (poison/bleed/…), body-part
> injuries + diseases/psychoses (permanent-status system, RNG-rolled, gated by
> resonance-resistance), drugs + harmony break. Post-phase additions: `healthRegen`
> (0.0002·grace/s), `essenceRegen` (0.005·insight/s), `resistCold`, `State.Paralyzed`
> (glaciation ≠ stagger), `DamageType::Cold`; `CharacterTick` extracted from scene;
> `AfflictionForm` + `DrugForm` supprimés — tout passe par `EffectForm`/`applyEffect`
> (GAS unification); `ResonanceDecays` pour le fondu post-expiration. Référence
> moddeur : `docs/MODDING-EFFECTS.md`.

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
