#include <chrono>
#include <cstdio>

#include "engine/core/Log.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "engine/terrain/generation/MapHydrology.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

#include <glm/glm.hpp>

#include "engine/core/Assert.hpp"

namespace render::terraingen {

namespace {

// Snap a world span to a whole texel count (grids must land exactly on
// texel corners for the crop indexing below).
u32 texels(f32 meters, f32 texel) {
    return static_cast<u32>(std::lround(meters / texel));
}

GridSpec simSpecFor(const TileBakeParams& params, i32 tx, i32 tz) {
    GridSpec sim;
    sim.texelSize = params.macroTexel;
    sim.originX =
        static_cast<f32>(tx) * params.tileSize - params.apron;
    sim.originZ =
        static_cast<f32>(tz) * params.tileSize - params.apron;
    sim.n = texels(params.tileSize + 2.0f * params.apron,
                   params.macroTexel) +
            1;
    return sim;
}

// Bounded-map border transitions: shaped BEFORE the imprint (a
// master course may carve its gorge through a range — the river
// exit) and BEFORE the erosion (a sea arm is a perfect drainage
// outlet).
void shapeMapBorders(const TileBakeParams& params,
                     const ProceduralControls& controls,
                     const GridSpec& sim, MacroResult& macro) {
    if (!params.mapGrid.valid) {
        return;
    }
    for (u32 row = 0; row < sim.n; ++row) {
        for (u32 col = 0; col < sim.n; ++col) {
            f32& h =
                macro.height[static_cast<size_t>(row) * sim.n +
                             col];
            h = applyMapGridShape(controls, params.macro,
                                  params.mapGrid, sim.x(col),
                                  sim.z(row), h);
        }
    }
}

// Sediment fills the socle's LOCAL closed basins BEFORE the
// erosion: with the hard cut budget the fastscape can no longer
// breach every dimple of the relief carrier, and each one became a
// lake (141 on the first budgeted map). The dimples are found on
// the HIGH-PASS of a 64 m copy (a ~1 km box mean removed — the
// lowland enclosed by its province is not a dimple, a flood of the
// raw surface drowned the whole map to its far spill), flooded to
// their local spill with a draining slope; the fill depth comes
// back bilinearly, on calm ground. The versants keep their basins
// (the erosion dissects them, the budget is wide there).
void fillCalmDimples(const TileBakeParams& params, const GridSpec& sim,
                     MacroResult& macro) {
    const u32 step = glm::max(
        1u, static_cast<u32>(std::lround(64.0f / sim.texelSize)));
    const GridSpec coarse { sim.originX, sim.originZ,
                            sim.texelSize * static_cast<f32>(step),
                            (sim.n - 1) / step + 1 };
    vector<f32> h(coarse.cells());
    for (u32 row = 0; row < coarse.n; ++row) {
        for (u32 col = 0; col < coarse.n; ++col) {
            h[static_cast<size_t>(row) * coarse.n + col] =
                macro.height[static_cast<size_t>(row * step) *
                                 sim.n +
                             col * step];
        }
    }
    vector<f32> low = h;
    for (u32 pass = 0; pass < 60; ++pass) { // ~1 km box mean
        low = boxBlur3(coarse, low);
    }
    vector<f32> hp(coarse.cells());
    for (size_t i = 0; i < hp.size(); ++i) {
        hp[i] = h[i] - low[i];
    }
    const vector<f32> filled =
        priorityFloodFill(coarse, hp, -1.0e9f, 1.0e-3f);
    // Dimples only: a hollow deeper than this between two ridges
    // is a valley the walker descends into, not a pond to fill.
    vector<f32> depth(coarse.cells());
    for (size_t i = 0; i < depth.size(); ++i) {
        depth[i] = glm::clamp(filled[i] - hp[i], 0.0f,
                              params.dimpleFillMax);
    }
    for (u32 row = 0; row < sim.n; ++row) {
        for (u32 col = 0; col < sim.n; ++col) {
            const size_t i = static_cast<size_t>(row) * sim.n + col;
            if (macro.height[i] <= params.macro.seaLevel) {
                continue;
            }
            f32 w = glm::smoothstep(0.25f, 0.75f, macro.calm[i]);
            if (!macro.basin.empty()) {
                // A designed basin is water: never filled.
                w *= 1.0f - glm::smoothstep(0.5f, 2.0f, macro.basin[i]);
            }
            if (w <= 0.0f) {
                continue;
            }
            macro.height[i] +=
                w * bilinearGrid(coarse, depth,
                                 static_cast<f32>(col) /
                                     static_cast<f32>(step),
                                 static_cast<f32>(row) /
                                     static_cast<f32>(step));
        }
    }
}

// The fleuve imprint — the "authored -> S1 before erosion" slot: the
// master courses carve their channel, plain and monotone bed into
// the macro BEFORE the fastscape, which then sculpts around them
// (imprintKeep). The apron makes neighbours stamp identically.
vector<f32> imprintFleuves(const TileBakeParams& params,
                           const ProceduralControls& controls,
                           const MacroParams& macroParams,
                           const GridSpec& sim, MacroResult& macro) {
    vector<f32> imprintKeep(sim.cells(), 0.0f);
    MasterNetworkParams network = params.network;
    network.seaLevel = params.macro.seaLevel;
    network.mapGrid = params.mapGrid;
    imprintMasterChannels(sim, macro, imprintKeep, controls, macroParams,
                          network, params.imprint,
                          params.hydrology.widthLaw());
    return imprintKeep;
}

// Passability corridors soften the erosion locally: softer rock
// (higher k -> LOWER equilibrium slopes: the physics of a mountain
// pass) and smoother scree. Heights are never touched directly.
// The uplift/plains/lithology character below applies with or without
// corridors; only the corridor softening itself is gated on `gentle`.
BiomeCharacter erosionCharacter(const TileBakeParams& params,
                                const GridSpec& sim,
                                const MacroResult& macro) {
    BiomeCharacter character =
        biomeCharacter(sim, macro.biome, params.biomeErosion);
    if (character.erodibility.empty()) {
        const size_t cells = sim.cells();
        character.erodibility.assign(cells, 1.0f);
        character.talusScale.assign(cells, 1.0f);
        character.capacityScale.assign(cells, 1.0f);
        character.fineScale.assign(cells, 1.0f);
    }
    for (size_t i = 0; i < macro.gentle.size(); ++i) {
        const f32 g = macro.gentle[i];
        character.erodibility[i] *= 1.0f + 1.6f * g;
        character.talusScale[i] *= 1.0f - 0.25f * g;
    }
    // Ranges shed steeper: a lower angle of repose where the uplift
    // is strong relaxes aretes into walkable scree shoulders. And
    // TRUE plains are soft sediment: doubled erodibility halves
    // their valley slopes — but only where no orogeny runs, no hill
    // chain rolls and no massif plateau stands. Foothills and hill
    // country keep hard rock: fastscape base-level lowering
    // propagates upstream, so a soft ring around a massif pulls its
    // summits down over the iterations.
    for (size_t i = 0; i < macro.uplift.size(); ++i) {
        const f32 u = glm::smoothstep(0.15f, 0.5f, macro.uplift[i]);
        character.talusScale[i] *= 1.0f - 0.3f * u;
        const f32 plain =
            (1.0f - glm::smoothstep(0.03f, 0.15f, macro.uplift[i])) *
            (1.0f -
             glm::smoothstep(30.0f, 90.0f, macro.hillRelief[i])) *
            (1.0f -
             glm::smoothstep(60.0f, 180.0f, macro.plateau[i]));
        character.erodibility[i] *= 1.0f + 0.3f * plain;
        // Lithology: hard pockets erode slow and hold steeper
        // scree, soft pockets roll — neutralized where a corridor
        // runs (a pass is a promise) and clamped so the stacked
        // factors (biome, plains, gentle) never run away.
        if (!macro.hardness.empty()) {
            const f32 gentleHere =
                i < macro.gentle.size() ? macro.gentle[i] : 0.0f;
            const f32 hard =
                glm::mix(macro.hardness[i], 0.5f, gentleHere);
            character.erodibility[i] *=
                glm::mix(1.6f, 0.55f, hard);
            character.talusScale[i] *= glm::mix(0.9f, 1.25f, hard);
        }
        // Calm socles shed gentle (a softer talus rounds what the
        // budget leaves) and fill flat; their erodibility is NOT
        // boosted any more — the cut budget below is what keeps
        // the walking rhythm, a soft socle only eroded faster.
        if (i < macro.calm.size()) {
            const f32 calm = macro.calm[i];
            const f32 low =
                1.0f - glm::smoothstep(
                           150.0f, 400.0f,
                           macro.height[i] - params.macro.seaLevel);
            character.talusScale[i] *= 1.0f - 0.2f * calm;
            // Sediment fills the low socle floors flat: LOWER
            // capacity makes the flux drop its load here
            // (deposition fires where flux exceeds capacity).
            character.capacityScale[i] *= 1.0f - 0.5f * calm * low;
        }
        character.erodibility[i] =
            glm::clamp(character.erodibility[i], 0.4f, 3.0f);
        character.talusScale[i] =
            glm::clamp(character.talusScale[i], 0.5f, 1.6f);
        // The plan's cliffs hold: a designed wall is not scree.
        if (i < macro.cliff.size()) {
            character.talusScale[i] *= 1.0f + 2.0f * macro.cliff[i];
        }
    }
    return character;
}

// The erosion's two per-texel bounds: `keep` re-blends toward the
// input height (the lifts, the border ridges and the imprinted
// channels survive the stream power), `maxCut` is the HARD cut budget
// (docs/PAYSAGE.md §7.5).
struct ErosionBounds {
    vector<f32> keep;
    vector<f32> maxCut;
};

ErosionBounds erosionBounds(const TileBakeParams& params,
                            const ProceduralControls& controls,
                            const GridSpec& sim, const MacroResult& macro,
                            const vector<f32>& imprintKeep) {
    // Base lifts (swell, old-massif plateau) partially survive the
    // stream power: without this keep, the fastscape carves highlands
    // back toward sea base level and summits lose most of the lift.
    vector<f32> keep(macro.plateau.size());
    // Crest field for the graduated keep: what stands above the ~500 m
    // mean is a crest and deserves full protection; the mid-slopes
    // below give their keep back to the erosion (the measured profile:
    // dissection belongs to the flanks, summits only need shaping).
    vector<f32> crest;
    if (params.keepCrestFade > 0.0f) {
        vector<f32> mean = macro.height;
        for (u32 pass = 0; pass < 30; ++pass) {
            mean = boxBlur3(sim, mean);
        }
        crest.resize(macro.height.size());
        for (size_t i = 0; i < crest.size(); ++i) {
            crest[i] = glm::smoothstep(10.0f, 80.0f,
                                       macro.height[i] - mean[i]);
        }
    }
    // The ridge factor of the border ranges, kept for the budget.
    vector<f32> ridgeFactor(macro.plateau.size(), 0.0f);
    for (size_t i = 0; i < macro.plateau.size(); ++i) {
        // The pieces' lift resists the carve (the summit survives,
        // the flanks dissect); the socles are protected by the cut
        // budget instead.
        keep[i] = glm::min(kPlateauKeepMax,
                           macro.plateau[i] * kPlateauKeepCoef);
        if (!crest.empty()) {
            keep[i] *= glm::mix(1.0f - params.keepCrestFade, 1.0f,
                                crest[i]);
        }
        // The border ranges resist too: the artificial crest has no
        // plateau field — without this keep the fastscape carved a
        // 670 m crest down to a 313 m median.
        if (params.mapGrid.valid) {
            const u32 col = static_cast<u32>(i % sim.n);
            const u32 row = static_cast<u32>(i / sim.n);
            ridgeFactor[i] = mapGridRidgeFactor(
                controls, params.macro, params.mapGrid, sim.x(col),
                sim.z(row), macro.height[i]);
            keep[i] = glm::max(keep[i],
                               kMapBorderRidgeKeep * ridgeFactor[i]);
        }
        // The imprinted fleuve channel/plain resists the fastscape: the
        // constructed course must survive erosion like a pad would.
        keep[i] = glm::max(keep[i], imprintKeep[i]);
    }
    // The HARD cut budget: calm socles keep their macro (the walking
    // rhythm), everything else dissects; the imprinted channels and
    // the border ridges' cols take the rough budget (the fleuve wins
    // its bed, a col gets carved walkable).
    vector<f32> maxCut(macro.height.size());
    for (size_t i = 0; i < maxCut.size(); ++i) {
        f32 cut = glm::mix(params.roughCut, params.calmCut,
                           glm::smoothstep(0.25f, 0.75f, macro.calm[i]));
        // A piece is a DESIGNED landmark: its flanks get a light
        // dissection, never a carve to the plain.
        cut = glm::mix(cut, 4.0f * params.calmCut,
                       glm::smoothstep(30.0f, 80.0f, macro.plateau[i]));
        // The zone's own dissection (its archetype's erosionCut): a
        // floor under the rule, off the designed landforms' flanks.
        if (i < macro.cut.size()) {
            cut = glm::max(cut, macro.cut[i] * (1.0f - glm::smoothstep(
                                                     30.0f, 80.0f,
                                                     macro.plateau[i])));
        }
        // A plateau's escarpment is a designed wall too: dissected
        // lightly, never carved to the plain (a carve that depended on
        // the window's drainage).
        if (i < macro.scarp.size()) {
            cut = glm::mix(cut, 4.0f * params.calmCut, macro.scarp[i]);
        }
        if (imprintKeep[i] > 0.0f) {
            cut = params.roughCut;
        } else if (ridgeFactor[i] > 0.2f) {
            // A border range is a DESIGNED landform: the light budget
            // of a piece. The band used to take the rough budget to
            // carve its cols, and the fastscape, fed along the crest
            // into every saddle, dug a 250 m trench ALONG the line — a
            // river running the border for a kilometer (dev bug report
            // 2026-10-08). A saddle is walkable by its own lowered
            // crest (a third of the lift over 900 m): no carve needed.
            cut = 4.0f * params.calmCut;
        }
        maxCut[i] = cut;
    }
    return { std::move(keep), std::move(maxCut) };
}

// The socle keeps the RAW story relief: the thermal pass (and the
// rounding below) only sculpt the rough ground — massifs, pieces'
// flanks, versants. Blended back per texel on the control calm.
vector<f32> roughWeights(const MacroResult& macro) {
    vector<f32> roughW(macro.calm.size());
    for (size_t i = 0; i < roughW.size(); ++i) {
        roughW[i] = 1.0f - glm::smoothstep(0.25f, 0.75f, macro.calm[i]);
    }
    return roughW;
}

// Round the knife edges the orogeny built. Uplift-gated: hill tops
// and mesa rims keep their edge, peaks and aretes lose theirs.
void roundCrests(const TileBakeParams& params, const GridSpec& sim,
                 const MacroResult& macro, const vector<f32>& roughW,
                 vector<f32>& eroded) {
    RidgeRoundParams rounding = params.rounding;
    rounding.seaLevel = params.macro.seaLevel;
    vector<f32> crestWeight(macro.uplift.size());
    for (size_t i = 0; i < macro.uplift.size(); ++i) {
        crestWeight[i] =
            glm::smoothstep(0.15f, 0.5f, macro.uplift[i]);
    }
    const vector<f32> rounded =
        roundRidges(sim, eroded, rounding, &crestWeight);
    for (size_t i = 0; i < eroded.size(); ++i) {
        eroded[i] = glm::mix(eroded[i], rounded[i], roughW[i]);
    }
}

// Valley floors join the calm-socle family here: they only exist
// after erosion carved them. A floor = low local relief (mean
// absolute deviation from a ~160 m box mean) and not the pit of a
// lake basin (those belong to the water). Pure gathers — the
// stage-1 determinism contract holds.
void fuseValleyFloors(const TileBakeParams& params, const GridSpec& sim,
                      const vector<f32>& eroded, vector<f32>& calm) {
    vector<f32> mean = eroded;
    for (u32 pass = 0; pass < 10; ++pass) {
        mean = boxBlur3(sim, mean);
    }
    vector<f32> dev(sim.cells());
    for (size_t i = 0; i < dev.size(); ++i) {
        dev[i] = std::abs(eroded[i] - mean[i]);
    }
    for (u32 pass = 0; pass < 3; ++pass) {
        dev = boxBlur3(sim, dev);
    }
    const vector<f32> filled = priorityFloodFill(
        sim, eroded, params.macro.seaLevel, 1.0e-4f);
    for (size_t i = 0; i < calm.size(); ++i) {
        if (eroded[i] <= params.macro.seaLevel) {
            continue;
        }
        const f32 floor =
            (1.0f - glm::smoothstep(2.5f, 6.0f, dev[i])) *
            (1.0f -
             glm::smoothstep(1.5f, 3.0f, filled[i] - eroded[i]));
        calm[i] = glm::max(calm[i], floor);
    }
}

// Tier classification (S4 tail): the master network's TRUE drainage
// areas, once for the whole map.
void classifyMapRivers(const TileBakeParams& params,
                       const HydrologyParams& hydrology,
                       MapHydrology& out) {
    ProceduralControlParams cp = params.controls;
    cp.seed = params.worldSeed;
    MasterNetworkParams network = params.network;
    network.seaLevel = params.macro.seaLevel;
    network.mapGrid = params.mapGrid;
    const f32 windowMaxX =
        out.window.originX +
        static_cast<f32>(out.window.n - 1) * out.window.texelSize;
    const f32 windowMaxZ =
        out.window.originZ +
        static_cast<f32>(out.window.n - 1) * out.window.texelSize;
    const vector<MasterRiver> master = masterRiversNear(
        ProceduralControls { cp }, params.macro, network,
        out.window.originX, out.window.originZ, windowMaxX,
        windowMaxZ);
    classifyRivers(out.hydro.rivers, hydrology, params.worldSeed,
                   master);
}

// Mouth flare (estuaries): a course that reaches the sea opens
// over its last mouthFlareLength metres. Width-only — the ribbon
// and the S5c bed carve both read hw, so the estuary bed follows.
void flareRiverMouths(const HydrologyParams& hydrology, f32 seaLevel,
                      vector<River>& rivers) {
    if (hydrology.mouthFlare > 1.0f &&
        hydrology.mouthFlareLength > 1.0f) {
        for (River& river : rivers) {
            if (river.points.size() < 2 ||
                river.points.back().surface >
                    seaLevel + 0.5f) {
                continue;
            }
            f32 arc = 0.0f; // from the mouth, upstream
            for (size_t k = river.points.size(); k-- > 0;) {
                if (k + 1 < river.points.size()) {
                    const RiverPoint& dn = river.points[k + 1];
                    arc += std::hypot(river.points[k].x - dn.x,
                                      river.points[k].z - dn.z);
                }
                if (arc >= hydrology.mouthFlareLength) {
                    break;
                }
                const f32 t = 1.0f - arc / hydrology.mouthFlareLength;
                river.points[k].halfWidth *= glm::mix(
                    1.0f, hydrology.mouthFlare,
                    t * t * (3.0f - 2.0f * t));
            }
        }
    }
}

} // namespace

BiomeCharacter biomeCharacter(const GridSpec& spec,
                              const vector<u8>& biome,
                              const vector<BiomeErosion>& table) {
    BiomeCharacter out;
    if (table.empty() || biome.size() != spec.cells()) {
        return out;
    }
    const BiomeErosion neutral;
    const auto entry = [&](u8 id) -> const BiomeErosion& {
        return id < table.size() ? table[id] : neutral;
    };
    const size_t cells = spec.cells();
    vector<f32> rawErod(cells);
    vector<f32> rawTalus(cells);
    vector<f32> rawCapacity(cells);
    vector<f32> rawFine(cells);
    for (size_t i = 0; i < cells; ++i) {
        const BiomeErosion& e = entry(biome[i]);
        rawErod[i] = e.erodibility;
        rawTalus[i] = e.talusScale;
        rawCapacity[i] = e.capacityScale;
        rawFine[i] = e.fineScale;
    }
    // 3x3 box blur (GridOps, deterministic row-major): the ids are
    // nearest-sampled, the blur keeps erosion from stepping at borders.
    out.erodibility = boxBlur3(spec, rawErod);
    out.talusScale = boxBlur3(spec, rawTalus);
    out.capacityScale = boxBlur3(spec, rawCapacity);
    out.fineScale = boxBlur3(spec, rawFine);
    return out;
}

TileStage1 bakeTileStage1(const TileBakeParams& params, i32 tx, i32 tz,
                          const std::atomic<bool>* cancel) {
    const auto cancelled = [cancel] {
        return cancel && cancel->load(std::memory_order_relaxed);
    };
    TileStage1 out;
    out.sim = simSpecFor(params, tx, tz);

    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    MacroParams macroParams = params.macro;
    macroParams.hillChainWavelength = controlParams.rhythm.crestWavelength;
    macroParams.bedWavelength = controlParams.rhythm.bedWavelength;
    // Phase clock (docs/CPU-PERF.md, Z1): one line per stage-1 with
    // the seconds of each pass — the measure the perf bricks read.
    const auto phaseStart = std::chrono::steady_clock::now();
    auto lapStart = phaseStart;
    char phases[512];
    size_t phasesLen = 0;
    const auto lap = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        const f64 sec =
            std::chrono::duration<f64>(now - lapStart).count();
        lapStart = now;
        const int n = std::snprintf(phases + phasesLen,
                                    sizeof(phases) - phasesLen, "%s%s %.2f",
                                    phasesLen ? " | " : "", name, sec);
        if (n > 0) {
            phasesLen = glm::min(phasesLen + static_cast<size_t>(n),
                                 sizeof(phases) - 1);
        }
    };
    MacroResult macro =
        synthesizeMacro(controls, out.sim, macroParams, params.worldSeed);
    lap("synthesis");
    shapeMapBorders(params, controls, out.sim, macro);
    fillCalmDimples(params, out.sim, macro);
    lap("borders+dimple");
    const vector<f32> imprintKeep =
        imprintFleuves(params, controls, macroParams, out.sim, macro);
    BiomeCharacter character = erosionCharacter(params, out.sim, macro);
    const ErosionBounds bounds =
        erosionBounds(params, controls, out.sim, macro, imprintKeep);
    out.macroHeight = macro.height;
    out.budget = bounds.maxCut;
    lap("imprint+budget");
    FluvialParams fluvial = params.fluvial;
    fluvial.seaLevel = params.macro.seaLevel;
    const FluvialResult eroded = erodeFluvial(
        out.sim, macro.height, macro.uplift, fluvial,
        bounds.keep.empty() ? nullptr : &bounds.keep,
        character.erodibility.empty() ? nullptr : &character.erodibility,
        character.capacityScale.empty() ? nullptr
                                        : &character.capacityScale,
        &bounds.maxCut, cancel);
    if (cancelled()) {
        out.eroded = eroded.height;
        return out; // partial, discarded by the caller
    }
    lap("fluvial");

