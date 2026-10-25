#pragma once

#include <unordered_map>

#include "engine/audio/Audio.hpp"
#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"       // ecs::Entity, flecs::query
#include "engine/fx/Particles.hpp"
#include "game/scenes/StreamingController.hpp" // WorldPolicy
#include "world/scene/Components.hpp" // world::Transform, FxSource

namespace data {
class FormDatabase;
}
namespace render {
struct TerrainParams;
struct WaterBodies;
namespace terrain {
struct WindField;
}
}

namespace game {

class SoundResolver;
class SpiritDirector;

// The scene systems the ambient dressing reads, bundled so the torches
// and the river fx are decoupled from LandscapeScene. The scene rebuilds
// it each call from its own members (references and a few scalars) —
// the InteractionContext pattern. Mirrors FireContext.
struct AmbientFxContext {
    const data::FormDatabase& forms;      // ParticleForms, RiverSourceFxForm
    fx::ParticleSim& fxSim;
    audio::AudioSystem& audio;
    SoundResolver& sounds;
    flecs::query<const world::Transform, world::FxSource>& fxSourceQuery;
    const SpiritDirector& spirits;        // the ground alight relights a torch
    const render::terrain::WindField& wind;      // a gust snuffs a torch
    const render::TerrainParams& terrain;        // ground under a fall
    const render::WaterBodies* waterBodies;      // the published courses (null = none)
    WorldPolicy policy;                   // hasWeather, hasWaterBodies
    Vec3 cameraPosition;
    f32 rainIntensity;                    // a downpour snuffs a torch
};

// The ambient fx that are not a spirit's: the placed flames
// (world::FxSource — torches, campfires) with their smoke, haze, loop
// and light, put out by water, wind or rain and relit by fire; the mist
// and murmur at the heads of the published rivers; the spray and roar
// at the foot of their falls. One emitter set per spot while it lives
// within reach of the camera, started and stopped by a reach/diff sweep.
// Main thread only; the particle sim and the audio system are the sinks.
class AmbientFxController {
public:
    // The torches that heat their surroundings (FxSource.igniteHeat > 0),
    // rebuilt by every update() from the LIT sources in reach: the fire
    // director feeds their heat to the wooden props and trees around them
    // and, when the flame stands low, to the ground.
    struct Igniter {
        Vec3 at { 0.0f };
        f32 radius { 0.0f };
        f32 heat { 0.0f };
        bool ground { false };
    };
    static constexpr f32 kSourceReach = 90.0f;

    // Per frame, after the particle sim advanced: the torches, then the
    // river heads, then the falls (each a reach/diff sweep). Consumes the
    // ground events of the frame.
    void update(const AmbientFxContext& ctx);
    // Map swap / clear: the torch emitters go with the world (the river
    // dressing sweeps itself on the next update, its keys being the
    // courses'), the pending events and the igniters too.
    void reset(const AmbientFxContext& ctx);

    // The frame's water (douses, jet lumps, splashes, springs) and fire
    // (sparks, brands, the jet's pulses) events, as discs on the ground:
    // what snuffs a torch and what relights it. Consumed by update().
    void noteWaterEvent(const Vec2& at, f32 radius) {
        waterEvents.push_back({ at, radius });
    }
    void noteFireEvent(const Vec2& at, f32 radius) {
        fireEvents.push_back({ at, radius });
    }

    const vector<Igniter>& igniters() const { return igniterList; }
    // Is `at` within an igniter's reach (plus half a metre of slack).
    static bool nearIgniter(const vector<Igniter>& igniters, const Vec3& at);
    bool nearIgniter(const Vec3& at) const { return nearIgniter(igniterList, at); }

private:
    struct Emitters {
        u32 flame { 0 };
        u32 smoke { 0 };
        u32 haze { 0 };
        audio::AudioSystem::SoundId sound { 0 };
    };
    struct GroundEvent {
        Vec2 at { 0.0f };
        f32 radius { 0.0f };
    };

    void updateSources(const AmbientFxContext& ctx);
    void updateRiverSources(const AmbientFxContext& ctx);
    void updateRiverCascades(const AmbientFxContext& ctx);
    void snuffSource(const AmbientFxContext& ctx, ecs::Entity entity,
                     world::FxSource& source, const Vec3& at);

    std::unordered_map<u64, Emitters> sourceEmitters;       // by entity id
    std::unordered_map<u64, Emitters> riverSourceEmitters;  // by quantized head
    std::unordered_map<u64, Emitters> riverCascadeEmitters; // by quantized foot
    vector<GroundEvent> waterEvents;
    vector<GroundEvent> fireEvents;
    vector<Igniter> igniterList;
};

} // namespace game
