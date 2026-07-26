#pragma once

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/combat/Projectile.hpp"

namespace phys {
class PhysicsWorld;
class CharacterBody;
}
namespace gameplay {
class GameplayTagRegistry;
class DerivedStatRegistry;
struct StatsTuningForm;
class EventBus;
class CueRegistry;
}

namespace render {
struct RenderSnapshot;
}

namespace game {

struct Npc;

// What one projectile tick needs — rebuilt per frame by the scene, the
// *Context idiom.
struct ProjectileContext {
    phys::PhysicsWorld* physics;
    const vector<uptr<Npc>>& npcs;
    ecs::Entity playerEntity;
    phys::CharacterBody* player; // null outside Play
    const gameplay::GameplayTagRegistry& gameTags;
    const gameplay::DerivedStatRegistry& derivedStats;
    const gameplay::StatsTuningForm& statsTuning;
    gameplay::EventBus& eventBus;
    gameplay::CueRegistry* cues;
    bool godMode;
};

// Arrows in flight (any shooter: the player's bow, an
// archer NPC, later a trap). Ballistics are sim-pure (gameplay/combat/
// Projectile); this director owns the COLLISION step each frame:
//   - the swept segment raycasts the STATIC world — a hit PLANTS the
//     arrow (the mesh lingers plantedTtl seconds, pickup is P1);
//   - actors are tested analytically (segment vs capsule — they live
//     outside the broadphase), shooter excluded; a hit
//     runs the CAPTURED payload through applyDamage + the usual events
//     and cues.
class ProjectileDirector {
public:
    void spawn(const gameplay::Projectile& projectile) {
        projectiles.push_back(projectile);
    }

    void update(f32 dt, const ProjectileContext& ctx);

    // Arrows as ordinary mesh instances, oriented along their flight.
    void extract(render::RenderSnapshot& out) const;

    void clear() { projectiles.clear(); }
    u32 count() const { return static_cast<u32>(projectiles.size()); }

private:
    vector<gameplay::Projectile> projectiles;
};

} // namespace game
