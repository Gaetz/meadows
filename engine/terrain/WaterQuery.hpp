#pragma once

#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterSim.hpp"

namespace render::terrain {

// R3 — the ONE gameplay water sample (docs/WATER-RENDER.md): what you
// SEE is what you swim in. Inside the sim's trusted rect the SIM
// snapshot is authoritative — its water counts, and its dryness
// counts too (the baked bodies claiming water under a hillside gave
// blue screens in galleries under lakes). Everywhere else — outside
// the rect, no snapshot yet, window settling behind the baked display
// — the BAKED bodies answer, so the query is never absent. The sea is
// handled explicitly in the authority zone (sim publishes sea cells
// dry by doctrine; the analytic sheet still swims).
struct WaterQuery {
    // Null = baked only (pre-roll, settling, sim disabled). The
    // caller decides — pass the snapshot ONLY when the display shows
    // the sim (WaterSystem::simIsValid() && !simIsSettling()).
    const WaterSimSnapshot* sim { nullptr };
    const WaterBodies* bodies { nullptr };
    f32 seaLevel { -1.0e6f };
};

// The highest plausible water surface over (x, z) for a probe at
// probeY — the drop-in replacement for waterSurfaceAt (same
// plausibility contract: a probe far above the water, or far below
// the sim column's bottom, reads dry). nullopt = dry.
std::optional<f32> waterSurfaceQuery(const WaterQuery& q, f32 x, f32 z,
                                     f32 probeY);

// XZ current at the point (m/s) — the drop-in replacement for
// waterFlowAt. The sim's depth-averaged velocity inside the rect
// (the current you SEE), the baked river blend outside. Zero when
// dry, zero on still water.
Vec2 waterFlowQuery(const WaterQuery& q, f32 x, f32 z, f32 probeY);

} // namespace render::terrain
