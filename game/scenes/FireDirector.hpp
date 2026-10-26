#pragma once

#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "engine/audio/Audio.hpp"
#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"       // ecs::Entity, flecs::query
#include "engine/fx/Particles.hpp"
#include "game/scenes/AmbientFxController.hpp" // Igniter
#include "game/scenes/StreamingController.hpp" // WorldPolicy
#include "gameplay/cue/GameplayCues.hpp"
#include "world/scene/Components.hpp" // world::Transform, RefId
#include "world/spirit/SpiritFire.hpp" // world::TreeFire

namespace core {
class JobSystem;
}
namespace data {
class FormDatabase;
}
namespace gameplay {
class DerivedStatRegistry;
class GameplayTagRegistry;
struct StatsTuningForm;
}
namespace phys {
class CharacterBody;
}
namespace render {
struct RenderSnapshot;
class WorldRenderer;
struct WaterBodies;
namespace terrain {
struct WindField;
}
}
namespace rhi {
class Device;
}

namespace game {

struct Npc;
class SoundResolver;
class SpiritDirector;

// The scene systems the fire's presentation reads and writes, bundled
// so the flames, the burning props and trees, the mask upload and the
// fire lights are decoupled from LandscapeScene. The scene rebuilds it
// each call from its own members — references, a few scalars, and the
// one action that stays scene territory as a closure (the
// InteractionContext pattern). Mirrors AmbientFxContext.
//
// What the spirit gestures share with the fire is the SpiritDirector's
// and arrives here by reference: the flame jet's touched set (the contact
// pass reads it), the ward (the globe), the wind field (the weather's
// plus the gusts); the igniters are the ambient torches'.
struct FireContext {
    SpiritDirector& spirits;              // the fire lane, the field reads, the ignitions
    const data::FormDatabase& forms;      // ParticleForms, StaticForms, EffectForms
    fx::ParticleSim& fxSim;
    gameplay::CueRegistry& cues;
    audio::AudioSystem& audio;
    SoundResolver& sounds;
    render::WorldRenderer& renderer;      // water sim, scatter props, scorch map
    const render::TerrainParams& terrainParams; // the scene's world params (ground heights, sea level)
    rhi::Device& device;                  // handed to the renderer's scorch map only
    core::JobSystem& jobs;                // the fire lane's worker
    flecs::query<const world::Transform, const world::RefId>& interactQuery; // the wooden props
    const vector<uptr<Npc>>& npcs;        // who stands in the fire
    ecs::Entity playerEntity;
    phys::CharacterBody* playerBody;      // null outside Play
    bool playMode;                        // mode == Play
    bool casterWarded;                    // the fire ward is up: no flame reaches the caster
    const std::unordered_set<u64>& jetTouched; // actors in the flame jet this frame
    const vector<AmbientFxController::Igniter>& igniters;
    const render::terrain::WindField& wind;
    sptr<const render::WaterBodies> waterBodies; // the baked bodies (the WaterQuery contract)
    f32 rainIntensity;                    // the weather's rain 0..1
    Vec3 cameraPosition;
    Vec3 cameraForward;
    WorldPolicy policy;                   // allowsSpirits gates contact and igniters
    const gameplay::GameplayTagRegistry& tags;
    const gameplay::DerivedStatRegistry& derived;
    const gameplay::StatsTuningForm& statsTuning;
    // A prop burnt to the ground: its static collider and its reference
    // leave the world for good (the save layer disables it, like a
    // picked-up item). The director destructs the entity afterwards.
    std::function<void(ecs::Entity prop)> retireProp;
};

// The fire's presentation (docs/FIRE-RENDER.md) over the field the
// SpiritDirector's lane simulates: the per-frame job kick, the scorch
// mask upload when a job lands, the flame emitters re-placed on the
// burning cells nearest the camera under a budget (the Far Cry 2 "hair
// transplant"), the field's 3D loop, the fire lights aggregated per
// tile, and the fire's bite on the world — actors standing in it take
// the spirit's contact effect (§2.9: the ONLY path to their
// attributes), wooden props heat up, burn their fuel and are gone,
// trees heat, burn and regrow (never removed: burnt is a transient
// state). Main thread only; the GPU is reached through the renderer.
class FireDirector {
public:
    // Once per sim tick, inside the spirit block (after the sources
    // ticked). `simSeconds` is the SIM dt.
    void update(const FireContext& ctx, f32 simSeconds);
    // The frame's extraction, after the world's lights: the front's
    // cells aggregated per tile into point lights, inserted ahead of
    // the world's in the snapshot (the clustered path and the GI take
    // them like any other).
    void extractLights(const FireContext& ctx, render::RenderSnapshot& out);
    // Per frame, after the ambient update rebuilt the igniters: their
    // heat goes to the ground under a low flame, to the wooden props
    // within reach, and wakes the lane for the trees.
    void applyIgniters(const FireContext& ctx, f32 dt);
    // Map swap / clear: window, mask, flames, props, trees and the loop go.
    void reset(const FireContext& ctx);

