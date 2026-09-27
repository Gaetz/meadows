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

// One lump of the stream in flight: launched from the nozzle with the
// jet's velocity of that instant, it flies its own arc and lands after
// `flightSeconds` (the landing resolved at launch).
struct JetSphere {
    Vec3 origin { 0.0f };
    Vec3 velocity { 0.0f };
    f32 age { 0.0f };
    f32 flightSeconds { 0.0f };
    f32 radius { 0.3f };
    Vec3 at(f32 gravity) const {
        return Vec3 { origin.x + velocity.x * age,
                      origin.y + velocity.y * age - 0.5f * gravity * age * age,
                      origin.z + velocity.z * age };
    }
};

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
    f32 upkeepScale { 0.25f }; // fraction of the activation cost per upkeep
    core::Guid ability;     // the AbilityForm whose cost the upkeep re-pays
    u32 emitter { 0 };      // presentation handle (the scene's particle
                            // emitter), 0 = none
    std::optional<JetLanding> landing;
    // The stream as lumps: launched every sphereInterval, each flying
    // its own arc (a swept aim leaves a trail, not a jump).
    vector<JetSphere> spheres;
    f32 sphereClock { 0.0f };
};

// A HELD volume (the control spell): while the key is held the caster
// draws the element out of the world at the aim and carries it as a
// floating volume; releasing drops it where the aim is. Transient like
// a jet. The kernel does not report what it removed, so the volume is
// accounted from the draw rate while the element is present at the aim.
struct SpiritHold {
    render::terrain::SpiritKind kind { render::terrain::SpiritKind::Water };
    f32 rate { 0.0f };       // element units drawn per second (water: m³/s)
    f32 radius { 2.0f };     // metres of the draw / drop footprint
    f32 volume { 0.0f };     // carried so far
    f32 maxVolume { 0.0f };  // rate x duration: the spell's capacity
    f32 costPeriod { 1.0f };
    f32 costClock { 0.0f };
    f32 upkeepScale { 0.25f };
    core::Guid ability;      // upkeep re-pays this ability's cost
    u32 emitter { 0 };       // the floating blob's particle emitter
    std::optional<Vec3> aim; // where it draws / where it will drop

    // Accounts one frame of drawing; `present` = the element sits at
    // the aim. True while the hold still has capacity to draw.
    bool absorb(f32 dt, bool present);
    // The discharge that drops the carried volume over `seconds`.
    f32 dropDischarge(f32 seconds) const;
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
    // Launches a sphere per `interval` from each jet's current nozzle and
    // velocity (radius from the volume it carries: rate x interval),
    // ages every sphere by dt, drops the landed ones — their landing
    // points go to `landed` (the scene splashes there). Call after
    // resolveLandings (a launch takes the current flight time).
    void advanceSpheres(f32 dt, f32 interval, f32 gravity,
                        vector<Vec3>* landed = nullptr);
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
