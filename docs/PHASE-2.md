# Phase 2 — ECS + world model (IN PROGRESS)

> Journal of the Phase 2 implementation, same role as `docs/PHASE-1.md`: the
> canonical record of what each brick delivered and the non-obvious decisions.
> Read it before touching `engine/ecs/` or `world/`. Re-read to resync after a
> context compression.

Phase 2 turns resolved data into a live world: an ECS, the
worldspace→cell→reference hierarchy, a shared 2D/3D scene representation, and a
populated 2D world. Built in **dependency order a → c → b → d → e** (≠ the
letter order in `CLAUDE.md §9` brick labels), each landing with green tests.

## Architecture decided with the dev (the load-bearing choices)

The full discussion is summarized here so it survives context loss.

### ECS = flecs, confined to the runtime
- **flecs** (pinned `v4.1.5`, static lib) is the storage/query engine, used
  **directly in systems** — deliberately *not* hidden behind an RHI-style
  façade (only the renderer earns that abstraction, §2.1). Components are plain
  reflected structs, so flecs appears only in ecs/world/system TUs, never in
  component-data headers.
- **Build confinement:** new lib **`meadows-ecs`** (sources `engine/ecs/`, links
  `flecs::flecs_static`). `meadows` (base engine) and `meadows-data` (forms,
  plugins, resolver, save-as-patch) **stay flecs-free** and testable without
  flecs. The whole data model + plugin system never see flecs.
- **Our reflection stays the keystone (§2.3)** for the *data model*: Forms,
  patches, §5 resolution, saves. flecs has its own meta system — we do **not**
  use it for persistence (that would fork the serialization path). Components
  are reflected in **our** system (for the Phase 5 save/patch path) and merely
  *stored/queried* by flecs. flecs meta is opt-in later, only for the Phase 9
  explorer.
- **flecs ids are opaque runtime tokens.** They encode a generation (and, for
  relationship pairs, relation+target) in their high bits. **Never persist
  them, index on them, or derive meaning from them.** Persistence keys on the
  GUIDs carried by components (e.g. `RefId`); stale handles are detected via
  `Entity::is_alive()`.

### Reference = a Form resolved by the §5 resolver
- A placed instance is a **`ReferenceForm`** (a Form), so placement, move,
  disable — and later the **save** — are all **field-level patches** through the
  existing resolver (§2.4/§5). No parallel save/override mechanism.
- **`cell` is a field on the reference**, not a list on the cell: reflection v1
  has no container type, and lists don't patch cleanly under last-writer-wins
  (so this is principled, not just a workaround). Moving a reference = patching
  one field; adding one = a new record; disabling = patching `enabled`.
- **Runtime spatial grouping** uses a flecs relation `(InCell, cellEntity)`;
  **cells are ephemeral flecs entities** built from the resolved `CellForm` and
  jettisoned at unload — never persisted. Modders edit cell contents purely by
  authoring/patching `ReferenceForm` records; they never touch flecs.

### Other decisions
- **GameplayTags (§6) ≠ flecs tags.** flecs "tags" are a storage concept;
  GameplayTags are hierarchical, moddable, reflected, serialized data queried by
  the condition evaluator. Phase 3 — do **not** model them as flecs tags.
- **Single registration point:** `World::registerComponent<T>()` registers a
  component in flecs **and** in our reflected-component registry at once (chose
  a template method over a `REGISTER_COMPONENT` macro — type-safe, mirrors
  `FormTypeRegistry::registerFormType`).
- **Systems = plain queries** for Phase 2 (no flecs pipelines yet; they arrive
  with the GAS in Phase 3).
- **Render bridge lives in `game/`** as a reusable standalone module (the only
  ECS↔rhi seam), so `meadows` never depends on `world/` (DAG stays acyclic) and
  a future editor can reuse it.
- **Phase 2 is single-threaded**, asset loading synchronous/eager. Async + the
  thread model (JobSystem vs flecs scheduler) are the new **Phase 4.5**.

## Lib / DAG

