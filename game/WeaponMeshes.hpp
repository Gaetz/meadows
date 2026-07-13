#pragma once

#include "engine/assets/MeshData.hpp"
#include "engine/core/Guid.hpp"

namespace game {

// Chantier P0 A2 — the procedural sword (dev design: the VISIBLE blade
// is what hits). Grip at the ORIGIN, blade along +Y up to `bladeLength`
// meters at the tip; vertex colors carry the look (steel blade, dark
// guard, leather grip — mesh.frag multiplies vColor over the white
// fallback albedo). Built on any thread, headless-testable.
render::MeshData makeSwordMesh(f32 bladeLength);

// The guid the default sword is injected under at scene boot
// (MeshCache::injectProcedural) — WeaponForms without a `model` fall
// back to it.
const core::Guid& swordMeshGuid();

// The procedural club (dev design 2026-07-11: a wooden shaft with a
// bigger metal head) — same conventions: grip at the origin, business
// end along +Y up to `length`. BanditClub points its `model` at
// clubMeshGuid in data.
render::MeshData makeClubMesh(f32 length);
const core::Guid& clubMeshGuid();

// P0 A7 — the bow: grip at the origin, limbs sweeping along ±Y with a
// forward (+Z) belly curve, string across the tips. `length` = tip to
// tip. HuntingBow points its `model` here.
render::MeshData makeBowMesh(f32 length);
const core::Guid& bowMeshGuid();

// The arrow: nock at the origin, tip at +Y `length` — the same +Y
// convention as every weapon, so the flight code orients ONE way.
render::MeshData makeArrowMesh(f32 length);
const core::Guid& arrowMeshGuid();

// FOLLOWERS É11 (tech proof): the crude pony — box body, four legs, neck,
// head, saddle. Feet at y = 0 (ground the origin like kit statics), nose
// toward +Z (the NPC facing convention). `shoulderHeight` scales the whole
// animal (canonical pony = 1.2 m at the withers). The « Poney » furniture
// form points its `model` here — v1 has no horse rig, so the mount rides
// the same injectProcedural path as the sword.
render::MeshData makeHorseMesh(f32 shoulderHeight);
const core::Guid& horseMeshGuid();

} // namespace game