    ThermalParams thermal = params.thermal;
    thermal.seaLevel = params.macro.seaLevel;
    ThermalResult relaxed = erodeThermal(
        out.sim, eroded.height, thermal,
        character.talusScale.empty() ? nullptr : &character.talusScale,
        cancel);

    out.eroded = std::move(relaxed.height);
    if (cancelled()) {
        return out; // partial, discarded by the caller
    }
    const vector<f32> roughW = roughWeights(macro);
    for (size_t i = 0; i < out.eroded.size(); ++i) {
        out.eroded[i] = glm::mix(eroded.height[i], out.eroded[i], roughW[i]);
    }
    lap("thermal");
    roundCrests(params, out.sim, macro, roughW, out.eroded);
    // One sediment field: thermal scree + fluvial alluvium.
    out.deposit = std::move(relaxed.deposit);
    if (!eroded.deposit.empty()) {
        for (size_t i = 0; i < out.deposit.size(); ++i) {
            out.deposit[i] += eroded.deposit[i];
        }
    }
    lap("rounding");
    out.calm = macro.calm;
    fuseValleyFloors(params, out.sim, out.eroded, out.calm);
    out.seaDist = std::move(macro.seaDist);
    out.biome = std::move(macro.biome);
    out.gentle = std::move(macro.gentle);
    out.uplift = std::move(macro.uplift);
    out.trunk = macro.trunk;
    lap("fusion");
    LOG_INFO("stage-1 ({}, {}) {}x{}: {} | total {:.1f} s", tx, tz,
             out.sim.n, out.sim.n, phases,
             std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                        phaseStart)
                 .count());
    return out;
}

