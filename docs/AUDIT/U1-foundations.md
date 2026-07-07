# Audit U1 — Fondations & keystone

Scope: `engine/core/`, `engine/reflect/`, `engine/assets/`, `engine/anim/`,
`engine/fx/`, `engine/platform/`. Read-only audit, no code changes.

## Verdict de santé

This unit is the **healthiest** part of the codebase, as the inventory
predicted. The primitives are small, single-purpose, RAII-clean, well
commented, and the CLAUDE.md invariants they touch hold:

- **§3.1 platform discipline: PASS.** No `windows.h`/`X11`/Wayland/SDL/glad
  include leaks into any `.hpp` (grep clean). SDL appears only in
  `platform/common/*.cpp`. Native handles are hidden behind pimpl
  (`Window::Impl`, `GlContext::Impl`) or opaque `void*`/`const void*` tokens
  (`Window::nativeHandle`, `Input::handleEvent`). Headers are platform-clean.
- **§8 ownership: PASS (near-total).** No `delete`/`malloc`/`free`; owning
  resources use `uptr`; cgltf/stb frees are RAII/scoped. Two literal `new`
  survive (factory pattern, finding 07).
- **§2.5 identity by GUID: PASS.** `core::Guid` is 128-bit, stable, with a
  deterministic `combine()` for the prefab-child contract; `Guid::generate()`
  correctly uses a SEPARATE `mt19937_64` so identity minting never disturbs the
  gameplay `core::Rng` stream (Guid.cpp:29-30).
- **§8 determinism: PASS.** `core::Rng` (xorshift64\*) is seedable, serializable
  (`rawState`/`setRawState`), passed by value/ref, no global.

The findings below are guard-rail gaps and shared-primitive **absences**, not
broken code. The single item that deserves attention is finding 01 (keystone
ordinal safety).

## Findings (most important first)