```
meadows (engine, flecs-free)
  ▲          ▲
meadows-ecs  meadows-data  (both flecs-free except meadows-ecs which OWNS flecs)
  ▲              ▲
  └── meadows-world ──┘     (world/, depends on meadows-ecs + meadows-data)
            ▲
          game            (links all; hosts the reusable render bridge)
```

## Bricks

### (a) ECS core — DONE 2026-06-13
- `engine/ecs/World.hpp` + `World.cpp` (lib `meadows-ecs`):
  - `using Entity = flecs::entity`; the opaque-id rule documented in-header.
  - `struct InCell` — runtime reference→cell relation (ephemeral, never
    serialized).
  - `ecs::World` — thin owner of `flecs::world` exposed via `handle()`;
    `create()`; move-only.
  - `registerComponent<T>()` — the single registration point: registers the
    component in flecs and records `T::staticTypeInfo()` in a
    `flecs id → reflect::TypeInfo*` map (`reflectedComponent(id)`), the skeleton
    for Phase 5 generic component serialization.
- CMake: flecs pinned in root `CMakeLists.txt` (`FLECS_SHARED OFF`,
  `FLECS_STATIC ON`, `FLECS_TESTS OFF`); flecs includes forced SYSTEM for
  consumers (clean `/W4 -Wextra`). `meadows-ecs` target in
  `engine/CMakeLists.txt`.
- Tests: `tests/EcsTest.cpp` — create/set/get + `destruct` invalidation,
  add/has/remove, multi-component query (intersection), `InCell` group + unload
  via `delete_with`, registerComponent→reflection bridge. Suite green
  (43 cases / 493 assertions).
- **flecs v4 API notes** (confirmed against v4.1.5 headers): `get<T>()` returns
  `const T&`, `try_get<T>()` returns `const T*` (nullable); `entity.id()` is the
  raw 64-bit id; `world.query<Comps...>().each(...)`; `delete_with<First>(target)`.

### (c) World data model — DONE 2026-06-13
- New lib **`meadows-world`** (`world/CMakeLists.txt`, links `meadows-data`;
  `meadows-ecs` added in brick b). `add_subdirectory(world)` wired in root.
- `world/worldspace/WorldForms.hpp` + `.cpp`: `WorldspaceForm` (`cellSize`,
  `interior`), `CellForm` (`worldspace` Guid, `gridX`, `gridY`, `interior`),
  `ReferenceForm` (`baseForm`, `cell`, `position` Vec3, `rotation` Quat, `scale`
  Vec3, `enabled`, `count`) — all reflected, inheriting `data::Form`;
  `registerWorldFormTypes(FormTypeRegistry&)`.
- `world/worldspace/FormCategory.hpp` + `.cpp`: `FormCategory` enum (Static /
  Item / Actor / Container / Door) + explicit `FormCategoryRegistry`
  (typeId → category) + `registerCoreCategories` (WeaponForm→Item,
  ActorForm→Actor). Consumed by the spawner in brick b.
