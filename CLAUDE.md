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
abstraction (RHI)** — OpenGL 4.6 today, Vulkan possible later.

The project was built **2D top-down first** (phases 0-8, all core systems)
and is now moving to **3D low-poly** with a stylized "Breath of the
Wild"-like look. The game *logic* runs unchanged across that transition —
the renderer is decoupled from gameplay.

**The lasting asset is the simulation layer, not the renderer.** The gameplay
model (flecs ECS, GAS, data-model, modding layer, Lua scripting) must compile
and run without any renderer — the headless test suite is the proof. The
graphical frontend is a replaceable shell. **Decision (2026-07-05): the
gameplay demo is built in Meadows itself** (the custom GL 4.6 3D renderer —
`docs/RENDERING.md` — proved out); the engine/tool roadmap for that demo
lives in **`docs/MEADOWS-PLAN.md`**. Godot (via GDExtension) remains a
**post-demo option**, its validation (Phase 8.5) postponed — which is exactly
why the sim/presentation seam stays a protected invariant.
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
   Decision (2026-07-12): the **macOS path is a Vulkan backend + MoltenVK**
   as a dedicated **post-demo** chantier — no GL 4.1 down-port of the 3D
   renderer (Apple GL is deprecated and lacks compute/SSBO/bindless), no
   native Metal backend (SPIRV-Cross can derive one later if ever needed).
   Until then: new RHI-facing code stays behind the capability flags.

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
   **Execution calculations are part of the pipeline, not an exception.** Some
   mutations can't be expressed as declarative add/multiply/override modifiers —
   typed-damage mitigation (armor/resist/crit → health), rate-driven regen with
   a dynamic captured magnitude and tag gates, and periodic status DoT / lethal
   zeroing. These are *execution calculations*: C++ in the damage path or the
   character tick that computes a result, writes it through the reflected
   `BaseValue`, and recomputes. Like `applyDamage`/`routeDamageMeta`, they are
   the pipeline's own terminal writes — centralized, auditable, and save-safe
   (they write `BaseValue`s) — not ad-hoc bypasses. What §2.9 forbids is
   *arbitrary* code mutating attributes **outside** this sanctioned set; new
   execution-calc sites live in the damage path / character tick and recompute
   after.

10. **Simulation runs headless.** `gameplay/`, `world/`, `data/`, `script/`
    have zero dependency on `engine/platform/`, `engine/rhi/`, or
    `engine/render/`. They compile and run without SDL, OpenGL, or any renderer.
    The test suite (229+ headless tests) enforces this. Any violation is a
    coupling to remove on sight — it is what keeps the post-demo Godot
    option (and any other frontend swap) open.

11. **Reuse before build.** Before writing any new system, name the
    existing one that covers the need (GAS effects/abilities, the
    condition evaluator, dialogue events, child-record data patterns,
    the pending save layer, schedules/AI packages, game-clock
    timestamps, the container/barter UI, RmlUi screen controllers…).
    New code is an EXTENSION of the nearest existing pattern, never a
    parallel mechanism. If nothing fits, say so explicitly and ask
    before inventing. (Dev directive 2026-07-12, FOLLOWERS planning.)

---

## 3. Tech stack

Use these unless there is a concrete reason not to (then ask first).

| Concern            | Choice                                  | Notes |
|--------------------|-----------------------------------------|-------|
| Build              | CMake + CPM.cmake (FetchContent)         | Deps pinned in CMake, fetched at build time, identical on Fedora/Debian/Windows, no per-machine bootstrap. vcpkg (manifest) / Conan also work cross-platform. |
| Window / input     | SDL3                                     | Gamepad, events, optional audio. |
| Math               | GLM                                      | Don't reinvent. |
| GPU                | Vulkan (final renderer) + OpenGL 4.6 fallback, behind RHI | 2D renderer + **custom 3D landscape renderer** (`docs/RENDERING.md`) — the demo ships on it (2026-07-05 pivot). Vulkan/MoltenVK is the shipped backend since 2026-07-19 (macOS is Vulkan-only); GL 4.6 stays the PC fallback. Godot via GDExtension = post-demo option. |
| Mesh / model       | glTF 2.0 via cgltf                       | Skinning, anims, PBR built in. |
| Textures           | stb_image + cooked `.mtex` (BC7/BC5/R16, offline mips) | `tools/cooker cook-terrain-materials`; loader `engine/assets/CookedTexture`. KTX2/Basis = future option if interop is ever needed. |
| ECS                | flecs (pinned v4.1.5)                     | Runtime only (lib `meadows-ecs`); data model stays flecs-free. Used directly in systems, not behind a façade. Our reflection — not flecs meta — is the keystone (§2.3). See `docs/PHASE-2.md`. |
| Physics            | Jolt (pinned v5.2.0)                     | Integrated (lib `meadows-physics`, pimpl facade — no Jolt type in any header). 2D scenes keep the simple custom collision. |
| Navmesh (3D)       | Recast / Detour                          | Not integrated yet; `engine/nav/` interface exists with a grid-A* stub behind it. |
| Scripting          | Lua via sol2                             | AngelScript / WASM as future options. |
| Dev UI / editor    | Dear ImGui                               | |
| Game UI            | RmlUi (decided 2026-07-05)               | Integrated (lib `meadows-ui`). RML/RCSS documents served through the §5 plugin VFS → document-level UI modding (the SkyUI/Scaleform model). Dev tools stay ImGui. |
| Audio              | miniaudio                                | Integrated (lib `meadows-audio`, null backend for headless). |
| Logging            | spdlog                                   | |
| Reflection         | Custom (macros/codegen)                  | The keystone — see §2.3. |

