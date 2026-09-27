#pragma once

#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/terrain/WaterQuery.hpp"
#include "world/spirit/SpiritJets.hpp" // GroundHeightFn
#include "world/spirit/SpiritSources.hpp"

// Chantier ESPRITS — Understand x Water (docs/SPELLS.md §4): what the
// water at a spot IS, read from the same sources gameplay already trusts
// (the unified WaterQuery, the baked bodies, the placed spirit sources).
// Pure computation: the scene formats and displays; nothing here touches
// the world.

namespace world {

struct WaterReading {
    bool water { false };  // water at the aim
    f32 depth { 0.0f };    // metres
    Vec2 flow { 0.0f };    // m/s (x = east, -z = north)

    enum class Body : u8 { None, Sea, Lake, River, Spirit, Pool };
    Body body { Body::None };
    f32 lakeLevel { 0.0f };       // Lake: surface altitude
    f32 lakeArea { 0.0f };        // Lake: m²
    f32 riverWidth { 0.0f };      // River: metres
    f32 riverDischarge { 0.0f };  // River: m³/s (width x depth x speed)
    f32 spiritRemaining { 0.0f }; // Spirit: sim seconds left, < 0 permanent

    // The connected body's volume: exact when the sim's trusted window
    // held all of it, an estimate (lake level over the baked basin)
    // otherwise; unknown when neither applies.
    bool volumeKnown { false };
    bool volumeExact { false };
    f32 volume { 0.0f }; // m³

    // A dry aim: where the nearest water is.
    bool nearestFound { false };
    Vec2 nearestDir { 0.0f };    // unit, from the aim
    f32 nearestDistance { 0.0f }; // metres
};

struct WaterReadingInputs {
    const render::terrain::WaterQuery* query { nullptr };
    const SpiritSourceList* sources { nullptr };
    core::Guid worldspace;
    GroundHeightFn ground;        // for the lake-basin volume estimate
    f32 searchRadius { 512.0f };  // nearest-water search, metres
};

WaterReading readWater(const WaterReadingInputs& in, f32 x, f32 z,
                       f32 groundY);

// The eight compass words for a horizontal direction (x east, -z
// north): "N", "NE", ... — the scene localizes them (dir.<code>).
const char* compassCode(const Vec2& dir);

} // namespace world
