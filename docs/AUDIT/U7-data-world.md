# U7 — Data model + monde (`data/`, `world/`)

> **✅ MISE À JOUR 2026-07-08 — U7-2 (H-a) + U7-4 (H-b) FAITS, suggestions amendées.**
> H-a (`4ac53a5`) : dispatch `Value` exhaustif via `engine/reflect/Visit.hpp`,
> **pas** une « kind-descriptor table » (couplerait binaire↔ImGui↔Lua, viol
> §2.10). BinaryFormat writer + TomlWriter convertis, octets identiques ; le
> **Reader reste un switch** (construit depuis les octets, échoue déjà proprement).
> H-b (`40c9fcf`) : `data::diffToRecord` partagé (EditSession + SaveState).
> **`cloneFields` NON extrait** (contrairement à la suggestion) : `copyFields`
> et `copyMatchingFields` sont deux opérations distinctes à 1 appelant chacune,
> les déplacer n'enlèverait rien (§10). Synthesis = assemblage de champs choisis
> (pas un diff), hors périmètre. Voir `README.md` §H-a/§H-b.

Audit unit U7. ~5074 lines. Scope: `data/forms/`, `data/plugins/`
(Resolver, PluginLoader, PluginConfig, BinaryFormat, TomlWriter, EditSession,
Synthesis), `world/{worldspace,streaming,scene,ai,terrain}`.

## Verdict

The healthiest unit alongside reflection. §5 field-level patch model is
implemented correctly and cleanly; §2.5 GUID identity is exemplary; the
Spawner is per-category, aggregated, and wires via reflection exactly as
§2.7 requires. Cleanliness is excellent: **no** TODO/FIXME/HACK, **no** raw
`new`/`delete` (RAII/`uptr` throughout), **no** alias drift (`Defines`
aliases used consistently), all-English. The findings below are refactors
and one invariant letter-violation, not correctness bugs.

## Invariant grep results (explicit pass/fail)

- **§2.10 headless purity — `data/` = PASS** (zero `rhi/`/`render/`/backend/
  GL/SDL/ImGui includes).
- **§2.10 headless purity — `world/` = FAIL (1 include)**:
  `world/terrain/TerrainPatches.hpp:8` includes
  `engine/render/landscape/TerrainNoise.hpp`. See finding #1 — the imported
  symbols are headless-pure, so the invariant's *spirit* holds; its *letter*
  does not.
- **§2.5 GUID identity — PASS**: `FormHandle` = `index+1` into
  `FormDatabase::entries`, runtime-only, "stable for the lifetime of one
  FormDatabase" (`Form.hpp:29`, `FormDatabase.hpp:44`); persistent identity
  is the `core::Guid`. No load-order position is encoded into any persistent
  id. `Form::id` is deliberately unreflected (`Form.hpp:13`).
- **§2.4 one layer for mods+saves — PASS**: `resolve()` takes a flat
  `vector<const Plugin*>` load order; a save is simply appended last. There
  is no separate save path in the resolver.

## §5 resolver correctness (verified)

`Resolver.cpp` is a single load-order walk collecting `writes` per form,
then a deterministic materialization pass. Last-writer-wins is correct:
writes are applied in load-order, later `field->set` overwrites earlier;
conflicts are collected only when ≥2 writers touch the same field
(`Resolver.cpp:159`), in declared field order (parents first) for
determinism (`:150`). `FieldConflict` already carries each writer's *value*
(`:168`) — the §5.1 prerequisite is met. Save-as-layer, field-level
patching, and floating/out-of-order patches are all handled and logged.
Clear and well-commented.

## Findings

1. **[archi / high / inter-unit] §2.10 letter-violation: `world/` includes
   `engine/render/`.** `world/terrain/TerrainPatches.hpp:8` includes
   `engine/render/landscape/TerrainNoise.hpp`. Mitigating: `TerrainNoise.hpp`
   is pure data (`HeightPatch`/`HeightPatches`, deps = `core/Defines` + glm
   only, no RHI/GL) living in the `render` namespace by convention
   (HORIZONTAL-PASS "the engine never sees Forms"). The real defect is a
   headless-safe data struct *misplaced* under `engine/render/`, which forces
   the only render-tree include in the whole `world/`+`data/` tree.
   *Action*: relocate `HeightPatch`/`HeightPatches` to a headless home (e.g.
   `world/terrain/` or a shared `engine/core`-adjacent header) so `world/`
   stops including `engine/render/`; update the two consumers. Effort S.
   Inter-unit (U3 renderer owns the file today).

2. **[factor / high / inter-unit — H-a] Per-`FieldKind` switch replicated
   across serialization sites.** `BinaryFormat.cpp:45` (write) and its Reader
   (read) ≈23 `FieldKind::` occurrences; `TomlWriter.cpp:25` (11);
   `PluginLoader.cpp` (11). Each enumerates all 11 kinds; adding a kind means
   editing every site (plus `game/ui/PropertyGrid.cpp`, `EditorScene.cpp` in
   U5). **✅ FAIT (`4ac53a5`)** via le generic `visit` (`engine/reflect/
   Visit.hpp`), **PAS** la « kind-descriptor table » : une table centrale des
   corps `{writeBin, writeToml, editWidget}` coupllerait binaire↔ImGui↔Lua
   (viol §2.10). Writer + TomlWriter convertis (octets identiques) ; le Reader
   reste un switch (construit depuis les octets). Voir bandeau + `README.md` §H-a.

