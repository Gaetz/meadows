#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/anim/Anim.hpp"          // anim::GraphDesc/GraphInstance/Pose/Skeleton
#include "engine/assets/GltfMesh.hpp"    // assets::GltfClip
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"          // ecs::Entity, ecs::World
#include "engine/rhi/Rhi.hpp"            // rhi::*Handle
#include "gameplay/ability/GameplayTags.hpp" // gameplay::GameplayTag

namespace rhi {
class Device;
}
namespace engine {
struct FrameContext;
}
namespace render {
struct TerrainParams;
class ShaderLibrary;
}
namespace data {
class FormDatabase;
struct WeaponForm;
}
namespace assets {
class AssetDatabase;
}
namespace phys {
class PhysicsWorld;
class CharacterBody;
}
namespace world {
class TerrainNavigator;
}
namespace gameplay {
class DerivedStatRegistry;
struct StatsTuningForm;
class EventBus;
struct GameClock;
class FurnitureOccupancy;
struct AiPackageForm;
} // namespace gameplay

namespace game {

// One rig cache entry per glTF asset (skeleton + its clips). Shared by every
// NPC built from that asset.
struct RigData {
    anim::Skeleton skeleton;
    vector<assets::GltfClip> clips;
};

// Per-NPC runtime state (non-reflected, §H5). uptr in the owning vector: the
// GraphInstance references Npc::graph — addresses must survive vector growth.
// Mixes render state (buffers, palette, pose) with AI state (schedule, path,
// combat) — a known tangle (audit U4-13) kept together for now.
struct Npc {
    ecs::Entity entity;
    const RigData* rig { nullptr };
    anim::GraphDesc graph; // owns the clips; `anim` references it
    uptr<anim::GraphInstance> anim;
    anim::Pose pose;
    vector<Mat4> palette;
    Vec4 tint { 1.0f };
    rhi::BufferHandle vertices {};
    rhi::BufferHandle indices {};
    u32 indexCount { 0 };
    rhi::BufferHandle paletteSsbo {};
    rhi::BufferHandle modelUbo {};
    rhi::BindGroupHandle group {};
    rhi::BindGroupHandle casterGroup {}; // B2a: ubo b4 + palette b2
    // Patrol: walk to patrolPoints[target], pause, swap ends.
    u32 target { 0 };
    f32 pauseTimer { 0.0f };
    f32 yaw { 0.0f };
    f32 speed { 0.0f }; // smoothed horizontal speed -> anim param

    // Chantier 3 B3: schedule-driven life (replaces the patrol when the
    // ActorForm carries a schedule; patrol stays the fallback).
    core::Guid schedule {};
    i32 lastEvaluatedSlot { -1 }; // 10-game-minute re-eval granularity
    const gameplay::AiPackageForm* activePackage { nullptr };
    core::Guid activeLocation {};
    str intentReason; // the debug view's "why"
    vector<Vec3> path;
    u32 pathIndex { 0 };
    f32 repathTimer { 0.0f };
    f32 wanderTimer { 0.0f };
    bool sitting { false };         // drives the State.Sitting anim gate
    bool furnitureClaimed { false };

    // Chantier 3 B5/B6: combat.
    bool hostile { false }; // ActorTagForm child "Faction.Bandits"
    bool guard { false };   // D2: "Faction.VillageGuard" — hostile while Wanted
    bool dead { false };    // mirrors the GAS State.Dead tag
    f32 attackCooldown { 0.0f };
    // Chantier 6 A1: the first Faction.* tag — what the OnDeath event carries
    // (quest kill filters, crime factions).
    gameplay::GameplayTag factionTag {};
};

// The scene systems the NPC subsystem touches, bundled so the whole NPC
// director (build / AI / schedule / combat / draw) is decoupled from
// LandscapeScene (audit U4-10). The scene rebuilds it each call from its own
// members — cheap: references plus a few scalars/handles. Mirrors
// EditorContext / StreamingContext.
struct NpcContext {
    ecs::World& world;
    data::FormDatabase& forms;
    assets::AssetDatabase& assetDb;
    const render::TerrainParams& terrainParams;
    gameplay::GameplayTagRegistry& gameTags;
    const gameplay::DerivedStatRegistry& derivedStats;
    const gameplay::StatsTuningForm& statsTuning;
    gameplay::EventBus& eventBus;
    gameplay::GameClock& gameClock;
    gameplay::FurnitureOccupancy& furnitureOccupancy;
    world::TerrainNavigator* navigator;
    phys::PhysicsWorld* physics;
    ecs::Entity playerEntity;
    phys::CharacterBody* player;
    bool playMode;                     // mode == Play (combat hunts the player)
    const data::WeaponForm* banditWeapon;
    bool godMode;
    f32 timeSeconds;                   // cosmetic wander hash (not gameplay RNG)
    // GPU (refresh builds skins; draw binds them):
    rhi::TextureHandle whiteTexture;
    rhi::SamplerHandle meshSampler;
    render::ShaderLibrary& shaders;
    rhi::BindGroupHandle frameBindGroup;
};

// The whole Forms-driven NPC subsystem, extracted from LandscapeScene (audit
// U4-10): owns the rig cache, the NPC list, and the skinned pipeline; builds
// newcomers on cell changes, runs their AI/schedule/combat each frame, and
// draws them. The scene still reads the list (npcs()) for player attack/crime,
// the shadow caster pass, the debug UI, the editor pick and the console.
class NpcDirector {
public:
    // Cell streaming makes NPC entities come and go: prune dead ones (freeing
    // their GPU state) and build newcomers. finalizeActorSpawn adds the stats /
    // saved-state / loadout components (shared with the player — stays in the
    // scene), applied deferred (a table move on the locked iteration).
    void refreshNpcs(
        rhi::Device& device, const NpcContext& ctx,
        const std::function<void(ecs::Entity, const core::Guid&)>&
            finalizeActorSpawn);

    // Per frame: character tick, schedule, path, combat, anim pose.
    void update(f32 dt, const NpcContext& ctx);

    // Opaque pass: one skinned draw per NPC (builds the pipeline on demand).
    void draw(engine::FrameContext& frame, const NpcContext& ctx);

    // onExit teardown: destroy every NPC's GPU state, the pipeline, and drop
    // the caches, so a re-enter starts clean.
    void teardown(rhi::Device& device);

    // The scene reads/mutates the list directly (shadow caster pass creates
    // per-NPC caster groups; player attack / crime / UI iterate it).
    std::vector<uptr<Npc>>& npcs() { return npcs_; }
    const std::vector<uptr<Npc>>& npcs() const { return npcs_; }
    Vec3 characterSpot() const { return characterSpot_; } // first NPC (teleport)

private:
    const RigData* loadRig(const NpcContext& ctx, const core::Guid& asset);
    void destroyNpc(rhi::Device& device, Npc& npc);
    void updateNpcSchedule(const NpcContext& ctx, Npc& npc, f32 hourOfDay);
    bool moveNpcAlongPath(const NpcContext& ctx, Npc& npc, f32 dt,
                          f32 speedScale);
    void buildSkinnedPipeline(rhi::Device& device, render::ShaderLibrary& shaders);

    std::unordered_map<core::Guid, RigData> rigCache;
    vector<uptr<Npc>> npcs_;
    vector<Vec3> patrolPoints;   // grounded "patrol" marker positions
    Vec3 characterSpot_ { 0.0f }; // first NPC position (teleport target)
    rhi::PipelineHandle skinnedPipeline {};
    u64 skinnedShaderGeneration { 0 };
};

} // namespace game