    // The pure placement (no scene access): the burning cells the camera
    // can SEE, nearest and most in view first, under budget — emitters
    // behind the camera are wasted (beyond a small radius, so turning
    // around never shows a gap). Beyond kFlameLodNear one anchor stands
    // for a 2x2 block of cells. Returns the anchors in score order.
    static vector<Vec2> placeFlames(const vector<Vec2>& burning,
                                    const Vec3& cam, const Vec3& forward);

    static constexpr f32 kFlameHazeNear = 30.0f; // shimmer within this
    static constexpr u32 kMaxFlames = 96;
    static constexpr f32 kFlameReach = 120.0f;
    static constexpr f32 kFlameBehindRadius = 12.0f; // behind the camera, flames only this close
    // Beyond this, one flame stands for a 2x2 block of cells (bigger,
    // sparser): the far front costs a quarter of the emitters.
    static constexpr f32 kFlameLodNear = 40.0f;
    static constexpr f32 kFireSoundReach = 60.0f;
    // The lights' tile (docs/FIRE-RENDER.md F3): the front's cells
    // aggregated per 8 m into one point light each.
    static constexpr f32 kFireLightTile = 8.0f;
    static constexpr f32 kPropHeatRate = 0.4f; // per second in full fire
    static constexpr f32 kTreeFireReach = 160.0f;

private:
    struct FlameEmitter {
        Vec2 at { 0.0f }; // the cell center (exact: matched by equality)
        u32 emitter { 0 }; // the flames
        u32 sparks { 0 };  // the embers it sheds
        u32 smoke { 0 };   // one cell in four smokes (discreet)
        u32 haze { 0 };    // the heat shimmer, near cells only
    };
    // A wooden prop alight: burns its material's fuel, lights the ground
    // around it once, then is gone for good.
    struct BurningProp {
        ecs::Entity entity;
        f32 remaining { 0.0f };
        u32 flames { 0 };
        u32 sparks { 0 };
        u32 smoke { 0 };
        bool lit { false }; // the ground around it took its spark
    };
    // One state per tree of the scatter (keyed by its quantized base),
    // heated by the burning cells around its trunk, burning with flames,
    // its canopy going through the mask's B channel (the tree shader's
    // leaf fall), regrowing after.
    struct TreeFireEntry {
        Vec3 at { 0.0f };
        f32 scale { 1.0f };
        world::TreeFire state;
        u32 flames { 0 };      // the trunk and branches
        u32 crownFlames { 0 }; // the canopy
        u32 sparks { 0 };
        u32 smoke { 0 };
        f32 emberClock { 0.0f };
    };

    static u64 treeKey(const Vec3& at) {
        const i64 x = static_cast<i64>(std::llround(at.x * 4.0f));
        const i64 z = static_cast<i64>(std::llround(at.z * 4.0f));
        return (static_cast<u64>(x) << 32) ^ (static_cast<u64>(z) & 0xffffffffu);
    }

    // Actors standing on a burning cell take the contact effect every
    // contactPeriod; `inFire` accumulates the clock, then the buildup
    // and the typed damage land (through applyDamage).
    void applyContact(const FireContext& ctx, f32 dt);
    void touch(const FireContext& ctx, ecs::Entity entity, f32& clock, f32 dt,
               bool inFire);
    // Per landed job: the wooden props in the glow heat up and catch.
    void updateProps(const FireContext& ctx);
    // A wooden prop catches: its flames, sparks and smoke — shared by the
    // fire's own heat and an igniter's.
    void lightProp(const FireContext& ctx, ecs::Entity entity,
                   const Vec3& position, f32 fuel);
    // Per landed job: the trees' exposure, phases and emitters, and the
    // canopy mask composed from their burn.
    void updateTrees(const FireContext& ctx, f32 landDt);
    // The field's 3D loop: one source following the burning cell nearest
    // the camera, silent beyond reach.
    void updateSound(const FireContext& ctx, const Vec3& cam);

    vector<FlameEmitter> flameEmitters;
    audio::AudioSystem::SoundId fireLoop { 0 };
    f32 playerFireClock { 0.0f };
    std::unordered_map<u64, f32> npcFireClocks;
    std::unordered_map<u64, f32> propFireHeat; // entity id -> heat 0..1
    vector<BurningProp> burningProps;
    std::unordered_map<u64, TreeFireEntry> treeFires;
    vector<u8> fireCanopy; // the mask's B channel, composed per landed job
    f32 fireLandClock { 0.0f }; // sim seconds since the last landed job
    // The igniters' tree check, once a second: a tree in a torch's flame
    // needs the lane awake (updateTrees reads the igniters).
    f32 igniterTreeClock { 0.0f };
    bool igniterNearTree { false };
};

} // namespace game