3. **[archi / med / inter-unit] Binary value stream depends on the
   `FieldKind` enum's numeric order.** `BinaryFormat.cpp:45` writes the kind
   as a raw `u8` ordinal (`valueKind(v)`); the Reader trusts it, guarded only
   by `kMaxKind` range-check (`:14`), not by reordering. Reordering the
   `FieldKind` enum in `engine/reflect/Reflect.hpp:39` silently corrupts every
   cooked plugin/save; there is no format-version byte on the value stream.
   (This is the concrete mechanism behind CLAUDE.md's "f64 appended last —
   binary ordinals stable" discipline — enforced only by a code comment.)
   *Action*: add a `static_assert` pinning enum ordinals, or a format-version
   byte. Effort S. Inter-unit (U1 reflect owns the enum).
   *Positive counterpart*: record fields are keyed by `fnv1a(name)`
   (`Reflect.hpp:87`, `Record.hpp:19`), so reordering `REFLECT_FIELD`
   declarations does **not** corrupt patches — the fragility is limited to the
   `FieldKind` enum order, not field declaration order.

4. **[factor / med / inter-unit — H-b] Reflection clone/diff logic
   duplicated.** `EditSession.cpp` has `copyFields` (`:12`, full
   `forEachField` clone) and `exportPlugin`'s diff-against-reference that
   emits only changed fields (`:193`–`:202`, the §5 "record carries only what
   it changes" rule); `Synthesis.cpp` assembles records by hand; and (per
   plan H-b) `gameplay/save/SaveState` runs `createRecord`/`copyMatchingFields`
   — the same emit-only-diffs pattern. **✅ FAIT (`40c9fcf`)** : `data::diffToRecord`
   partagé par EditSession + SaveState. **`cloneFields` NON extrait** : `copyFields`
   (same-type) et `copyMatchingFields` (cross-type par nom+kind) sont des
   opérations distinctes à 1 appelant chacune — les déplacer n'enlèverait rien
   (§10). Synthesis assemble des champs choisis (pas un diff), hors périmètre.

5. **[archi / low] §2.9: Spawner seeds attributes via a direct base-value
   write.** `Spawner.cpp:103`–`:106` calls `gameplay::setBaseValue(...)` for
   `maxHealth`/`health` rather than through a GameplayEffect. Defensible as
   one-time spawn *initialization* (not runtime mutation), and it reads the
   seed through reflection (`:99`, no per-type code — good). *Action*: confirm
   this is the sanctioned init path, or route seeding through an instant
   effect for uniformity. Effort S.

6. **[qualité / low] Load order is not validated/enforced.** `Plugin` carries
   `dependencies` (`Record.hpp:35`) but the resolver never checks them;
   patch-before-create is handled by a rank-insert + warn
   (`Resolver.cpp:78`–`:99`). Correct-enough for a hand-authored prototype and
   clearly commented as "no enforced master order yet". Record only; enforcing
   is Effort M when it bites.

7. **[reuse / low] FormQuery scans are unindexed.** `forEach` /`childrenOf`
   (`FormQuery.hpp:23`,`:38`) are O(n) full-database scans, and `childrenOf`
   re-scans every form on each call; `referencesTo` (`:67`) is O(forms×fields)
   (documented tooling-only). MEADOWS-PLAN §J already flags secondary indexes
   as future work — recorded, not over-flagged. Effort M.

8. **[propreté / low] Spawner pulls a heavy `gameplay/stats/*` include set.**
   `Spawner.cpp:10`–`:18` includes ~12 gameplay/stats headers to attach the
   full actor stat sheet (`:117`–`:123`). Legitimate — the Spawner is the
   Form→ECS seam — but it makes `world/scene` a heavy compile dependency of
   `gameplay`. Consider a `registerActorComponents(entity)` helper owned by
   gameplay so the include weight lives there. Effort S.

## Positives worth preserving

- **§2.7 Spawner**: per-category dispatch, both spawners and category
  mappings aggregated in one place each (`Spawner.cpp:264`
  `registerCoreSpawners`, `FormCategory.cpp:10` `registerCoreCategories`);
  universal wiring (Transform, Sprite/MeshRender, RefId) is driven purely by
  reflected `findField` (`:226`,`:236`) — zero per-type code. Prefab
  expansion uses derived deterministic guids so saves can target one child
  forever (`:150`). Textbook §2.7.
- **§2.2**: References are Forms (`ReferenceForm`); place/move/disable are
  field patches; render components hold `core::Guid` handles, never pixels
  (`Spawner.cpp:229`,`:238`) — §7 residency contract respected.
- **Cleanliness**: no TODO/FIXME/HACK, no raw owning pointers, no alias drift,
  all-English identifiers/comments.

## Finding table

| id | sev | axe | file:line | inter-unit |
|----|-----|-----|-----------|-----------|
| U7-1 | high | archi | world/terrain/TerrainPatches.hpp:8 | yes (U3) |
| U7-2 | high | factor | data/plugins/BinaryFormat.cpp:45; TomlWriter.cpp:25; PluginLoader.cpp | yes (U5) |
| U7-3 | med | archi | data/plugins/BinaryFormat.cpp:14,45; reflect/Reflect.hpp:39 | yes (U1) |
| U7-4 | med | factor | data/plugins/EditSession.cpp:12,193; Synthesis.cpp | yes (U6) |
| U7-5 | low | archi | world/scene/Spawner.cpp:103 | no |
| U7-6 | low | qualité | data/plugins/Resolver.cpp:78; Record.hpp:35 | no |
| U7-7 | low | reuse | data/forms/FormQuery.hpp:23,38,67 | no |
| U7-8 | low | propreté | world/scene/Spawner.cpp:10 | no |
