#pragma once

#include "data/forms/Form.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/reflect/Reflect.hpp"

// The shared scene representation (Phase 2, brick d's data; spawned by brick b).
// These are plain reflected structs — no rhi/render types — so the world stays
// render-free (§4) and the renderer observes them from above. flecs does not
// appear here; only `registerSceneComponents` (in the .cpp) touches the ECS.

namespace ecs {
class World;
}

namespace world {

// Dimension-agnostic transform (§2.6): 3D-ready so gameplay survives the 2D→3D
// move unchanged. The 2D phase simply projects (position.xy, yaw-from-quat).
struct Transform {
    Vec3 position { 0.0f, 0.0f, 0.0f };
    Quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
    Vec3 scale { 1.0f, 1.0f, 1.0f };

    REFLECT_BEGIN(Transform, void)
        REFLECT_FIELD(position)
        REFLECT_FIELD(rotation)
        REFLECT_FIELD(scale)
    REFLECT_END()
};

// Holds an asset handle, never pixels or GPU data (§7). The render bridge
// (brick d) resolves `sprite` to a texture; streaming may spawn this before the
// asset is resident, so the bridge draws a placeholder until it is.
struct SpriteRender {
    core::Guid sprite;
    Vec2 size { 1.0f, 1.0f };
    Vec4 tint { 1.0f, 1.0f, 1.0f, 1.0f };
    i32 layer { 0 }; // painter order (no depth buffer in 2D)
    // Sub-region of the texture to draw (u0, v0, u1, v1). Default = the whole
    // texture. Lets one atlas / sprite-sheet hold many frames (e.g. the 8
    // directional facings of a character); the renderer samples only this rect.
    Vec4 uvRect { 0.0f, 0.0f, 1.0f, 1.0f };

    REFLECT_BEGIN(SpriteRender, void)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(size)
        REFLECT_FIELD(tint)
        REFLECT_FIELD(layer)
        REFLECT_FIELD(uvRect) // appended last: binary ordinals stay stable
    REFLECT_END()
};

// Planar velocity in world units/second, integrated into Transform by the
// movement system (`applyMovement`). Reflected so it can serialize later.
struct Velocity {
    Vec3 value { 0.0f, 0.0f, 0.0f };

    REFLECT_BEGIN(Velocity, void)
        REFLECT_FIELD(value)
    REFLECT_END()
};

// A 2D axis-aligned box for collision. Solid colliders block movement; trigger
// colliders only report overlaps (the §3 "simple custom 2D collision").
struct Collider {
    Vec2 halfExtents { 0.5f, 0.5f };
    bool trigger { false };

    REFLECT_BEGIN(Collider, void)
        REFLECT_FIELD(halfExtents)
        REFLECT_FIELD(trigger)
    REFLECT_END()
};

// Links an entity back to its source records. `referenceId` is the persistent
// identity (the ReferenceForm guid) that saves key on; `base`/`cell` are runtime
// handles, deliberately NOT reflected (handles never persist — §2.5).
struct RefId {
    core::Guid referenceId;
    data::FormHandle base;
    data::FormHandle cell;

    REFLECT_BEGIN(RefId, void)
        REFLECT_FIELD(referenceId)
    REFLECT_END()
};

// Zero-size ECS marker tags stamped by the per-category spawner, so systems can
// query "all actors", "all items"... These are runtime ECS markers (flecs
// tags), NOT GameplayTags (§6, which are reflected moddable data).
struct StaticMarker {};
struct ItemMarker {};
struct ActorMarker {};

// Registers the scene components in the ECS (flecs storage + our reflected-
// component registry). Call once per World before spawning.
void registerSceneComponents(ecs::World& world);

} // namespace world
