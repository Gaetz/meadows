#pragma once

#include "engine/core/Defines.hpp"

// Terrain generation pipeline — stage S1 (macro synthesis). Headless
// (lib meadows): the bake runs on JobSystem workers or in doctests, never
// on the render side. docs/TERRAIN-GEN.md holds the pipeline overview.
//
// The pipeline consumes CONTROL FIELDS (elevation tier, uplift, sea,
// biome) through the ControlSource seam. Two providers exist by design:
// ProceduralControls (sandbox mode — everything derived from the world
// seed) and painted control maps (scenario/editor mode, later brick). The
// stages downstream never know which one fed them.

namespace render::terraingen {

struct GridSpec {
    f32 originX { 0.0f }; // world meters, min corner
    f32 originZ { 0.0f };
    f32 texelSize { 8.0f };
    u32 n { 0 }; // n x n samples

    f32 x(u32 col) const { return originX + static_cast<f32>(col) * texelSize; }
    f32 z(u32 row) const { return originZ + static_cast<f32>(row) * texelSize; }
    size_t cells() const { return static_cast<size_t>(n) * n; }
};

struct ControlSample {
    f32 tier { 0.0f };   // continuous index into MacroParams::tiers
    f32 uplift { 0.0f }; // [0,1] tectonic uplift strength (stage S2 input)
    bool sea { false };  // this point is open water by decree
    u8 biome { 0 };
};

class ControlSource {
public:
    virtual ~ControlSource() = default;
    virtual ControlSample at(f32 x, f32 z) const = 0;
};

// Sandbox controls: continentalness (warped low-frequency FBM) decides
// sea and elevation tier; a ridged mask decides where ranges rise. Pure
// functions of (seed, x, z) — infinite and deterministic.
struct ProceduralControlParams {
    u32 seed { 1337 };
    // Horizontal rhythm: full vertical scale (peaks above the clouds)
    // packed into hikeable distances — a valley-to-summit walk, not a
    // day trip.
    f32 continentWavelength { 10000.0f };
    f32 seaThreshold { 0.42f }; // continentalness below -> open sea
    f32 tierSpread { 0.30f };   // continentalness span of the tier ramp
    f32 maxTier { 3.0f };
    f32 upliftWavelength { 6500.0f };
    f32 upliftMaskLow { 0.5f };
    f32 upliftMaskHigh { 0.8f };
    f32 warpWavelength { 4000.0f }; // continent-shape domain warp
    f32 warpStrength { 1100.0f };
    // Climate fields deciding the biome id (must match the BiomeForm
    // palette shipped in data: 0 temperate, 1 arid, 2 alpine, 3 tundra).
    f32 climateWavelength { 7000.0f };
};

class ProceduralControls final : public ControlSource {
public:
    explicit ProceduralControls(const ProceduralControlParams& params)
        : p { params } {}
    ControlSample at(f32 x, f32 z) const override;
    f32 continentalness(f32 x, f32 z) const; // [0,1], warped

    const ProceduralControlParams& params() const { return p; }

private:
    ProceduralControlParams p;
};

// One elevation tier: the macro "floors" the artist/controls pick from —
// plains, hills, plateau, highlands by default. Hills can rise from the
// ground OR from a plateau because the relief rides ON the tier altitude.
struct TierLevel {
    f32 altitude { 0.0f };        // meters
    f32 reliefAmplitude { 0.0f }; // +/- meters of macro relief
    f32 reliefWavelength { 500.0f };
    f32 terrace { 0.0f }; // 0..1 strata quantization strength (mesas)
};

// Real-mountain scale: the highest tier + its relief + the fastscape
// orogeny (upliftRate x iterations, minus erosion) put ridge lines at
// ~1300-1700 m — above the cloud layer (600-840 m); the valleys the
// stream power carves into that stay walkable.
struct MacroParams {
    vector<TierLevel> tiers {
        { 40.0f, 18.0f, 420.0f, 0.0f },   // coastal plains
        { 110.0f, 55.0f, 650.0f, 0.0f },  // hills
        { 270.0f, 18.0f, 700.0f, 0.8f },  // mesa plateau
        { 520.0f, 140.0f, 1100.0f, 0.0f }, // high ranges
    };
    f32 seaLevel { 21.0f };
    f32 seaFloor { -30.0f };   // deep-water altitude (absolute meters)
    f32 shallowDepth { 6.0f }; // below seaLevel over the first shelf band
    f32 shelfWidth { 90.0f };  // meters of shallow shelf past the shore
    f32 seaFalloff { 750.0f }; // meters from shore to the deep floor
    f32 shoreWidth { 220.0f }; // beach ramp band on land
    f32 shoreHeight { 0.8f };  // meters above sea level at the waterline
    // Tiers above this keep their altitude to the rim: the coast becomes
    // a sea cliff instead of a beach ramp.
    f32 cliffTierStart { 1.6f };
    f32 cliffTierEnd { 2.4f };
    f32 terraceStep { 40.0f };     // meters between mesa strata
    f32 terraceEdge { 0.16f };     // fraction of a step kept as soft slope
    f32 warpWavelength { 700.0f }; // relief domain warp
    f32 warpStrength { 140.0f };
};

struct MacroResult {
    GridSpec spec;
    vector<f32> height;
    vector<f32> uplift;  // [0,1] per texel, stage S2 input
    vector<f32> seaDist; // signed meters to the sea mask (+ on land)
    vector<u8> biome;
};

// S1: control fields -> macro elevation. Tier-blended base + domain-warped
// relief + soft terracing + coast shelf (beach ramp or cliff rim from the
// tier). Deterministic for (controls, spec, params, seed).
MacroResult synthesizeMacro(const ControlSource& controls,
                            const GridSpec& spec, const MacroParams& params,
                            u32 seed);

// Pointwise approximation of the S1 surface (shore falloff derived from
// continentalness instead of the grid distance field): far silhouettes
// beyond baked tiles and the bake's boundary condition.
f32 macroHeightAnalytic(const ProceduralControls& controls,
                        const MacroParams& params, f32 x, f32 z);

} // namespace render::terraingen
