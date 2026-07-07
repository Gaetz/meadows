# U5 — Runtime bridge + scènes + game/ui

Scope: `game/` excluding `game/scenes/LandscapeScene.*` (U4). Covers SceneSubmit,
TextureCache/MeshCache, SaveGame, AllForms, EditorScene + other scenes
(CombatArena, DemoScenes/StatsScene, WorldDemo, UiDemo), game/ui panels
(PropertyGrid, ConsolePanel, CharacterStatsPanel), SceneStack/ScreenStack.
Lib: meadows-runtime + true-adventurer exe glue.

## Verdict

Healthy unit. The two invariants this unit owns hold:

- **§2.10 / Phase-5 seam (PASS, verified line-by-line).** `RenderSnapshot`
  (`game/SceneSubmit.hpp:29-44`) is a self-owning POD: `render::Sprite` carries an
  already-resolved `rhi::TextureHandle` (a handle, not a pointer), `MeshInstance`
  carries `core::Guid` + `Mat4` (no live World/asset pointers), `SceneLight` is
  plain values. `extractScene`/`extractMeshes`/`collectLights` are read-only over
  the World; `submitSnapshot` touches only the packet + renderer. No leak. The
  invariant is intact.
- **§2.9 (PASS).** No direct attribute mutation (`setValue`/`base =`/`currentValue =`)
  anywhere in scenes or panels. No raw `new`/`delete`, RAII throughout (§8).
- **§2.3 serialization (PASS).** SaveGame/Console route entirely through reflection
  (`createRecord`/`formFromRecord`/`copyMatchingFields`, EditSession, PropertyGrid),
  no ad-hoc per-type save code.

Main levers are factorization: the `FieldKind` switch triplicated (H-a lands here),
the residency machinery duplicated between the two caches, and the three dev-editor
panels sharing an un-extracted list+tree+propertygrid pattern.

Note (not scored): `game/` is the frontend lib, so its includes of `rhi/`/`render/`
are legitimate — the §2.10 purity grep applies to gameplay/world/data, not here.

## Findings

