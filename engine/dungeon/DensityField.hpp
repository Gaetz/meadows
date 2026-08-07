#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/dungeon/SpaceGraph.hpp"

// Stage D3 of the dungeon pipeline (docs/DUNGEON-GEN.md) — the carve field:
// an ANALYTIC signed density over world space, negative inside carved air,
// positive inside rock. Rooms are ellipsoids, corridors are capsule chains
// along the routed grid paths, all warped by 3D fbm for the organic look.
// Being a pure function of position (no stored voxel grid), two neighbouring
// chunks sample identical values at identical coordinates — mesh extraction
// (D4) and nav baking (D5) cannot disagree at seams by construction.

namespace dungeon {

struct DensityParams {
    u32 seed { 1337 };
    f32 tunnelRadius { 2.4f };
    f32 roomHeight { 5.0f };      // vertical diameter of a one-floor room
    f32 noiseAmplitude { 0.9f };  // meters of wall wobble
    f32 noiseWavelength { 7.0f }; // meters per noise cell
};

class DensityField {
public:
    DensityField(const SpaceGraph& graph, const DensityParams& params);

    // Signed density in meters (SDF-like): < 0 is air, > 0 is rock.
    f32 sample(const Vec3& p) const;

    // Central-difference gradient; points from air into rock, so mesh
    // normals are -gradient normalized.
    Vec3 gradient(const Vec3& p) const;

    // Carved volume bounds (noise margin included) — the chunking domain.
    const Vec3& boundsMin() const { return minBounds; }
    const Vec3& boundsMax() const { return maxBounds; }

private:
    // Every primitive is cut by its FLOOR PLANE (max(sd + noise, floor - y)):
    // noise sculpts walls and ceilings only, floors stay exact. Rooms and
    // tunnels of one slot share the slot's floor height, so junctions have
    // no lip — which is what keeps the nav grid's step limit satisfiable
    // (curved noisy floor unions create >0.5 m ledges at every mouth).
    // Ramps get a plane sloping along their axis; vertical shafts get no
    // floor at all (the drop stays the intended one-way).
    struct Ball {
        Vec3 center;
        Vec3 radii;
        f32 floorY { 0.0f };
    };
    struct Pipe {
        Vec3 a;
        Vec3 b;
        f32 radius { 0.0f };
        f32 floorA { 0.0f };
        f32 floorB { 0.0f };
    };

    vector<Ball> balls;
    vector<Pipe> pipes;
    DensityParams params;
    Vec3 minBounds { 0.0f };
    Vec3 maxBounds { 0.0f };
};

} // namespace dungeon
