#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// Chantier ESPRITS E4 — the wind as a FIELD: one global wind (the
// weather's direction and strength, the same the grass, trees, clouds,
// mist and rain read from the frame UBO) plus transient GUSTS placed by
// the wind spirit. Analytic, no grid: windAt(x, z) is the sum. Headless,
// deterministic.

namespace render::terrain {

struct WindGust {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 dirX { 1.0f }; // unit direction the gust blows toward
    f32 dirZ { 0.0f };
    f32 speed { 0.0f };  // m/s at the centre
    f32 radius { 8.0f }; // metres, smooth falloff to the edge
    f32 remaining { 0.0f }; // seconds; <= 0 = expired
};

struct WindField {
    Vec2 globalDir { 1.0f, 0.0f }; // unit, the direction the wind blows toward
    f32 globalSpeed { 0.0f };      // m/s
    vector<WindGust> gusts;

    // The wind at a point (m/s, XZ): global + every live gust within reach.
    Vec2 windAt(f32 x, f32 z) const;
    // Ages the gusts; expired ones go.
    void tick(f32 dt);
};

// The wind direction the weather authors, in compass degrees the wind
// BLOWS TOWARD: 0 = east (+x), 90 = north (-z), counter-clockwise from
// above — the compass of the readings (WaterReading::compassCode).
Vec2 windDirectionFromDegrees(f32 degrees);

} // namespace render::terrain