**On-disk formats:** authoring/mod records in **TOML** (decided Phase 1) so
mods diff cleanly under version control (this serves the "git-like" intent);
a **binary cooked** format for runtime load. The text↔binary tool is
`tools/cooker`.

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
  /platform     # cross-platform layer (mostly SDL3). ONE .hpp per concern;
    /common     #   only common/ exists today — posix/linux/win32 appear the
                #   day something needs a per-OS .cpp (§3.1 pattern)
  /core         # logging, asserts, job system, math aliases, Guid, Rng, clock
  /reflect      # reflection system (field registration, type info) — keystone
  /rhi          # graphics abstraction (interface + compute extension)
    /backends
      /gl       # OpenGL 4.6 backend (only backend for now)
  /render       # 2D sprite renderer on the RHI
    /landscape  # the custom 3D renderer (docs/RENDERING.md)
  /assets       # asset DB (GUID), loaders (stb, cgltf), async residency
  /ecs          # flecs wrapper (lib meadows-ecs; flecs confined here)
  /anim         # skeletal anim runtime — headless, flat params (no data/ dep)
  /physics      # Jolt facade (lib meadows-physics, pimpl)
  /ui           # RmlUi adapter (lib meadows-ui) — GAME UI, not dev panels
  /audio        # miniaudio facade (lib meadows-audio, null backend headless)
  /fx           # CPU particle emitters (flat params)
  /nav          # Navigator interface (grid-A* stub; Recast later)
/data
  /forms        # Form types + FormQuery helpers (childrenOf: the child-record
                #   pattern for variable cardinality — reflection stays flat)
  /plugins      # loader, field-level patch resolution, TomlWriter,
                #   PluginConfig (plugins.toml), EditSession (editor→plugin);
                #   a save is a runtime patch layer on this machinery — there
                #   is deliberately NO separate /save directory
/world
  /worldspace   # worldspace -> cell grid -> references; categories; prefabs
  /streaming    # cell load/unload (CellLoader)
  /scene        # components, Spawner, AnimBridge — shared 2D/3D, maps
                #   Forms -> engine flat params (the data->engine seam)
  /ai           # GridNavigator
/gameplay       # ability (GAS core), actors, ai (packages/schedules),
                #   combat, condition, cue (GameplayCues), event, faction,
                #   interaction (furniture), inventory, stats
/script         # Lua VM integration (sol2), event dispatch, bindings
/quest          # quest state machines, aliases, dialogue
/game           # true-adventurer: entry point, scenes (incl. EditorScene,
                #   LandscapeScene), ImGui dev panels (game/ui), data plugins