MapHydrology extractMapHydrology(
    const TileBakeParams& params, const TileStage1& mapS1, i32 mapX,
    i32 mapZ, i32 tilesPerSide,
    const vector<const FinalizeResult*>& groundSlices,
    const vector<render::TerrainRegion>& neighbourRegions,
    const std::atomic<bool>* cancel) {
    MapHydrology out;
    // Phase clock (docs/CPU-PERF.md): one line per map with the seconds
    // of each hydrology pass plus the corrective-pass activity counters
    // — the measure that decides which correction survives a routing
    // change and where the perf levers go.
    const auto phaseStart = std::chrono::steady_clock::now();
    auto lapStart = phaseStart;
    char phases[768];
    size_t phasesLen = 0;
    const auto lap = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        const f64 sec =
            std::chrono::duration<f64>(now - lapStart).count();
        lapStart = now;
        const int n = std::snprintf(phases + phasesLen,
                                    sizeof(phases) - phasesLen, "%s%s %.2f",
                                    phasesLen ? " | " : "", name, sec);
        if (n > 0) {
            phasesLen = glm::min(phasesLen + static_cast<size_t>(n),
                                 sizeof(phases) - 1);
        }
    };
    const f32 mapSize =
        params.tileSize * static_cast<f32>(tilesPerSide);
    out.window.texelSize = params.hydroTexelResolved();
    out.window.originX =
        static_cast<f32>(mapX) * mapSize - params.waterMargin;
    out.window.originZ =
        static_cast<f32>(mapZ) * mapSize - params.waterMargin;
    out.window.n = texels(mapSize + 2.0f * params.waterMargin,
                          out.window.texelSize) +
                   1;
    // The window samples the FINAL (phase-A) ground where the slices
    // cover it — water placed on the terrain the player walks — and
    // falls back to the stage-1 past the map rect (blended over
    // ~192 m so the flood sees no step along the coverage edge). A
    // window finer than the 16 m fallback simply oversamples a smooth
    // bilinear patch out there: no new pits, no step at the seam.
    const f32 mapMinX = static_cast<f32>(mapX) * mapSize;
    const f32 mapMinZ = static_cast<f32>(mapZ) * mapSize;
    const MapGroundSampler ground { mapS1, groundSlices, neighbourRegions,
                                    mapMinX, mapMinZ, mapSize };
    out.ground.resize(out.window.cells());
    for (u32 row = 0; row < out.window.n; ++row) {
        for (u32 col = 0; col < out.window.n; ++col) {
            out.ground[static_cast<size_t>(row) * out.window.n + col] =
                ground.at(out.window.x(col), out.window.z(row));
        }
    }
    lap("fill");
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return out; // partial: caller discards
    }
    HydrologyParams hydrology = params.hydrology;
    hydrology.seaLevel = params.macro.seaLevel;
    // The outlet piercing (E5): the depressions that earn no lake are
    // drained BEFORE extraction — valleys again, and their accumulation
    // flows on to the real lakes and courses downstream.
    const vector<f32> preGround = out.ground;
    pierceLakeOutlets(out.window, out.ground, hydrology);
    out.pierceDelta.resize(out.ground.size());
    for (size_t i = 0; i < out.ground.size(); ++i) {
        out.pierceDelta[i] = out.ground[i] - preGround[i];
    }
    lap("pierce");
    out.hydro = extractHydrology(out.window, out.ground, hydrology);
    lap("extract");
    classifyMapRivers(params, hydrology, out);
    flareRiverMouths(hydrology, params.macro.seaLevel, out.hydro.rivers);
    lap("classify");
    const HydroRelevelStats stats =
        relevelMapHydrology(params, hydrology, ground, preGround, out, lap);
    LOG_INFO("map hydrology phases: {} | total {:.1f} s | recenter {} "
             "pts | retrace {} runs | fill |ds| {:.0f} m | pools {} | "
             "saddle {} runs",
             phases,
             std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                        phaseStart)
                 .count(),
             stats.recenterMoved, stats.retracedRuns, stats.fillShift,
             stats.poolLakes, stats.saddleRuns);
    return out;
}

