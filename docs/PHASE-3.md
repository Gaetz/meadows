# Phase 3 — Gameplay 2D (GAS core first) (IN PROGRESS)

> Journal of the Phase 3 implementation, same role as `docs/PHASE-1/2.md`. Read
> it before touching `gameplay/` or the GAS systems. Re-read to resync after a
> context compression.

Phase 3 builds gameplay on the 2D world. The architectural risk is the
**simplified Gameplay Ability System** (§6/§2.9); the rest (player controller,
2D collision, inventory, AI, factions, perception) is more mechanical and lands
as later bricks.

## Architecture decided with the dev (after reading Unreal GAS)

We read the canonical GAS reference (tranek's GASDocumentation) to find which
pieces are inseparable. Outcome:

- **The GAS core is inseparable.** ASC (our **AbilitySystem** component),
  **Attributes/AttributeSets**, **GameplayEffects**, and **GameplayTags** are
  mutually dependent — you cannot do "just attributes" (an attribute has no
  meaning without effects, the only mutator, which need tags for requirements /
  granted state). So Phase 3 builds the **whole core**, **plus a minimal
  GameplayAbility** (dev's call). The detachable upper layer — AbilityTasks
  (`wait()`), the condition evaluator, Lua — is **Phase 4**.
- **Attributes = reflected C++ AttributeSets.** An attribute is a `{ BaseValue,
  CurrentValue }` pair; an AttributeSet is a reflected component grouping them.
  Effects target an attribute **by reflection** (`TypeInfo::findField`), exactly
  like UE's `FGameplayAttribute` (a reflected property reference). Mods extend
  via effects/abilities/tags, not by adding attributes (faithful to GAS, which
  requires AttributeSets in C++).
- **GameplayEffects are the only mutator** (§2.9): Instant→BaseValue,
  Duration/Infinite→CurrentValue (modifiers), Periodic→re-apply on tick. Ops:
  **add / multiply / override + clamp**. Effects carry **granted tags**,
  **required/blocked tags**, and **immunity**.
- **Damage/heal = a transient `Damage` meta-attribute + PostExecute hook** that
  routes into `Health` (clamp, later armor/shield). Clamp via PreAttributeChange.
  Keeps the damage formula data-driven (the GAS way).
- **GameplayTags = interned hierarchical vocabulary + ref-counted container.**
  Dotted names (`Status.Burning`) interned to `core::fnv1a` ids; a registry
  records parent chains; the owned-tag container is **ref-counted** (multiple
  effects can grant the same tag). **Not** one Form per tag; **≠** flecs tags
  (ECS storage).
- **Minimal GameplayAbility**: an `AbilityForm` with required/blocked activation
  tags, a `cost` effect, a `cooldown` effect, a primary effect applied to the
  target, and an optional C++ handler id. Activation checks tags + cooldown +
  affordable cost, then applies cost + cooldown to self and the effect to the
  target. **Cost & cooldown are themselves effects** (faithful to GAS).
- **Effect pipeline is a flat linear sequence, NOT a node-graph.** Application is
  a fixed ordered sequence (capture → magnitude → requirements → apply →
  PostExecute → tags). Conditional branching is expressed with **required/blocked
  tags + the condition evaluator** (Phase 4), not a graph: a graph is a topology
  that does not field-patch cleanly (§5/§2.4) and hurts determinism (§8). A
  node/graph representation is **reserved** for AI behavior trees, dialogue/quest
  graphs (Phase 4), and an editor view *over* flat data (Phase 9) — never the
  runtime/persisted GAS model.
- **Public API kept deliberately tiny** (~4 functions): `applyEffect`,
  `tryActivate`, `getAttribute`, `hasTag/addTag/removeTag`. Authoring is flat
  declarative TOML. Complexity stays internal.
- **Cut/deferred** (the doc marks them optional): prediction/replication
  (single-player), GameplayCues (we have our own rendering), AbilityTasks
  (Phase 4), MMC / ExecutionCalculations, snapshot, stacking.
- **Persistence**: runtime containers (active effects, owned tags, granted
  abilities) are `std::` containers; their serialization is **Phase 5** (same
  no-container-type-in-reflection wall as cells).
- **Determinism**: GAS tick is an ordered C++ update (no flecs pipeline yet);
  randomness via the engine RNG (§8).

## Lib / placement

New lib **`meadows-gameplay`** (`gameplay/`), depends on `meadows-data` +
`meadows-ecs`, **render-free** (§4). The `AbilitySystem` is a reflected ECS
component on actor entities. Tags live here too (reused by factions in Phase 3,
then the condition evaluator / quests in Phase 4).

## Bricks (dependency order; each lands green; STOP for validation between each)

### (3a) GameplayTags — DONE 2026-06-13
- New lib **`meadows-gameplay`** (`gameplay/`, deps `meadows-data` +
  `meadows-ecs`, render-free); `add_subdirectory(gameplay)` wired in root.
- `gameplay/ability/GameplayTags.hpp` + `.cpp`:
  - `GameplayTag { u32 id; }` — fnv1a of the dotted name, 0 = invalid.
  - `GameplayTagRegistry` — `registerTag` (auto-registers ancestors,
    idempotent), `find`, `nameOf`, `parentOf`, `isA` (walks the parent chain).
  - `TagContainer` — ref-counted, stores the **ancestor-expanded closure** so
    `has()` is registry-free and ancestor-aware (has("Status") true when
    "Status.Burning" is owned); ref-count keeps a tag while any source grants it.
- Tests `tests/GameplayTagsTest.cpp`: ancestor auto-registration, idempotent +
  stable ids, `isA` chain, ancestor-aware `has`, ref-counting across multiple
  grants. Suite green (59 cases / 584 assertions); build clean.
- **Note:** these are our moddable gameplay tags, **not** flecs tags. The
  registry is explicit (no statics); a data manifest can feed it later.

### (3b) Attributes + AttributeSets + AbilitySystem skeleton — DONE 2026-06-13
- **Representation decision** (reflection v1 has no nested-struct / no
  `Attribute` Value alternative): the `AttributeSet` reflects its **BaseValues**
  as flat `f32` fields (patchable §5, serializable Phase 5); **CurrentValues**
  are a runtime overlay on the `AbilitySystem` (recomputed in 3c, §2.9). This is
  the §2.9-faithful split and reuses the reflection keystone — no new system.
- `gameplay/ability/Attributes.hpp`: `AttributeSet` reflected component
  (health/maxHealth/stamina/maxStamina/magicka/maxMagicka/armorRating + the
  transient `damage` meta-attribute). `attr(name)` = `fnv1a` id (the data-driven
  attribute handle effects target).
- `gameplay/ability/AbilitySystem.hpp` + `.cpp`: `AbilitySystem` component
  (`TagContainer` + `current` overlay map; runtime-only, not reflected — its
  members are containers). `registerGameplayComponents(World&)` (AttributeSet via
  the reflection bridge, AbilitySystem as a plain flecs component). Accessors
  **addressed by reflection**: `baseValueOf`/`setBaseValue` (read/write a set
  field by id via `findField`), `initializeCurrent` (seed overlay from base over
  every reflected f32), `currentValueOf`/`setCurrentValue`.
- Tests `tests/AbilitySystemTest.cpp`: reflection-addressed base get/set
  (unknown id → nullopt/false), overlay seeded from base then independent,
  components register + attach to an entity. Suite green (62 cases / 598
  assertions); build clean.

### (3c) GameplayEffects + modifier pipeline — TODO (the big one)
`EffectForm` (Form): modifiers (attribute, op, magnitude), duration policy,
period, granted/required/blocked tags. Application + recompute + tick system +
meta-attribute Damage→Health + clamp + immunity.

### (3d) Minimal GameplayAbility — TODO
`AbilityForm`, grant + `tryActivate` (tags/cost/cooldown), apply effect to target.

### (3e) Combat in 2D — TODO
Attack ability → damage effect → `State.Dead`; ImGui debug panel in `game/`.

## Rest of Phase 3 (later bricks)
Player controller, 2D collision/triggers, inventory/items/equipment, AI
(grid A* + packages + perception), factions (tags + relations table).
