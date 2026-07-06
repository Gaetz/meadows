#pragma once

#include <functional>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/anim/Anim.hpp"

// Forms -> anim runtime mapping (horizontal pass H5). The engine's anim
// module consumes plain structs (rule n°2 of the pass: engine never sees
// data::); THIS is where AnimGraphForm + its child states/transitions and
// AnimClipForm + events become an anim::GraphDesc.
//
// Clips live in glTF assets: the caller provides the resolver (runtime
// side — it owns the asset database and caching). The bridge itself is
// pure and headless-testable with a fake resolver.
//
// HOW TO FILL (post-7/07): cache GraphDescs by graph guid (they are
// immutable per resolve); one GraphInstance per animated entity, stored
// in a runtime component; entity speed and tags flow in per frame.

namespace data {
struct ActorForm;
}

namespace world {

// (assetGuid, animationName) -> clip. Return nullopt on failure; the
// bridge then skips the state (logged) instead of failing the graph.
using ClipResolver = std::function<std::optional<anim::AnimClip>(
    const core::Guid& asset, const str& animationName)>;

std::optional<anim::GraphDesc> buildAnimGraph(
    const data::FormDatabase& forms, const core::Guid& graphId,
    const ClipResolver& resolveClip);

// Actor visual resolution (chantier 1, B6): ActorForm.appearance ->
// what to draw. v1 picks the FIRST filled slot mesh (a single body mesh);
// full per-slot compositing (equipment swaps a slot's mesh) is the
// EquipmentVisuals vertical. nullopt = no 3D visual (2D/legacy actor).
struct ActorVisual {
    core::Guid skeleton;  // glTF asset carrying the rig (and its clips)
    core::Guid mesh;      // skinned body mesh asset
    Vec4 tint { 1.0f };   // AppearanceForm.skinTint
    core::Guid animGraph; // ActorForm.animGraph
};
std::optional<ActorVisual> resolveActorVisual(const data::FormDatabase& forms,
                                              const data::ActorForm& actor);

} // namespace world