void applyPierceDelta(FinalizeResult& fine, const MapHydrology& hydro) {
    if (hydro.pierceDelta.empty() || fine.height.empty()) {
        return;
    }
    const GridSpec& window = hydro.window;
    const GridSpec& fs = fine.fineSpec;
    const f32 winMaxX =
        window.originX +
        static_cast<f32>(window.n - 1) * window.texelSize;
    const f32 winMaxZ =
        window.originZ +
        static_cast<f32>(window.n - 1) * window.texelSize;
    for (u32 row = 0; row < fs.n; ++row) {
        const f32 z = fs.z(row);
        if (z < window.originZ || z > winMaxZ) {
            continue;
        }
        for (u32 col = 0; col < fs.n; ++col) {
            const f32 x = fs.x(col);
            if (x < window.originX || x > winMaxX) {
                continue;
            }
            const f32 d =
                bilinearWorld(window, hydro.pierceDelta, x, z);
            if (d < -0.01f) {
                fine.height[static_cast<size_t>(row) * fs.n + col] +=
                    d;
            }
        }
    }
}

MapDrainage computeMapDrainage(const TileBakeParams& params,
                               const TileStage1& mapS1, i32 mapX,
                               i32 mapZ, i32 tilesPerSide) {
    MapDrainage out;
    const f32 mapSize =
        params.tileSize * static_cast<f32>(tilesPerSide);
    // Deliberately macroTexel, NOT hydroTexelResolved: this drainage
    // field only orients the fine erosion over the stage-1 ground —
    // the routing window's texel is a separate concern.
    out.window.texelSize = params.macroTexel;
    out.window.originX =
        static_cast<f32>(mapX) * mapSize - params.waterMargin;
    out.window.originZ =
        static_cast<f32>(mapZ) * mapSize - params.waterMargin;
    out.window.n =
        texels(mapSize + 2.0f * params.waterMargin, params.macroTexel) +
        1;
    vector<f32> ground(out.window.cells());
    for (u32 row = 0; row < out.window.n; ++row) {
        for (u32 col = 0; col < out.window.n; ++col) {
            ground[static_cast<size_t>(row) * out.window.n + col] =
                bilinearWorld(mapS1.sim, mapS1.eroded,
                              out.window.x(col), out.window.z(row));
        }
    }
    HydrologyParams hydrology = params.hydrology;
    hydrology.seaLevel = params.macro.seaLevel;
    out.area = drainageArea(out.window, ground, hydrology);
    return out;
}

