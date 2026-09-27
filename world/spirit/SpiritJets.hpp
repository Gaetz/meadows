#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/SpiritField.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"

// Chantier ESPRITS: jets — a spirit STREAMED from the caster along a
// ballistic arc, simulated where it lands. A jet is transient (it is the
// caster's gesture, like an arrow in flight): what persists is only what
// the field kept — for water, nothing (sim water is never saved; springs
// are). Owned by the scene's SpiritDirector, main thread only; the water
// kernel receives the landing spots as plain sources at job enqueue.

namespace world {

using GroundHeightFn = std::function<f32(f32 x, f32 z)>;

struct JetLanding {
    Vec3 point { 0.0f };
    f32 flightSeconds { 0.0f };
};

// Where an arc launched from `origin` at `velocity` meets the ground: the
// arc is stepped at `step` seconds until it dips under the terrain, then
// bisected once for a clean spot. nullopt when it is still flying after
// `maxSeconds` (off a cliff, over the sea rim).
std::optional<JetLanding> jetLanding(const Vec3& origin, const Vec3& velocity,
                                     f32 gravity, const GroundHeightFn& height,
                                     f32 maxSeconds = 6.0f,
                                     f32 step = 1.0f / 30.0f);

struct SpiritJet {
    render::terrain::SpiritKind kind { render::terrain::SpiritKind::Water };
    Vec3 origin { 0.0f };   // the nozzle (hand / eye)
    Vec3 velocity { 0.0f }; // m/s at the nozzle
    f32 rate { 0.0f };      // kind-specific per second (water: m³/s)
    f32 radius { 2.0f };    // metres at the landing spot
    f32 remaining { 0.0f }; // SIM seconds (rate x duration is invariant)
    bool followsCaster { true }; // re-aimed every frame from the caster
    // A channeled jet lives while its key is held (the scene ends it on
    // release) and re-pays its ability cost every costPeriod seconds.
    bool channeled { false };
    f32 costPeriod { 1.0f };
    f32 costClock { 0.0f };
    core::Guid ability;     // the AbilityForm whose cost the upkeep re-pays
    u32 emitter { 0 };      // presentation handle (the scene's particle
                            // emitter), 0 = none
    std::optional<JetLanding> landing;
};

class SpiritJetList {
public:
    static constexpr size_t kMaxJets = 4;
    static constexpr f32 kGravity = 9.81f;

    // Starts a jet; the OLDEST yields when full. Returns that evicted
    // jet's emitter handle (0 = nothing to stop).
    u32 start(SpiritJet jet);
    // Advances lifetimes; expired jets leave, their emitter handles are
    // appended to `stopped`. True when the set changed.
    bool tick(f32 simSeconds, vector<u32>* stopped = nullptr);
    // Re-aims the follower jets: same speed, the caster's current nozzle
    // and forward.
    void aim(const Vec3& origin, const Vec3& forward);
    // Recomputes every landing against the ground (call after aim).
    void resolveLandings(const GroundHeightFn& height);
    // The kernel view: one disc per landed WATER jet.
    void appendWaterSources(vector<render::terraingen::WaterSource>& out) const;

    const vector<SpiritJet>& entries() const { return list; }
    vector<SpiritJet>& entriesMut() { return list; } // channel upkeep
    bool empty() const { return list.empty(); }
    vector<u32> clear(); // returns the emitter handles to stop

private:
    vector<SpiritJet> list;
};

} // namespace world
