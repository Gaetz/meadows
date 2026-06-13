# Phase 1 — Data model (DONE 2026-06-12)

> Journal of the Phase 1 implementation, moved out of `CLAUDE.md` to keep the
> roadmap lean. This is the canonical record of what Phase 1 delivered and the
> non-obvious decisions taken along the way. Re-read it to resync after a
> context compression, or before touching the data/modding model (§5 of
> `CLAUDE.md`).

Phase 1 de-risks the whole project: reflection, Forms, the field-level plugin
resolver, the text↔binary cooker, and the GUID asset DB. Each brick landed with
green tests in the `meadows-tests` (doctest) suite.

## Libraries / layering

- `meadows` — core engine static lib (core, reflect, rhi, render, assets, ui,
  platform). Never depends on data.
- `meadows-data` — Forms + plugins static lib. Depends on `meadows`, **never**
  on `render`/`rhi`.
- `meadows-tests` — doctest suite (`tests/`).
- `tools/cooker` — standalone CLI (`tools/cooker/Main.cpp`).

## Bricks

### (a) GUID + reflection
- `core::Guid` (`engine/core/Guid.{hpp,cpp}`): v4 GUID, string roundtrip.
- FNV-1a string ids (`engine/core/Hash.hpp`).
- All-in-header reflection (`engine/reflect/Reflect.hpp`):
  `REFLECT_BEGIN / FIELD / END` macros, type-erased field get/set via
  member-pointer lambdas.
- Explicit `reflect::Registry` (`engine/reflect/Registry.{hpp,cpp}`).
- Tests: `tests/GuidTest.cpp`, `tests/ReflectTest.cpp`.

### (b) Forms
- `data/forms/`: `Form` base (`Guid id` + `editorId`), sample
  `WeaponForm`/`ActorForm` (`CoreForms.{hpp,cpp}`), `FormDatabase`
  (Guid → Form lookup + compact `FormHandle` table), `FormTypeRegistry`.
- TOML record parsing via toml++ 3.4 (`TOML_EXCEPTIONS=0`).
- **Decisions**: `Form::id` is NOT reflected (it is identity, not payload);
  quaternion file order is `[x, y, z, w]`.
- Tests: `tests/FormsTest.cpp`.

### (c) Plugin resolver
- `data/plugins/Resolver.{hpp,cpp}`: ordered-layer resolution,
  **last-writer-wins per field**.
- Conflict report: a field written by ≥2 plugins (base game included —
  filtering the base out is a presentation concern, not a resolver one).
- Edge cases handled: duplicate `create` degrades to patch; orphan patches
  (no creator) counted + dropped; a patch appearing before its creator is
  re-ranked by load order; wrong-type patch skipped; fully deterministic.
- The **save system reuses this resolver as-is** (§2.4) — there is no separate
  save subsystem.
- `data/plugins/Record.hpp`, `PluginLoader.{hpp,cpp}` carry the record / load
  plumbing.
- Tests: `tests/ResolverTest.cpp`, `tests/PluginLoaderTest.cpp`.

### (d) Cooker
- `data/plugins/BinaryFormat.{hpp,cpp}`: magic `'MDWP'` + version,
  little-endian, fields sorted by id → deterministic cooks, bounds-checked
  reader. Binary reading needs **no registry** (raw ids, validated by the
  resolver).
- `data/plugins/TomlWriter.{hpp,cpp}`: fields sorted by name → clean diffs.
- `tools/cooker/Main.cpp` CLI: `cook` / `uncook` / `new-guid`.
- Tests: `tests/CookerTest.cpp`.

### (e) Asset DB
- `engine/assets/AssetDatabase.{hpp,cpp}`: guid → path, **last layer wins**.
  Knows nothing about plugins — game code feeds it.
- `[assets]` table in plugin TOML (+ binary format).
- Synchronous stb_image loading (`assets::loadImageFile`,
  `engine/assets/Image.{hpp,cpp}`, `StbImpl.cpp`).
- `platform::executableDir()` (`engine/platform/Paths.hpp`).
- **End-to-end demo** in `game/main.cpp`: base plugin (iron sword) +
  `golden-blades` mod patching 3 fields and overriding the sprite asset; live
  toggle in ImGui with full re-resolution.
- Tests: `tests/AssetsTest.cpp`.

## Invariants reaffirmed in Phase 1

- Field-level patching is the primary mechanism (not record-level overrides).
- Stable identity by GUID; runtime uses compact handles resolved at load.
- Reflection is the single mechanism for serialization, patching, saves, and
  (future) editor panels.
- `meadows-data` must never pull in render/rhi.
