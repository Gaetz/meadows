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

} // namespace game