- `world/worldspace/WorldModel.hpp` + `.cpp`: resolved spatial index built by
  scanning a `FormDatabase` in handle order (deterministic). Queries:
  `cellAt(worldspace, x, y)`, `referencesIn(cell)` (handle order, includes
  disabled refs — filtering is the loader's call), `worldspaceOf(cell)`,
  `worldspaces()`, `cells()`. **Key decision realized:** reference→cell is a
  field; "what is in a cell" is derived by the index, not authored as a list.
- Tests `tests/WorldModelTest.cpp`: index by cell + `cellAt`/`worldspaceOf`;
  a reference keeps its instance fields; **patching a reference's `cell` moves
  it** between cells on re-resolve (ties brick c to the §5 invariant); category
  mapping. Suite green (47 cases / 521 assertions).

### (b) Reference → entity spawner — DONE 2026-06-13
- `world/scene/Components.hpp` + `.cpp`: `Transform` (Vec3/Quat/Vec3,
  dimension-agnostic §2.6), `SpriteRender` (asset Guid + size/tint/layer, **no
  rhi** §7), `RefId` (reflects only `referenceId`; `base`/`cell` are runtime
  handles, not reflected — handles never persist), and zero-size category
  markers (`StaticMarker`/`ItemMarker`/`ActorMarker` — runtime ECS tags, *not*
  GameplayTags). `registerSceneComponents(World&)` bridges the three reflected
  components into flecs + our registry; markers auto-register in flecs.
- `world/scene/Spawner.hpp` + `.cpp`: `SpawnContext { World&, FormDatabase&,
  FormCategoryRegistry& }`, `SpawnFn` per category, `Spawner::spawn(ctx, ref,
  cellEntity)`. Flow: resolve `ref.baseForm` → category → wire universal
  components (Transform from the reference; RefId; **SpriteRender seeded from the
  base form's `sprite` field via reflection** — §2.7, no per-type code) → pose
  `(InCell, cellEntity)` → call the category hook (markers now; AbilitySystem in
  Phase 3). Guards: invalid entity (logged) if the base form is unresolvable or
  its category has no spawner. Does not check `enabled` (the loader's call).
- `registerCoreSpawners` installs Static/Item/Actor.
- `meadows-world` now links `meadows-ecs`.
- **Build note:** flecs' `component<T>` template trips MSVC C4702 in our TUs;
  the SYSTEM include doesn't cover template-instantiation warnings, so `/wd4702`
  is suppressed PUBLIC from `meadows-ecs` for every flecs consumer (benign,
  flecs-internal).
- Tests `tests/SpawnerTest.cpp`: mandatory components present with the right
  values (sprite via reflection, RefId keyed on GUID, InCell, Item marker for a
  WeaponForm); **a mod patch on the reference flows into the spawned entity**
  (ties brick b to §5); unresolvable base form → no entity. Suite green
  (50 cases / 545 assertions).

### (d) Scene representation + render bridge — DONE 2026-06-13
- New reusable lib **`meadows-runtime`** (`game/CMakeLists.txt`, links `meadows`
  + `meadows-world`), sitting **above** engine and world so `meadows` never
  depends on `world/` (DAG acyclic). `game/` is now lib + exe; the executable
  links `meadows-runtime`. A future editor reuses the lib without the exe.
- `game/TextureCache.hpp` + `.cpp`: GUID → `rhi::TextureHandle`, caching and
  **owning** the textures (destroyed on `clear()` / teardown). Caches misses
  too. Missing/unloadable asset → invalid handle (SpriteRenderer draws its white
  fallback). Synchronous; async residency (§7) deferred to Phase 4.5/5.
- `game/SceneSubmit.hpp` + `.cpp`: the single ECS↔rhi seam.
  - `spriteFor(Transform, SpriteRender, texture)` — **pure** mapping (no GPU,
    unit-testable): position.xy, size = sprite.size · scale.xy, tint, and the 2D
    rotation = yaw of the quaternion (`2·atan2(z, w)`, §2.6).
  - `submitScene(world, textures, renderer)` — queries
    `const Transform`+`const SpriteRender` (read-only, §Game::draw), resolves
    textures, **stable-sorts by `layer`** (painter; no depth in 2D), submits.
    Assumes the engine owns `begin`/`end`.
- Tests `tests/SceneSubmitTest.cpp`: the `spriteFor` mapping + yaw-from-quat.
  The GPU path (submission) is verified by running the game (brick e). Suite
  green (52 cases / 554 assertions); `true-adventurer.exe` builds clean.

### (e) Populate a 2D world — TODO
`world/streaming/CellLoader` (`loadCell`/`unloadCell`; eager load all cells in
Phase 2, interface shaped for Phase 5 async). `game/main.cpp` loads a worldspace
+ cells (statics, an actor, items) and keeps the live mod re-resolution toggle.

## Tests (mandatory — §8)

`tests/EcsTest.cpp` (done) + upcoming WorldModel, Spawner, and an end-to-end
resolver↔spawn test (a mod patches a reference's `position`/`enabled` → resolved
`ReferenceForm` reflects it → spawned entity reflects it), tying Phase 2 back to
the §5 invariant.