FinalizeResult bakeSliceGround(const TileBakeParams& params, i32 tx,
                               i32 tz, const TileStage1& mapS1,
                               const MapDrainage& drainage,
                               const std::atomic<bool>* cancel) {
    const f32 tileMinX = static_cast<f32>(tx) * params.tileSize;
    const f32 tileMinZ = static_cast<f32>(tz) * params.tileSize;
    const TileStage1& self = mapS1;
    const GridSpec& sim = self.sim;

    // --- The slice finalize prologue (self = the map).
    MacroResult macroFields;
    macroFields.spec = sim;
    macroFields.seaDist = self.seaDist;
    macroFields.biome = self.biome;
    BiomeCharacter character =
        biomeCharacter(sim, self.biome, params.biomeErosion);
    const vector<f32>& fineDamp =
        self.calm.empty() ? self.gentle : self.calm;
    if (!fineDamp.empty()) {
        if (character.fineScale.empty()) {
            character.fineScale.assign(sim.cells(), 1.0f);
        }
        const u32 simN = sim.n;
        for (size_t i = 0; i < fineDamp.size(); ++i) {
            const f32 damp =
                params.fineCalmGateHigh > 0.0f
                    ? glm::smoothstep(params.fineCalmGateLow,
                                      params.fineCalmGateHigh,
                                      fineDamp[i])
                    : fineDamp[i];
            character.fineScale[i] *= 1.0f - 0.95f * damp;
            if (params.fineSlopeReturn > 0.0f &&
                !self.eroded.empty()) {
                const u32 col = static_cast<u32>(i % simN);
                const u32 row = static_cast<u32>(i / simN);
                const u32 c1 = glm::min(col + 1, simN - 1);
                const u32 r1 = glm::min(row + 1, simN - 1);
                const f32 sx =
                    (self.eroded[static_cast<size_t>(row) * simN + c1] -
                     self.eroded[i]) /
                    sim.texelSize;
                const f32 sz =
                    (self.eroded[static_cast<size_t>(r1) * simN + col] -
                     self.eroded[i]) /
                    sim.texelSize;
                const f32 steep = glm::smoothstep(
                    0.18f, 0.45f, std::sqrt(sx * sx + sz * sz));
                character.fineScale[i] *=
                    1.0f +
                    params.fineSlopeReturn * steep * (1.0f - damp);
            }
        }
    }
    if (!self.uplift.empty()) {
        if (character.fineScale.empty()) {
            character.fineScale.assign(sim.cells(), 1.0f);
        }
        for (size_t i = 0; i < self.uplift.size(); ++i) {
            const f32 low =
                1.0f - glm::smoothstep(0.05f, 0.4f, self.uplift[i]);
            character.fineScale[i] *= 1.0f - 0.5f * low;
        }
    }
    const f32 keepMinX = tileMinX - params.overlapMargin;
    const f32 keepMinZ = tileMinZ - params.overlapMargin;
    const f32 keepSpan = params.tileSize + 2.0f * params.overlapMargin;

    FinalizeParams finalize = params.finalize;
    finalize.seaLevel = params.macro.seaLevel;
    finalize.upsampleFactor = glm::max(finalize.upsampleFactor, 1u);
    finalize.fineMinX = keepMinX - kFineErosionHalo;
    finalize.fineMinZ = keepMinZ - kFineErosionHalo;
    finalize.fineSpan = keepSpan + 2.0f * kFineErosionHalo;
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return {};
    }
    return finalizeGround(
        sim, self.eroded, macroFields,
        drainage.area.empty() ? nullptr : &drainage.area,
        drainage.area.empty() ? nullptr : &drainage.window, finalize,
        params.worldSeed,
        character.fineScale.empty() ? nullptr : &character.fineScale,
        cancel);
}