### U1-01 — [HIGH][archi/qualité] Three parallel ordered lists gate the binary format, guarded only by comments
`engine/reflect/Reflect.hpp:39-77` (+ consumer `data/plugins/BinaryFormat.cpp:128-201`).
`FieldKind` (enum), the `Value` variant's alternative order, and
`detail::KindOf<T>` must stay in lockstep: `valueKind()` casts
`variant::index()` straight to `FieldKind` (Reflect.hpp:59-61), and the binary
serializer **persists that index as a raw byte** — so it is the on-disk contract
for every cooked plugin AND every save. Only human comments ("appended last so
existing kind ordinals stay stable", line 50) enforce the ordering. Note: the
pre-scan premise that *reordering a `REFLECT_FIELD` corrupts binaries* is
FALSE — field records are keyed by `fnv1a(name)`, not position (BinaryFormat.cpp:325-330);
the real fragile ordinal is the `FieldKind`/`Value` pair. No `static_assert`
pins `variant index == FieldKind == KindOf`. A future insert/reorder in the
middle of these lists silently corrupts all existing data with no compile error.
Action: add `static_assert` per kind that `std::variant_alternative_t<index, Value>`
matches and that `KindOf<T>::value == index`. Effort **S**. **inter-unit: yes**
(binary/TOML/resolver all consume this ordering).

### U1-02 — [MED][qualité] Reflection type-id collision is logged, not enforced; colliding type silently vanishes
`engine/reflect/Registry.cpp:7-14`. `add()` uses `emplace`, which keeps the
FIRST entry; a genuine `fnv1a` (32-bit) collision or accidental double-register
only emits `LOG_ERROR` and drops the second `TypeInfo`. That Form then fails to
resolve at load with no hard stop, and in release the log may be filtered. For
the keystone registry this should be a hard invariant. Action:
`ENGINE_ASSERT` on a real collision (distinct pointers, same id), or a
startup/build duplicate scan. Effort **S**.

### U1-03 — [MED][réutil] ABSENCE confirmed: no `Result`/`expected` type despite §8 mandate
§8 asks for `std::expected`/result types for recoverable errors; grep finds
`std::expected` ONLY in CLAUDE.md — nowhere in first-party code. The de-facto
pattern is `std::optional<T>` + a separate `LOG_*` at the failure site
(`assets/Image.cpp:15-18`, `AssetDatabase.cpp:11-19`, `Guid::fromString`,
`GltfMesh` loaders, `reflect` `set` returning bare `bool`). Consequence: the
caller gets `nullopt`/`false` with the error REASON already thrown away to the
log — it cannot react to *why*. A shared `core::Result<T>`/`Status` in
`engine/core/` would unify this. Action: introduce one small result type, adopt
incrementally. Effort **M**. **inter-unit: yes** (every subsystem returns
optionals).

### U1-04 — [MED][réutil] ABSENCE confirmed: no shared clock/time primitive
`engine/core/FrameProbe.hpp:20` re-derives `using Clock = std::chrono::steady_clock`
privately, and raw `std::chrono` is scattered across `engine/Engine.cpp`,
`engine/render/landscape/TerrainSystem.cpp`, `engine/ui/UiSystem.cpp`,
`game/SaveGame.cpp`. There is no `core::Clock`/`core::TimePoint`/`nowMs()` alias
even though `core/` is exactly where it belongs. Action: add a tiny
`core/Clock.hpp` (steady_clock alias + `nowSeconds()`/`nowMs()` helper), migrate
call sites opportunistically. Effort **S**. **inter-unit: yes** (core, render,
ui, game).

### U1-05 — [LOW][réutil/factor] `hashU32` integer mixer duplicated (fx ↔ landscape)
`engine/fx/Particles.cpp:10-26` defines `hashU32` + `HashRng`, with a comment
admitting it is the "same hash family as the landscape scatter". The particle
non-determinism is justified (cosmetic, explicitly not gameplay RNG — §8
respected), but the *integer finalizer* is a copy of the one in the landscape
scatter system. Candidate: a single `core::hashU32(u32)` mixer reused by both.
Effort **S**. **inter-unit: yes** (fx + render/landscape).

### U1-06 — [LOW][qualité] `JobCounter` lifetime is comment-enforced; dangling reference is UB with no guard
`engine/core/Jobs.hpp:14-26`, `Jobs.cpp:55-66`. `enqueue(JobCounter&, Job)`
captures `&counter` by reference into a detached worker job; the counter "must
outlive its jobs" is enforced only by a header comment. A stack counter that
leaves scope before `wait()` is silent UB. Acceptable for the prototype
(single current client), but a footgun as JobSystem gains callers. Action:
document the ownership contract at each call site, or consider a shared-state
handle. Effort **M** (design).

### U1-07 — [LOW][propreté] Literal `new` in the two factory constructors
`engine/platform/common/Window.cpp:46` and `GlContext.cpp:43` do
`uptr<Window>{ new Window() }` because the ctor is private and `make_unique`
cannot reach it. Benign and immediately owned, but §8 says "no raw owning
pointers"; a private friend make-helper would keep the literal `new` out of
sight. Effort **S**.

### U1-08 — [LOW][archi] `assets/GltfMesh.hpp` depends on `render::MeshData`
`engine/assets/GltfMesh.hpp:8` includes `engine/render/MeshData.hpp`, coupling
the asset loader to the renderer's vertex layout. This is NOT a headless-sim
violation (both are `engine/`, and gameplay/world stay clean), but it means the
gltf loader cannot be reused without the render module's mesh format. Note /
watch-only; no action required unless the seam is reused elsewhere. **inter-unit:
yes** (assets ↔ render, U1/U3).

### U1-09 — [LOW][propreté] Dead-code tree `_old/renderer_test/` present at repo root
Confirmed (own `Log.cpp`, `Engine.cpp`, `chrono` usage). Outside this unit's
paths but surfaces in every engine-wide scan; already flagged in the brief.
Action: delete. Effort **S**. **inter-unit: yes** (repo-wide).

### U1-10 — [LOW][qualité] Per-field `std::function` indirection on the serialization hot path
`engine/reflect/Reflect.hpp:94-98`. Each `FieldInfo` holds two `std::function`
(get/set). Fine for the editor/property grid, but diff/clone/save iterate every
field through a type-erased indirect call. If profiling ever flags the save/diff
path, a switch-on-`FieldKind` visitor over the member offset would remove it.
Watch-only; do not change speculatively (§10). Effort **L**.

## Findings table

| id | sev | axe | file:line | desc | effort | inter-unit |
|----|-----|-----|-----------|------|--------|-----------|
| U1-01 | high | archi/qualité | reflect/Reflect.hpp:39-77 | FieldKind/Value/KindOf ordinal triple = binary contract, no static_assert | S | yes |
| U1-02 | med | qualité | reflect/Registry.cpp:7-14 | type-id collision logged not asserted; 2nd type silently dropped | S | no |
| U1-03 | med | réutil | (core, absence) | no Result/expected type (§8); optional+log loses error reason | M | yes |
| U1-04 | med | réutil | core/FrameProbe.hpp:20 (+scattered) | no shared clock primitive; std::chrono re-derived 4+ sites | S | yes |
| U1-05 | low | réutil/factor | fx/Particles.cpp:10-26 | hashU32 mixer duplicated with landscape scatter | S | yes |
| U1-06 | low | qualité | core/Jobs.hpp:14-26 | JobCounter lifetime comment-only; dangling ref UB | M | no |
| U1-07 | low | propreté | platform/common/Window.cpp:46; GlContext.cpp:43 | literal `new` in private-ctor factories | S | no |
| U1-08 | low | archi | assets/GltfMesh.hpp:8 | asset loader depends on render::MeshData | S | yes |
| U1-09 | low | propreté | _old/renderer_test/ | dead-code tree at repo root | S | yes |
| U1-10 | low | qualité | reflect/Reflect.hpp:94-98 | per-field std::function indirection on save/diff hot path | L | no |
