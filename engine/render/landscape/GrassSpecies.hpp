#pragma once

#include "engine/core/Defines.hpp"

namespace render {

// The grass species table — ONE source for the scatter (height, applied
// CPU-side to positionScale.w) and the FrameUbo arrays grass.vert reads
// (width/lean/profile/tints by aSpecies.x). Species are DATA on the same
// blade (the Ghost of Tsushima model): a clump-level pick at scatter, no
// new render system. Journal + selection rules: docs/GRASS-REDO.md.
constexpr u32 kGrassSpeciesCount = 6;

enum GrassSpeciesId : u32 {
    GrassSpecies_Meadow = 0, // tall lush prairie (the historical blade)
    GrassSpecies_Dry = 1,    // short strawy tufts (arid drift, rock fringe)
    GrassSpecies_Oat = 2,    // tall pale wild oats, strongly leaning
    GrassSpecies_Flower = 3, // broad blade, colored tip (flower patches)
    // Micro tier (docs/GRASS-REDO.md P4): the same blade squashed into
    // ground cover — the 0-10 cm layer between the POM texture and the
    // plants. Moss carpets shaded wet forest floor and the worn/dirt
    // ground variants; lichen shares the rock-crevice path with Dry.
    GrassSpecies_Moss = 4,   // low broad rounded shells, deep green
    GrassSpecies_Lichen = 5, // flatter still, pale grey-green
};

// x = height scale (scatter), y = width scale, z = lean scale,
// w = tip profile (1 = rounded near taper, 0 = pointed).
inline const f32 kGrassSpeciesShape[kGrassSpeciesCount][4] = {
    { 1.00f, 1.00f, 1.00f, 1.00f },
    { 0.55f, 0.85f, 0.70f, 0.25f },
    { 1.35f, 0.70f, 1.50f, 0.15f },
    { 0.80f, 1.60f, 0.60f, 1.00f },
    { 0.10f, 4.50f, 0.15f, 1.00f },
    { 0.07f, 3.50f, 0.10f, 1.00f },
};

// Base / tip tints, multiplied over the inherited ground albedo (the one
// color source with the terrain stays intact — species shift it).
inline const f32 kGrassSpeciesBase[kGrassSpeciesCount][3] = {
    { 1.00f, 1.00f, 1.00f },
    { 1.12f, 1.02f, 0.72f },
    { 1.05f, 1.00f, 0.82f },
    { 0.95f, 1.00f, 0.95f },
    { 0.68f, 0.92f, 0.52f },
    { 0.92f, 0.96f, 0.88f },
};
inline const f32 kGrassSpeciesTip[kGrassSpeciesCount][3] = {
    { 1.05f, 1.05f, 0.95f },
    { 1.25f, 1.12f, 0.75f },
    { 1.32f, 1.26f, 0.98f },
    { 1.55f, 1.15f, 1.45f },
    { 0.85f, 1.05f, 0.62f },
    { 1.10f, 1.15f, 1.05f },
};

} // namespace render