| id | sev | axe | fichier:ligne | description | action | effort | inter-unité |
|----|-----|-----|---------------|-------------|--------|--------|-------------|
| U5-1 | high | factor | game/ui/PropertyGrid.cpp:69-113, :155-258; game/scenes/EditorScene.cpp:34-67 | The per-`FieldKind` switch is written 3× in U5: `valueFromString` (parse), `drawPropertyGrid` (edit widgets), `valueRepr` (display), plus the `valueToString` std::visit variant at PropertyGrid.cpp:28-52. Each is ~11 cases; adding a FieldKind means editing all of them. This is the U5 face of H-a (5 sites repo-wide, ~77 occ). | Introduce one reflect `Value` visitor / descriptor table in `engine/reflect`; PropertyGrid + Console + EditorScene consume it. Collapses ~4 switches here to callbacks. | L | yes (H-a) |
| U5-2 | high | factor | game/TextureCache.{hpp,cpp}; game/MeshCache.{hpp,cpp} | The two caches duplicate the entire async-residency machinery: `Decoded{guid,generation,payload}`, `Shared{ConcurrentQueue}`, `Residency` enum, `Entry{payload,state}`, `generation`/`pending`, and the resolve→placeholder→enqueue / pumpUploads-drain / clear-bumps-generation / "no-wait destructor" logic. Only the payload type and the GPU-upload step differ. | Extract a `ResidencyCache<Payload>` template (or CRTP base) holding the queue/generation/pending/state plumbing; each cache supplies decode + upload. Removes ~60% of both. | M | no |
| U5-3 | med | factor | game/scenes/CombatArenaScene.cpp:190-207; game/scenes/DemoScenes.cpp:347-386,442-496; (also LandscapeScene, U4) | Runtime gameplay-tag registration ("State.Dead/Staggered/Paralyzed", a status-tag loop, `registerStatsRuntimeTags`, dodge/attack cooldowns) is copy-pasted per scene. There is a `registerAllFormTypes` aggregator but **no equivalent aggregator for components/tags/spawners** (pre-scan finding #7). | Add `gameplay::registerRuntimeGameplayTags(tags)` (+ a component/spawner aggregator) parallel to `registerAllFormTypes`; scenes call one function. | M | yes |
| U5-4 | med | factor | game/scenes/EditorScene.cpp:128-334 (dialogues), :341-518 (quests), :520-704 (schedules) | The three dev editors each re-implement the same shape: `forEachVisible` double-collect of a parent type + child type into vectors, a `childrenOf`/`nameOf` lambda, a parent-linked tree with inline create + `order`-swap reorder, then `drawPropertyGrid(selected)`. ~700 lines, ~70% structural overlap. | Extract a reflection-driven "record-tree editor" helper (type ids + child field + label fn) reused by all three; keep only the per-type node label. | L | no |
| U5-5 | med | archi | game/SaveGame.cpp:24-54 (materializeReference) vs :58-110 (captureReference) | The two reference-capture paths diverge in field set: `captureReference` (existing refs) diffs only `cell`, and `position`/`rotation` for actors; `materializeReference` (prefab-child) writes `position`/`rotation`/`scale`/`enabled`. A runtime-scaled or otherwise-changed ref therefore persists differently depending on prefab origin. Also the SavedStatsForm mirror (H-f) must be hand-kept in sync with component fields — a new persistent field is silently dropped if not mirrored. | Confirm the divergence is intentional (item/static immobility) and document, or unify the diffed field set; for H-f, add a test asserting component↔SavedStatsForm field-name parity. | M | yes (H-f) |
| U5-6 | med | qualité | game/ui/PropertyGrid.cpp:20 | `ActiveEdit gActive;` is a file-scope mutable global holding the in-progress edit cache. Works because ImGui has one active item, but it is a hidden global mutable state (§8 "no global mutable singletons"), and would break under two PropertyGrids / two ImGui contexts. | Move the edit cache into the caller's state (EditSession or a per-panel struct passed in), or scope it to the ImGui context id. | S | no |
| U5-7 | low | propreté | game/TextureCache.cpp:59-63 | The "First sighting: show the placeholder and decode off-thread…" comment block is pasted twice back-to-back. | Delete the duplicate. | S | no |
| U5-8 | low | factor | game/scenes/CombatArenaScene.cpp:290-293; game/scenes/DemoScenes.cpp:222-231 (also LandscapeScene) | WASD→movement-vector input handling is re-hand-written per scene. Minor, but a shared `readMoveAxis(input)` helper would dedupe. | Optional: a small input helper in game/ or engine/platform. | S | yes |
| U5-9 | low | propreté | game/scenes/DemoScenes.{hpp,cpp} | One 523-line .cpp bundles 6 scene classes (PluginScene, WorldEditScene, CombatScene, GameplayScene, NarrativeScene, StatsScene) all deriving WorldDemoScene. Acceptable as demo scaffolding but hurts navigation; StatsScene in particular (referenced elsewhere as its own scene) is buried here. | Optionally split per scene, or accept as intentional demo bundle. | S | no |
| U5-10 | low | qualité | game/scenes/EditorScene.cpp:97-108 (reload) | `reload()` rebuilds the full plugin stack + `resolve()` synchronously on the UI thread on every "Reload data". Fine for editor DB sizes today; note as a potential hitch if the DB grows. | Leave; revisit only if reload latency is felt. | S | no |

## Inter-unit items to escalate (Tier-3)

- **U5-1 (H-a):** the FieldKind switch — a single `reflect::Value` visitor/descriptor
  would collapse PropertyGrid (2 switches + 1 variant), ConsolePanel (reuses
  PropertyGrid), and EditorScene `valueRepr`, plus the data/plugins sites (U7).
- **U5-3 (finding #7):** missing component/tag/spawner aggregators — the tag list
  is duplicated across U5 scenes **and** U4 LandscapeScene.
- **U5-5 (H-f):** save capture/apply field-set drift spans U5 (`SaveGame.cpp`
  reference capture) and U6 (`gameplay/save/SaveState.hpp` SavedStatsForm mirror).
- **U5-2 / U5-8:** residency-cache and input-axis dedup are U5-internal but the
  residency pattern echoes any future streaming cache.