TileBakeResult bakeSliceWater(const TileBakeParams& params, i32 tx,
                              i32 tz, const TileStage1& mapS1,
                              const MapHydrology& mapHydro,
                              FinalizeResult&& fineIn,
                              const std::atomic<bool>* cancel) {
    const auto cancelled = [cancel] {
        return cancel && cancel->load(std::memory_order_relaxed);
    };
    const f32 tileMinX = static_cast<f32>(tx) * params.tileSize;
    const f32 tileMinZ = static_cast<f32>(tz) * params.tileSize;
    const TileStage1& self = mapS1;
    const GridSpec& sim = self.sim;
    const GridSpec& window = mapHydro.window;
    const HydrologyResult& hydro = mapHydro.hydro;
    const f32 keepMinX = tileMinX - params.overlapMargin;
    const f32 keepMinZ = tileMinZ - params.overlapMargin;
    const f32 keepSpan = params.tileSize + 2.0f * params.overlapMargin;

    FinalizeResult fine = std::move(fineIn);
    // The piercing is terrain: gorges and drained basin floors land
    // in the fine ground BEFORE the carves ride it.
    applyPierceDelta(fine, mapHydro);
    MacroResult macroFields;
    macroFields.spec = sim;
    macroFields.seaDist = self.seaDist;
    macroFields.biome = self.biome;
    FinalizeParams finalize = params.finalize;
    finalize.seaLevel = params.macro.seaLevel;
    if (cancelled()) {
        return {};
    }
    finalizeWater(fine, sim, self.eroded, macroFields, hydro, window,
                  finalize, params.worldSeed,
                  self.deposit.empty() ? nullptr : &self.deposit);

    // Crop to slice + overlap margin (the same crop the windowed
    // path used —
    // neighbouring slices carry bit-identical bands by construction).
    TileBakeResult out;
    TerrainRegion& region = out.region;
    region.originX = keepMinX;
    region.originZ = keepMinZ;
    region.texelSize = fine.fineSpec.texelSize;
    region.edgeBlend = 2.0f * params.overlapMargin;
    const u32 fineOff = texels(keepMinX - fine.fineSpec.originX,
                               fine.fineSpec.texelSize);
    const u32 fineN = texels(keepSpan, fine.fineSpec.texelSize) + 1;
    region.width = fineN;
    region.height = fineN;
    region.heights.resize(static_cast<size_t>(fineN) * fineN);
    for (u32 row = 0; row < fineN; ++row) {
        const size_t src =
            static_cast<size_t>(row + fineOff) * fine.fineSpec.n +
            fineOff;
        std::copy_n(fine.height.begin() + static_cast<ptrdiff_t>(src),
                    fineN,
                    region.heights.begin() +
                        static_cast<ptrdiff_t>(
                            static_cast<size_t>(row) * fineN));
    }
    const u32 maskOff = texels(keepMinX - sim.originX, sim.texelSize);
    const u32 maskN = texels(keepSpan, sim.texelSize) + 1;
    region.maskWidth = maskN;
    region.maskHeight = maskN;
    const auto crop = [&](const vector<u8>& srcGrid, vector<u8>& dst) {
        dst.resize(static_cast<size_t>(maskN) * maskN);
        for (u32 row = 0; row < maskN; ++row) {
            const size_t src =
                static_cast<size_t>(row + maskOff) * sim.n + maskOff;
            std::copy_n(srcGrid.begin() + static_cast<ptrdiff_t>(src),
                        maskN,
                        dst.begin() + static_cast<ptrdiff_t>(
                                          static_cast<size_t>(row) *
                                          maskN));
        }
    };
    crop(fine.flow, region.flow);
    crop(fine.wetness, region.wetness);
    crop(fine.beach, region.beach);
    crop(fine.detailAmp, region.detailAmp);
    crop(self.biome, region.biome);
    crop(fine.rockExposure, region.rockExposure);
    region.detailAmplitude = kRegionDetailAmplitude;
    region.detailWavelength = kRegionDetailWavelength;
    region.detailOctaves = kRegionDetailOctaves;

    // Water ownership: ONE hydrology per map — a lake belongs to the
    // slice holding its bbox center, CLAMPED to the map rect: a
    // margin basin straddling a map line kept cutting the ribbons
    // (its mask covers them) while NO slice ever published its sheet
    // — courses shredded into stubs around an invisible lake
    // (measured dev at (7846, 9844)). The edge slice publishes it;
    // the runtime clip to the map rect keeps line authority clean.
    const f32 tileMaxX = tileMinX + params.tileSize;
    const f32 tileMaxZ = tileMinZ + params.tileSize;
    const f32 mapMinX = window.originX + params.waterMargin;
    const f32 mapMinZ = window.originZ + params.waterMargin;
    const f32 mapSpanW =
        static_cast<f32>(window.n - 1) * window.texelSize -
        2.0f * params.waterMargin;
    for (const Lake& lake : hydro.lakes) {
        const f32 cx =
            glm::clamp((lake.minX + lake.maxX) * 0.5f, mapMinX,
                       mapMinX + mapSpanW - 0.01f);
        const f32 cz =
            glm::clamp((lake.minZ + lake.maxZ) * 0.5f, mapMinZ,
                       mapMinZ + mapSpanW - 0.01f);
        if (cx < tileMinX || cx >= tileMaxX || cz < tileMinZ ||
            cz >= tileMaxZ) {
            continue;
        }
        out.lakes.push_back(lake);
    }
    // No re-validation pass any more: the hydrology ran ON the final
    // ground (two-phase bake) — its lakes ARE the published truth.
    const vector<Lake>& allLakes = hydro.lakes;
    const f32 keepMaxX = keepMinX + keepSpan;
    const f32 keepMaxZ = keepMinZ + keepSpan;
    const auto inKeep = [&](const RiverPoint& pt) {
        return pt.x >= keepMinX && pt.x <= keepMaxX && pt.z >= keepMinZ &&
               pt.z <= keepMaxZ;
    };
    for (const River& river : hydro.rivers) {
        // A lake-fed head whose source lake is not under it (rim
        // basins clipped at the window edge): DEMOTED to a spring —
        // dropping the whole course would delete a long river for
        // its first metre.
        const bool headLakeGone =
            river.lakeFed != 0 && !river.points.empty() &&
            !lakeReachesPoint(allLakes, river.points.front().x,
                              river.points.front().z);
        const auto emit = [&](River&& run) {
            for (const Vec2& ford : river.fords) {
                for (const RiverPoint& pt : run.points) {
                    const f32 dx = pt.x - ford.x;
                    const f32 dz = pt.z - ford.y;
                    if (dx * dx + dz * dz < 64.0f * 64.0f) {
                        run.fords.push_back(ford);
                        break;
                    }
                }
            }
            out.rivers.push_back(std::move(run));
        };
        // A clipped run keeps the course's identity fields — the .twb
        // readers reclassify and name sources off them (TWB5).
        const auto freshRun = [&] {
            River run;
            run.tier = river.tier;
            run.mouthArea = river.mouthArea;
            run.lakeFed = headLakeGone ? u8 { 0 } : river.lakeFed;
            run.sourceLake = headLakeGone ? -1 : river.sourceLake;
            run.headType =
                headLakeGone
                    ? static_cast<u32>(RiverHeadType::Spring)
                    : river.headType;
            return run;
        };
        // Ribbon-vs-nappe contract: the trace runs THROUGH lakes;
        // the emission drops the points a nappe covers. One point of
        // overlap survives on each side (the ribbon reaches under
        // the sheet), and a run resuming past a nappe skips the
        // spring taper like any outlet.
        const auto inFinalLake = [&](const RiverPoint& pt) {
            for (const Lake& lake : allLakes) {
                if (lake.covers(pt.x, pt.z)) {
                    return true;
                }
            }
            return false;
        };
        River run = freshRun();
        bool lastInLake = false;
        bool atHead = true; // next accepted point IS the course head
        const RiverPoint* lastLakePt = nullptr;
        for (const RiverPoint& pt : river.points) {
            const bool head = atHead;
            atHead = false;
            if (!inKeep(pt)) {
                if (run.points.size() >= 2) {
                    emit(std::move(run));
                }
                run = freshRun();
                lastInLake = false;
                lastLakePt = nullptr;
                continue;
            }
            if (inFinalLake(pt)) {
                if (!lastInLake) {
                    run.points.push_back(pt); // reach under the sheet
                    if (run.points.size() >= 2) {
                        emit(std::move(run));
                    }
                    run = freshRun();
                }
                lastInLake = true;
                lastLakePt = &pt;
                continue;
            }
            if (lastInLake) {
                run = freshRun();
                run.lakeFed = 1;
                run.headType =
                    static_cast<u32>(RiverHeadType::LakeOutlet);
                if (lastLakePt) {
                    run.points.push_back(*lastLakePt);
                }
                lastInLake = false;
            }
            if (head) {
                run.headIsSource = 1;
            }
            run.points.push_back(pt);
        }
        if (run.points.size() >= 2) {
            emit(std::move(run));
        }
    }
    return out;
}