/tools          # cooker (text<->binary); future importers/validators
/tests          # the headless doctest suite (proof of §2.10)
/userdoc        # user & modder documentation (hub: userdoc/README.md)
```

Keep `gameplay/`, `world/`, `quest/`, `script/`, `data/` free of any
`rhi/`, `render/`, or backend includes; `engine/*` never includes `data/*`
(Forms are mapped to plain params in world/gameplay/runtime code).

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
  functions `resolved FormDatabase -> patch plugin`; scriptable in Lua (the
  shared VM exists since Phase 4). Output is again an ordinary plugin.
- Prerequisite when building the tool: extend `FieldConflict` to carry each
  writer's *value*, not just its name (small resolver change).
- Invariant to protect: never add a parallel resolution mechanism — the
  answer to "force this value" is always "one more layer" (§2.4).

Target: the editor conflict view (MEADOWS-PLAN, chantier « interfaces » —
the PluginsPanel already displays per-field conflicts), or a CLI subcommand
if the need bites earlier.

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

- **The custom renderer path IS taken** (2026-07-05 pivot: demo in Meadows).
  The 3D landscape renderer is built — journal, architecture and remaining
  brick specs in `docs/RENDERING.md`; their scheduling lives in the
  `docs/MEADOWS-PLAN.md` chantiers. The instanced 2D sprite renderer stays
  for the 2D test scenes. Godot remains a post-demo option
  (`docs/SIMULATION-AND-PRESENTATION.md`).
- The "BotW look" is mostly **art direction + soft GI feel + atmosphere**, not
  exotic tech. Flat-ish albedo, lighting does the work. Keep it stylized.
- **Asset residency at spawn (both 2D and 3D).** Render components
  (`SpriteRender`, `MeshRender`) hold an **asset handle**, never pixels
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
- **Comments carry only durable information** (policy 2026-07-22): the
  architectural why, invariants, contracts, cross-file coupling warnings,
  and one-line pointers to `docs/*.md`. **Never** put dates, attributions
  ("dev pick", "retour dev"), old values ("0.045 -> 0.03", "halved"), or
  plan/journal tags (brick/brique N, chantier X, V8f, "appended") in code
  comments — that history belongs in the `docs/` journals and in git. A
  hand-tuned magic number gets at most `// hand-tuned`. Class/function
  contract docs live in the **header** at the declaration; implementation
  detail lives at the code site in the `.cpp`; don't duplicate `docs/`
  content — point to it.
- Every new Form type / component **registers its reflection** in the same
  file it's declared.
- Determinism: route all gameplay randomness through the engine RNG (seeded,
  serialized) so saves and replays are reproducible.
- Tests for the data model, plugin resolver, and save layering are
  **mandatory** — these are the systems where silent bugs are most expensive.

---

## 9. Implementation history (phases 0-8) & roadmap pointer

**The living roadmap is `docs/MEADOWS-PLAN.md`** (single driver since the
2026-07-05 pivot: the gameplay demo is built in Meadows; work proceeds in
*chantiers*, each planned brick-by-brick when it starts). This section is
the **history** of the phased 2D build-up — kept because each phase's
journal (`docs/PHASE-*.md`) documents decisions you must read before
touching the corresponding systems.

- **Phase 0 — Foundations** (2026-06-12): CMake+CPM.cmake, SDL3
  window/input, logging, job system, math, RHI interface + GL 4.6 backend,
  instanced 2D sprite renderer, ImGui integration.
- **Phase 1 — Data model** (2026-06-12): reflection system, Forms,
  **field-level patch resolution**, text↔binary cooker, GUID asset DB.
  **Journal: `docs/PHASE-1.md` — read it before touching the data/modding
  model** (the non-obvious decisions live there).
- **Phase 2 — ECS + world model** (2026-06-13): entities/components,
  worldspace→cell→reference hierarchy, 2D world populated.
  **Journal: `docs/PHASE-2.md` — read before touching ecs/world.**
- **Phase 3 — Gameplay 2D + GAS core** (2026-06-14): player controller,
  2D collision/triggers, inventory/items, simplified GAS (attributes +
  effects + tags + abilities), combat as effects, grid-A* AI, factions,
  perception. Lib `meadows-gameplay`. **Journal: `docs/PHASE-3.md` — read
  before touching gameplay/GAS.**
- **Phase 4 — Scripting, abilities & quests** (2026-06-14): Lua (sol2)
  VM, event dispatch, scripts on forms/refs, tag-based condition evaluator,
  quest state machines + aliases, dialogue trees. Libs `meadows-script` +
  `meadows-narrative`. **Journal: `docs/PHASE-4.md`.**
- **Phase 5 — Multithreading architecture** (2026-06-15): the thread
  model, the **JobSystem-vs-flecs-scheduler boundary**, and the async
  asset-residency path (§7: background decode → main-thread GPU upload,
  never block spawn or the frame). Three rules constrain new code daily:
  - **Decouple ≠ thread.** The sim produces a self-owning **render
    snapshot** each frame (`extractScene` → `RenderSnapshot` →
    `submitSnapshot`, `game/SceneSubmit`); the renderer never touches the
    `World`. So where the consumer runs is a **scheduling policy, not an
    architectural invariant** — sim+render on the main thread stays the
    default, and the strict snapshot keeps a render thread cheap to add.
  - **JobSystem owns task parallelism; flecs systems stay
    single-threaded** until profiling says otherwise. Orthogonal axes —
    do not conflate them.
  - **Main owns the ECS world and the GPU; a worker touches neither.**
    Workers publish into a non-blocking completion queue, drained at a
    fixed point each frame in a **deterministic order** (never
    `JobSystem::wait()` on the frame thread) so completion order never
    perturbs the engine RNG or saves (§8).

  **Journal + crash postmortem: `docs/PHASE-5.md` — read before touching
  `game/SceneSubmit`, `engine/core/`, or `game/TextureCache`.** Build
  lesson: after a shared-type layout change, do a **clean rebuild** (ninja
  header-dep miss → stale-obj heap corruption).
- **Phase 6 — Character stats: vertical slice** (2026-06-15): 9
  attributes → 3 primary stats (Health/Energy/Essence), **Resonance**
  (hidden signed stat) + **Harmony** cascade, derived secondary stats via
  C++ calculators + data constants, typed-damage → armor/resistance →
  health+posture pipeline with stagger, game clock, survival→resonance
  loop. **Journal: `docs/PHASE-6.md`; design: `docs/STATS.md`.**
- **Phase 7 — Permanent-status & resonance mechanics** (2026-06-17):
  seeded engine RNG (`core::Rng`, §8), `StatsTuningForm` (Phase-6
  constants → moddable data, §5), stat-bearing **items/equipment**,
  **rest/sleep** recovery, status **buildup**, body-part **injuries**,
  **diseases/psychoses**, **drugs + harmony break**. Everything routes
  through `EffectForm`/`applyEffect` (GAS unification). **Journal:
  `docs/PHASE-7.md`; design: `docs/STATS.md`; modder reference:
  `docs/MODDING-EFFECTS.md`.**
- **Phase 8 — Combat 2D dynamique (PARTIELLE, absorbée) :** steps 1-3
  faits (tick multi-combattants, contrôleur joueur move/dodge, attaque
  mêlée à dégâts typés) ; la scène CombatArena reste le banc d'essai GAS.
  Le reliquat (IA ennemie, PNJ repos/marchand) est absorbé par le
  **chantier « vivant »**. **Journal: `docs/PHASE-8.md`.**
- **Phases 8.5 → 14 — ABSORBÉES par `docs/MEADOWS-PLAN.md` (2026-07-06)**,
  à une exception près : la *validation Godot* (ex-Phase 8.5) est
  **reportée post-démo**, son scope conservé dans
  `docs/SIMULATION-AND-PRESENTATION.md` — le seam sim/présentation (§2.10)
  reste un invariant prouvé par les tests headless.

> **WHERE THE PROJECT IS (2026-07-06) — the roadmap now has ONE driver:**
> **`docs/MEADOWS-PLAN.md`** (décision 2026-07-05 : la démo de gameplay se
> fait dans Meadows ; Godot reporté post-démo). La numérotation de phases
> s'arrête ici : les phases 0-8 ci-dessus sont l'HISTORIQUE (avec leurs
> journaux `docs/PHASE-*.md`), les phases 8.5-14 sont absorbées par les
> chantiers de MEADOWS-PLAN (mapping ci-dessus). **Toute planification de
> la suite part de MEADOWS-PLAN, pas de cette section.**
>
> **Trois documents d'état, un par piste :**
> 1. **`docs/MEADOWS-PLAN.md`** — LE plan de la démo : décisions actées
>    (RmlUi, in-place, skills-by-use, texturé stylisé, monde fait main),
>    chantiers verticaux à venir. À lire avant tout nouveau chantier.
> 2. **`docs/HORIZONTAL-PASS.md`** — la passe d'architecture FAITE
>    (2026-07-06) : nouveaux Forms, boucle éditeur→plugins (GameDB/
>    console), seams Jolt/RmlUi/anim/miniaudio (libs meadows-physics/-ui/
>    -audio), cues/schedules/mobilier/particules/nav, prefabs, contrats
>    renderer + audit de compat. **C'est le contrat d'implémentation des
>    verticales : suivre les seams, ne pas les redessiner.**
> 3. **`docs/RENDERING.md`** — LE doc du rendu (anglais, 2026-07-26) :
>    architecture RHI/backends (Vulkan = renderer final), lighting/GI/
>    volumétrique, perf, leçons, roadmap, chantiers RENDERER-EXTRACT et
>    nuages. Les journaux détaillés d'origine sont dans `docs/archive/`.
>    À lire avant de toucher `engine/rhi/`, `engine/render/` (dont
>    `render::WorldRenderer`) ou `LandscapeScene`.
>
> Doc utilisateur/moddeur : `userdoc/README.md` (hub) — à maintenir à
> chaque verticale livrée. Entrée du dépôt : `README.md`.

---

## 10. When in doubt

- If a task touches the **data model, plugin resolution, or saves**, re-read §5
  and prefer asking over guessing — these invariants are expensive to change
  later.
- If a feature can be proven **headless** (a doctest) or in an existing 2D
  scene, do that before wiring it into the 3D frontend.
- If you're about to make the **renderer fancier** while core systems are
  incomplete, stop and reprioritize.
- Never set an attribute directly — apply a **GameplayEffect** (§2.9, §6).
- If a new feature seems to need a **new system**, re-read §2.11 first —
  name the existing mechanism it extends before writing anything.
- Prefer the **simplest thing that exercises the concept**; this is a prototype
  for a solo developer, not a production engine.