TileBakeResult bakeSoloTile(const TileBakeParams& params, i32 tx,
                            i32 tz, const std::atomic<bool>* cancel) {
    // A 1x1-slice map: the tile IS the map rect, so the map pipeline
    // applies unchanged — one stage-1, one hydrology, one finalize.
    // The apron only needs to COVER the hydrology window (rect +
    // waterMargin); production maps widen it to kMapApron
    // (game::bakeMap) so rim basins resolve fully — a bench tile
    // accepts window-clipped rim basins instead of a 6 km sim.
    TileBakeParams solo = params;
    solo.apron = glm::max(params.apron, params.waterMargin);
    const TileStage1 mapS1 = bakeTileStage1(solo, tx, tz, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return {};
    }
    const MapDrainage drainage =
        computeMapDrainage(solo, mapS1, tx, tz, 1);
    FinalizeResult ground =
        bakeSliceGround(solo, tx, tz, mapS1, drainage, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return {};
    }
    const vector<const FinalizeResult*> grounds { &ground };
    const MapHydrology hydro =
        extractMapHydrology(solo, mapS1, tx, tz, 1, grounds, {},
                            cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return {};
    }
    return bakeSliceWater(solo, tx, tz, mapS1, hydro,
                          std::move(ground), cancel);
}

bool lakeReachesPoint(const vector<Lake>& lakes, f32 x, f32 z) {
    for (const Lake& lake : lakes) {
        if (lake.mask.empty() || lake.maskWidth == 0 ||
            lake.cells == 0) {
            continue;
        }
        // 16 m of reach, in world metres (not mask texels, which
        // follow the routing grid): a receded lake whose nearest
        // covered cell sits farther is a COINCIDENCE, not a supply
        // (measured dev at the spawn col: a pocket of the receded
        // lake 32 m from the head, walled off by its own rim, kept
        // the orphan fleuve alive at 32 m of reach).
        constexpr f32 reach = 16.0f;
        if (x < lake.minX - reach || x > lake.maxX + reach ||
            z < lake.minZ - reach || z > lake.maxZ + reach) {
            continue;
        }
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const f32 px = x + static_cast<f32>(dx) * reach;
                const f32 pz = z + static_cast<f32>(dz) * reach;
                if (lake.covers(px, pz)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace render::terraingen
