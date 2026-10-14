#include <chrono>
#include <cstdio>

#include "engine/core/Log.hpp"
#include "engine/terrain/generation/TileBake.hpp"
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
    // Bounded-map border transitions: shaped BEFORE the imprint (a
    // master course may carve its gorge through a range — the river
    // exit) and BEFORE the erosion (a sea arm is a perfect drainage
    // outlet).
    if (params.mapGrid.valid) {
        for (u32 row = 0; row < out.sim.n; ++row) {
            for (u32 col = 0; col < out.sim.n; ++col) {
                f32& h =
                    macro.height[static_cast<size_t>(row) * out.sim.n +
                                 col];
                h = applyMapGridShape(controls, params.macro,
                                      params.mapGrid, out.sim.x(col),
                                      out.sim.z(row), h);
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
    {
        const u32 step = glm::max(
            1u, static_cast<u32>(std::lround(64.0f / out.sim.texelSize)));
        const GridSpec coarse { out.sim.originX, out.sim.originZ,
                                out.sim.texelSize * static_cast<f32>(step),
                                (out.sim.n - 1) / step + 1 };
        vector<f32> h(coarse.cells());
        for (u32 row = 0; row < coarse.n; ++row) {
            for (u32 col = 0; col < coarse.n; ++col) {
                h[static_cast<size_t>(row) * coarse.n + col] =
                    macro.height[static_cast<size_t>(row * step) *
                                     out.sim.n +
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
        for (u32 row = 0; row < out.sim.n; ++row) {
            for (u32 col = 0; col < out.sim.n; ++col) {
                const size_t i = static_cast<size_t>(row) * out.sim.n + col;
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
    vector<f32> imprintKeep(out.sim.cells(), 0.0f);
    {
        MasterNetworkParams network = params.network;
        network.seaLevel = params.macro.seaLevel;
        network.mapGrid = params.mapGrid;
        lap("borders+dimple");
        imprintMasterChannels(out.sim, macro, imprintKeep, controls,
                              macroParams, network, params.imprint,
                              params.hydrology.widthLaw());
    }
    BiomeCharacter character =
        biomeCharacter(out.sim, macro.biome, params.biomeErosion);
    // Passability corridors soften the erosion locally: softer rock
    // (higher k -> LOWER equilibrium slopes: the physics of a mountain
    // pass) and smoother scree. Heights are never touched directly.
    // The uplift/plains/lithology character below applies with or without
    // corridors; only the corridor softening itself is gated on `gentle`.
    if (character.erodibility.empty()) {
        const size_t cells = out.sim.cells();
        character.erodibility.assign(cells, 1.0f);
        character.talusScale.assign(cells, 1.0f);
        character.capacityScale.assign(cells, 1.0f);
        character.fineScale.assign(cells, 1.0f);
    }
    {
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
    }

    FluvialParams fluvial = params.fluvial;
    fluvial.seaLevel = params.macro.seaLevel;
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
            mean = boxBlur3(out.sim, mean);
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
            const u32 col = static_cast<u32>(i % out.sim.n);
            const u32 row = static_cast<u32>(i / out.sim.n);
            ridgeFactor[i] = mapGridRidgeFactor(
                controls, params.macro, params.mapGrid, out.sim.x(col),
                out.sim.z(row), macro.height[i]);
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
    out.macroHeight = macro.height;
    out.budget = maxCut;
    lap("imprint+budget");
    const FluvialResult eroded = erodeFluvial(
        out.sim, macro.height, macro.uplift, fluvial,
        keep.empty() ? nullptr : &keep,
        character.erodibility.empty() ? nullptr : &character.erodibility,
        character.capacityScale.empty() ? nullptr
                                        : &character.capacityScale,
        &maxCut, cancel);
    if (cancelled()) {
        out.eroded = eroded.height;
        return out; // partial, discarded by the caller
    }

    ThermalParams thermal = params.thermal;
    thermal.seaLevel = params.macro.seaLevel;
    lap("fluvial");
    ThermalResult relaxed = erodeThermal(
        out.sim, eroded.height, thermal,
        character.talusScale.empty() ? nullptr : &character.talusScale,
        cancel);

    out.eroded = std::move(relaxed.height);
    if (cancelled()) {
        return out; // partial, discarded by the caller
    }
    // The socle keeps the RAW story relief: the thermal pass (and the
    // rounding below) only sculpt the rough ground — massifs, pieces'
    // flanks, versants. Blended back per texel on the control calm.
    vector<f32> roughW(macro.calm.size());
    for (size_t i = 0; i < roughW.size(); ++i) {
        roughW[i] = 1.0f - glm::smoothstep(0.25f, 0.75f, macro.calm[i]);
    }
    for (size_t i = 0; i < out.eroded.size(); ++i) {
        out.eroded[i] = glm::mix(eroded.height[i], out.eroded[i], roughW[i]);
    }
    // Round the knife edges the orogeny built. Uplift-gated: hill tops
    // and mesa rims keep their edge, peaks and aretes lose theirs.
    {
        RidgeRoundParams rounding = params.rounding;
        rounding.seaLevel = params.macro.seaLevel;
        vector<f32> crestWeight(macro.uplift.size());
        for (size_t i = 0; i < macro.uplift.size(); ++i) {
            crestWeight[i] =
                glm::smoothstep(0.15f, 0.5f, macro.uplift[i]);
        }
        lap("thermal");
        const vector<f32> rounded =
            roundRidges(out.sim, out.eroded, rounding, &crestWeight);
        for (size_t i = 0; i < out.eroded.size(); ++i) {
            out.eroded[i] = glm::mix(out.eroded[i], rounded[i], roughW[i]);
        }
    }
    // One sediment field: thermal scree + fluvial alluvium.
    out.deposit = std::move(relaxed.deposit);
    if (!eroded.deposit.empty()) {
        for (size_t i = 0; i < out.deposit.size(); ++i) {
            out.deposit[i] += eroded.deposit[i];
        }
    }
    // Valley floors join the calm-socle family here: they only exist
    // after erosion carved them. A floor = low local relief (mean
    // absolute deviation from a ~160 m box mean) and not the pit of a
    // lake basin (those belong to the water). Pure gathers — the
    // stage-1 determinism contract holds.
    out.calm = macro.calm;
    lap("rounding");
    {
        vector<f32> mean = out.eroded;
        for (u32 pass = 0; pass < 10; ++pass) {
            mean = boxBlur3(out.sim, mean);
        }
        vector<f32> dev(out.sim.cells());
        for (size_t i = 0; i < dev.size(); ++i) {
            dev[i] = std::abs(out.eroded[i] - mean[i]);
        }
        for (u32 pass = 0; pass < 3; ++pass) {
            dev = boxBlur3(out.sim, dev);
        }
        const vector<f32> filled = priorityFloodFill(
            out.sim, out.eroded, params.macro.seaLevel, 1.0e-4f);
        for (size_t i = 0; i < out.calm.size(); ++i) {
            if (out.eroded[i] <= params.macro.seaLevel) {
                continue;
            }
            const f32 floor =
                (1.0f - glm::smoothstep(2.5f, 6.0f, dev[i])) *
                (1.0f -
                 glm::smoothstep(1.5f, 3.0f, filled[i] - out.eroded[i]));
            out.calm[i] = glm::max(out.calm[i], floor);
        }
    }
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
    const f32 mapSize =
        params.tileSize * static_cast<f32>(tilesPerSide);
    out.window.texelSize = params.macroTexel;
    out.window.originX =
        static_cast<f32>(mapX) * mapSize - params.waterMargin;
    out.window.originZ =
        static_cast<f32>(mapZ) * mapSize - params.waterMargin;
    out.window.n =
        texels(mapSize + 2.0f * params.waterMargin, params.macroTexel) +
        1;
    // The window samples the FINAL (phase-A) ground where the slices
    // cover it — water placed on the terrain the player walks — and
    // falls back to the stage-1 past the map rect (blended over
    // ~192 m so the flood sees no step along the coverage edge).
    const f32 mapMinX = static_cast<f32>(mapX) * mapSize;
    const f32 mapMinZ = static_cast<f32>(mapZ) * mapSize;
    const auto groundAt = [&](f32 x, f32 z) -> f32 {
        const f32 s1 = bilinearWorld(mapS1.sim, mapS1.eroded, x, z);
        // Among the covering slices, the one holding the point
        // DEEPEST inside wins: a slice's outer kFineErosionHalo ring
        // is edge-effect territory (sampling it flooded a buried
        // lake 40 m under the owner slice's real ground, measured on
        // a slice line).
        const FinalizeResult* best = nullptr;
        f32 bestMargin = -1.0e9f;
        for (const FinalizeResult* g : groundSlices) {
            if (!g || g->height.empty()) {
                continue;
            }
            const GridSpec& fs = g->fineSpec;
            const f32 maxX =
                fs.originX + static_cast<f32>(fs.n - 1) * fs.texelSize;
            const f32 maxZ =
                fs.originZ + static_cast<f32>(fs.n - 1) * fs.texelSize;
            const f32 margin = glm::min(
                glm::min(x - fs.originX, maxX - x),
                glm::min(z - fs.originZ, maxZ - z));
            if (margin < 0.0f) {
                continue;
            }
            if (margin > bestMargin) {
                bestMargin = margin;
                best = g;
            }
        }
        const f32 inner = best
                              ? bilinearWorld(best->fineSpec,
                                              best->height, x, z)
                              : s1;
        const f32 dOut = glm::max(
            glm::max(mapMinX - x, x - (mapMinX + mapSize)),
            glm::max(mapMinZ - z, z - (mapMinZ + mapSize)));
        if (dOut <= 0.0f) {
            return inner;
        }
        // Past the rect: a cached NEIGHBOUR map's published final
        // ground is truer than our own halo or the stage-1 — the
        // shared margin's basins then resolve on the terrain both
        // maps actually publish (the remaining water seam at lines).
        // Documented impurity: the margin water refines with the bake
        // order; a map baked alone stays bit-exact.
        for (const render::TerrainRegion& nr : neighbourRegions) {
            const f32 fx = (x - nr.originX) / nr.texelSize;
            const f32 fz = (z - nr.originZ) / nr.texelSize;
            if (fx < 0.0f || fz < 0.0f ||
                fx > static_cast<f32>(nr.width) - 1.001f ||
                fz > static_cast<f32>(nr.height) - 1.001f) {
                continue;
            }
            const u32 c = static_cast<u32>(fx);
            const u32 r = static_cast<u32>(fz);
            const f32 tx = fx - static_cast<f32>(c);
            const f32 tz = fz - static_cast<f32>(r);
            const auto at = [&](u32 cc, u32 rr) {
                return nr.heights[static_cast<size_t>(rr) * nr.width +
                                  cc];
            };
            const f32 nH = glm::mix(
                glm::mix(at(c, r), at(c + 1, r), tx),
                glm::mix(at(c, r + 1), at(c + 1, r + 1), tx), tz);
            return glm::mix(inner, nH,
                            glm::smoothstep(0.0f, 96.0f, dOut));
        }
        if (!best) {
            return s1;
        }
        const f32 w = 1.0f - glm::smoothstep(0.0f, 192.0f, dOut);
        return glm::mix(s1, inner, w);
    };
    out.ground.resize(out.window.cells());
    for (u32 row = 0; row < out.window.n; ++row) {
        for (u32 col = 0; col < out.window.n; ++col) {
            out.ground[static_cast<size_t>(row) * out.window.n + col] =
                groundAt(out.window.x(col), out.window.z(row));
        }
    }
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
    out.hydro = extractHydrology(out.window, out.ground, hydrology);
    // Tier classification (S4 tail): the master network's TRUE drainage
    // areas, once for the whole map.
    {
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
    if (hydrology.mouthFlare > 1.0f &&
        hydrology.mouthFlareLength > 1.0f) {
        for (River& river : out.hydro.rivers) {
            if (river.points.size() < 2 ||
                river.points.back().surface >
                    params.macro.seaLevel + 0.5f) {
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
    // Fine re-leveling: the 16 m window overstates an incised valley's
    // floor (the walls average in) and the priority-flood routing
    // surface is a chain of FLAT pools at spill levels — published
    // ribbons rode metres above the 2 m bed, and a clamp-only seat
    // turned every descent into 60-100 m stairs with 4-13 m lips whose
    // free side-water drains away. The profile is therefore REBUILT
    // from the fine pre-carve ground: per point, the lower envelope of
    // the carve footprint, then an upstream pooling fill — the surface
    // follows the floor on descents (rapids, not stairs) and pools
    // only where the floor genuinely dips under the downstream level.
    // Positions stay untouched (any post-smoothing displacement
    // re-creates crenellation — measured, docs/PAYSAGE.md).
    {
        const f32 winTexel = out.window.texelSize;
        const f32 winMaxX =
            out.window.originX +
            static_cast<f32>(out.window.n - 1) * winTexel;
        const f32 winMaxZ =
            out.window.originZ +
            static_cast<f32>(out.window.n - 1) * winTexel;
        // The pierce notches exist only as a window delta until phase
        // C transfers them (applyPierceDelta): sample the same
        // carve-only transfer so the profile sees the ground S5c will.
        const auto fineAt = [&](f32 x, f32 z) -> f32 {
            f32 g = groundAt(x, z);
            if (!out.pierceDelta.empty() && x >= out.window.originX &&
                x <= winMaxX && z >= out.window.originZ &&
                z <= winMaxZ) {
                const f32 d =
                    bilinearWorld(out.window, out.pierceDelta, x, z);
                if (d < -0.01f) {
                    g += d;
                }
            }
            return g;
        };
        // The water sits JUST UNDER the lowest fine ground of its
        // footprint (ribbon + carve shoulder): nothing beyond the
        // ribbon edge may stay below the waterline. A 5-point
        // longitudinal min covers the texel rows between points (~8 m
        // spacing) and absorbs sampling jitter; the pits it would
        // have dragged along in a clamp-only pass are bridged by the
        // pooling fill instead.
        constexpr f32 kEdgeTuck = 0.2f;
        // Ceiling of the fill: no point's water may stand more than a
        // pond's depth above the LOWEST fine floor upstream of it —
        // the no-floating invariant. A course routed along a fine
        // ridge the window never saw (its fine floor RISES downstream
        // by tens of metres) is tunneled at the upstream valley level,
        // buried and invisible under the ridge (the accepted trade;
        // the re-route brick owns those courses), instead of pooling
        // the whole upstream valley into a floating sheet — a plain
        // routing-level ceiling was measured 38 m above the valley
        // floor on such a reach. The slow recovery keeps one deep
        // mis-sampled pit from dragging its penalty kilometres
        // downstream (the lesson of the old through-cut).
        constexpr f32 kPoolMax = 2.5f;        // wall-tunnel slack (m)
        constexpr f32 kBoundRecover = 0.01f;  // m per m of course
        auto& rivers = out.hydro.rivers;
        const size_t riverCount = rivers.size();
        // Lateral re-centering: the window routed ~20% of points along
        // fine-terrain walls — the axis ground sits metres above the
        // groove the footprint min finds, so the course is buried and
        // its water invisible. Each point drifts toward the LOWEST
        // fine sample of its own footprint, in small SMOOTHED steps
        // (a raw 1-step snap crenellated the polylines — v98). Ends,
        // junction tails and points under a sheet stay put. The gate
        // is per-river and HARD: if any turn ends sharper than 45°,
        // the river's shift is rolled back whole.
        {
            constexpr i32 kRecenterPasses = 10;
            constexpr f32 kRecenterStep = 4.0f; // m per smoothed pass
            constexpr f32 kKinkGateCos = 0.70710678f; // 45 degrees
            const auto underAnyLake = [&](f32 x, f32 z) {
                for (const Lake& lake : out.hydro.lakes) {
                    if (lake.mask.empty() || lake.maskWidth == 0 ||
                        x < lake.minX || x > lake.maxX ||
                        z < lake.minZ || z > lake.maxZ) {
                        continue;
                    }
                    const u32 mx = glm::min(
                        static_cast<u32>(glm::max(
                            (x - lake.minX) / lake.maskTexel + 0.5f,
                            0.0f)),
                        lake.maskWidth - 1);
                    const u32 mz = glm::min(
                        static_cast<u32>(glm::max(
                            (z - lake.minZ) / lake.maskTexel + 0.5f,
                            0.0f)),
                        lake.maskHeight - 1);
                    if (lake.mask[static_cast<size_t>(mz) *
                                      lake.maskWidth +
                                  mx]) {
                        return true;
                    }
                }
                return false;
            };
            vector<f32> want;
            vector<f32> smooth;
            vector<f32> backupX;
            vector<f32> backupZ;
            for (River& river : rivers) {
                const size_t count = river.points.size();
                if (count < 9) {
                    continue;
                }
                backupX.resize(count);
                backupZ.resize(count);
                for (size_t i = 0; i < count; ++i) {
                    backupX[i] = river.points[i].x;
                    backupZ[i] = river.points[i].z;
                }
                // The perpendicular frame is FROZEN on the original
                // polyline: recomputing it from already-shifted
                // neighbours fed the shift back into the frame and
                // folded the line into hairpins (measured: 174/176
                // rivers rolled back by the kink gate).
                vector<f32> perpX(count, 0.0f);
                vector<f32> perpZ(count, 0.0f);
                for (size_t i = 1; i + 1 < count; ++i) {
                    const f32 dx = backupX[i + 1] - backupX[i - 1];
                    const f32 dz = backupZ[i + 1] - backupZ[i - 1];
                    const f32 len = std::sqrt(dx * dx + dz * dz);
                    if (len > 0.5f) {
                        perpX[i] = -dz / len;
                        perpZ[i] = dx / len;
                    }
                }
                for (i32 pass = 0; pass < kRecenterPasses; ++pass) {
                    want.assign(count, 0.0f);
                    for (size_t i = 3; i + 3 < count; ++i) {
                        const RiverPoint& pt = river.points[i];
                        if (underAnyLake(pt.x, pt.z)) {
                            continue;
                        }
                        const f32 px = perpX[i];
                        const f32 pz = perpZ[i];
                        if (px == 0.0f && pz == 0.0f) {
                            continue;
                        }
                        const f32 axisG = fineAt(pt.x, pt.z);
                        f32 bestG = axisG;
                        f32 bestD = 0.0f;
                        for (const f32 off :
                             { -1.4f, -1.2f, -1.0f, -0.75f, -0.5f,
                               0.5f, 0.75f, 1.0f, 1.2f, 1.4f }) {
                            const f32 d = off * pt.halfWidth;
                            const f32 g = fineAt(pt.x + px * d,
                                                 pt.z + pz * d);
                            if (g < bestG - 0.01f) {
                                bestG = g;
                                bestD = d;
                            }
                        }
                        // Converge instead of marching: only a point
                        // whose AXIS stands clearly above its groove
                        // moves (the buried family), and the pull
                        // fades out as it reaches the floor — on a
                        // plain monotone flank the min sits forever
                        // at the footprint edge and an ungated pull
                        // walked the course downhill without end.
                        const f32 gain = glm::clamp(
                            (axisG - bestG - 1.0f) / 3.0f, 0.0f,
                            1.0f);
                        want[i] = bestD * gain;
                    }
                    // Wide smoothing (~+-25 m): sign flips in the
                    // raw targets are what crenellates the line.
                    smooth = want;
                    for (i32 s = 0; s < 3; ++s) {
                        vector<f32> next = smooth;
                        for (size_t i = 3; i + 3 < count; ++i) {
                            f32 acc = 0.0f;
                            for (i32 o = -3; o <= 3; ++o) {
                                acc += smooth[i + o];
                            }
                            next[i] = acc / 7.0f;
                        }
                        smooth.swap(next);
                    }
                    for (size_t i = 3; i + 3 < count; ++i) {
                        RiverPoint& pt = river.points[i];
                        const f32 step =
                            glm::clamp(smooth[i], -kRecenterStep,
                                       kRecenterStep);
                        pt.x += perpX[i] * step;
                        pt.z += perpZ[i] * step;
                    }
                }
                // Kink gate, SURGICAL: only the points around a
                // too-sharp turn fall back to the original line (the
                // restored set grows monotonically, so the loop
                // terminates — worst case the whole river reverts).
                // An all-or-nothing gate threw away every clean reach
                // of a river for one fold (90/176 rivers, measured).
                for (i32 guard = 0; guard < 64; ++guard) {
                    bool dirty = false;
                    for (size_t i = 1; i + 1 < count; ++i) {
                        const f32 ax = river.points[i].x -
                                       river.points[i - 1].x;
                        const f32 az = river.points[i].z -
                                       river.points[i - 1].z;
                        const f32 bx2 = river.points[i + 1].x -
                                        river.points[i].x;
                        const f32 bz2 = river.points[i + 1].z -
                                        river.points[i].z;
                        const f32 la = std::sqrt(ax * ax + az * az);
                        const f32 lb =
                            std::sqrt(bx2 * bx2 + bz2 * bz2);
                        if (la < 0.1f || lb < 0.1f) {
                            continue;
                        }
                        if ((ax * bx2 + az * bz2) / (la * lb) >=
                            kKinkGateCos) {
                            continue;
                        }
                        bool changed = false;
                        for (size_t k = (i >= 4 ? i - 4 : 0);
                             k <= glm::min(i + 4, count - 1); ++k) {
                            if (river.points[k].x != backupX[k] ||
                                river.points[k].z != backupZ[k]) {
                                river.points[k].x = backupX[k];
                                river.points[k].z = backupZ[k];
                                changed = true;
                            }
                        }
                        dirty = dirty || changed;
                    }
                    if (!dirty) {
                        break;
                    }
                }
                // Fords were snapped to the old axis: follow it.
                for (Vec2& ford : river.fords) {
                    f32 best = 1.0e18f;
                    size_t bi = 0;
                    for (size_t i = 0; i < count; ++i) {
                        const f32 fx = river.points[i].x - ford.x;
                        const f32 fz = river.points[i].z - ford.y;
                        const f32 d2 = fx * fx + fz * fz;
                        if (d2 < best) {
                            best = d2;
                            bi = i;
                        }
                    }
                    ford.x = river.points[bi].x;
                    ford.y = river.points[bi].z;
                }
            }
        }
        // Window-lake re-seat: an accepted lake's spill level is a
        // 16 m estimate — containment the window guarantees can leak
        // at 2 m through a sub-cell notch, and the rivers now seated
        // on the fine floor expose every such overstatement (sheets
        // hanging over their own shores). Each natural lake is
        // priority-flooded on the 8 m lattice from its own wet cells:
        // an escape below level - 0.1 is the TRUE spill — the level
        // drops to it and the mask sheds the cells the lower sheet no
        // longer holds. A lake touching the window edge is left alone
        // (its basin is not fully visible here).
        {
            constexpr f32 kLakeTexel = 8.0f;
            constexpr f32 kLakeReach = 320.0f;
            for (Lake& lake : out.hydro.lakes) {
                if (lake.dug || lake.mask.empty() ||
                    lake.maskWidth < 2) {
                    continue;
                }
                const i32 gx0 = static_cast<i32>(std::floor(
                    (lake.minX - kLakeReach) / kLakeTexel));
                const i32 gx1 = static_cast<i32>(std::ceil(
                    (lake.maxX + kLakeReach) / kLakeTexel));
                const i32 gz0 = static_cast<i32>(std::floor(
                    (lake.minZ - kLakeReach) / kLakeTexel));
                const i32 gz1 = static_cast<i32>(std::ceil(
                    (lake.maxZ + kLakeReach) / kLakeTexel));
                if (static_cast<f32>(gx0) * kLakeTexel <
                        out.window.originX ||
                    static_cast<f32>(gx1) * kLakeTexel > winMaxX ||
                    static_cast<f32>(gz0) * kLakeTexel <
                        out.window.originZ ||
                    static_cast<f32>(gz1) * kLakeTexel > winMaxZ) {
                    continue;
                }
                const i32 gw = gx1 - gx0 + 1;
                const i32 gh = gz1 - gz0 + 1;
                vector<u8> seen(static_cast<size_t>(gw) * gh, 0);
                struct LakeCell {
                    f32 ground;
                    i32 cx;
                    i32 cz;
                    bool operator>(const LakeCell& o) const {
                        return ground > o.ground;
                    }
                };
                std::priority_queue<LakeCell, vector<LakeCell>,
                                    std::greater<LakeCell>>
                    heap;
                const f32 level = lake.level;
                const auto push = [&](i32 cx, i32 cz) {
                    if (cx < gx0 || cx > gx1 || cz < gz0 ||
                        cz > gz1) {
                        return;
                    }
                    const size_t idx =
                        static_cast<size_t>(cz - gz0) * gw +
                        static_cast<size_t>(cx - gx0);
                    if (seen[idx]) {
                        return;
                    }
                    seen[idx] = 1;
                    const f32 g =
                        fineAt(static_cast<f32>(cx) * kLakeTexel,
                               static_cast<f32>(cz) * kLakeTexel);
                    if (g >= level - 0.1f) {
                        return;
                    }
                    heap.push({ g, cx, cz });
                };
                for (u32 mr = 0; mr < lake.maskHeight; ++mr) {
                    for (u32 mc = 0; mc < lake.maskWidth; ++mc) {
                        if (!lake.mask[static_cast<size_t>(mr) *
                                           lake.maskWidth +
                                       mc]) {
                            continue;
                        }
                        const f32 px =
                            lake.minX +
                            static_cast<f32>(mc) * lake.maskTexel;
                        const f32 pz =
                            lake.minZ +
                            static_cast<f32>(mr) * lake.maskTexel;
                        push(static_cast<i32>(
                                 std::floor(px / kLakeTexel)),
                             static_cast<i32>(
                                 std::floor(pz / kLakeTexel)));
                    }
                }
                f32 spill = level;
                bool escaped = false;
                while (!heap.empty()) {
                    const LakeCell c = heap.top();
                    heap.pop();
                    if (c.cx == gx0 || c.cx == gx1 || c.cz == gz0 ||
                        c.cz == gz1) {
                        spill = c.ground;
                        escaped = true;
                        break;
                    }
                    push(c.cx - 1, c.cz);
                    push(c.cx + 1, c.cz);
                    push(c.cx, c.cz - 1);
                    push(c.cx, c.cz + 1);
                }
                if (!escaped || spill >= level - 0.5f) {
                    continue; // contained (or within the window's due)
                }
                lake.level =
                    glm::max(spill - 0.1f, hydrology.seaLevel);
                // Shed the mask cells the lower sheet no longer
                // holds (the dig gate's own rule), keep the count
                // honest.
                lake.cells = 0;
                for (u32 mr = 0; mr < lake.maskHeight; ++mr) {
                    for (u32 mc = 0; mc < lake.maskWidth; ++mc) {
                        u8& wet =
                            lake.mask[static_cast<size_t>(mr) *
                                          lake.maskWidth +
                                      mc];
                        if (!wet) {
                            continue;
                        }
                        const f32 px =
                            lake.minX +
                            static_cast<f32>(mc) * lake.maskTexel;
                        const f32 pz =
                            lake.minZ +
                            static_cast<f32>(mr) * lake.maskTexel;
                        if (fineAt(px, pz) > lake.level + 0.5f) {
                            wet = 0;
                        } else {
                            ++lake.cells;
                        }
                    }
                }
            }
        }
        vector<vector<f32>> floors(riverCount);
        vector<vector<f32>> bounds(riverCount);
        // Per point, the level of the accepted lake covering it (or
        // far below): a course traced THROUGH a lake must never be
        // seated under the lake's own sill — the fill's ceiling came
        // from the lake BED, tunnelling the course metres under the
        // outlet and letting S5c slot a trench through the shore (the
        // "glass dam"). Inside a sheet, the water is the sheet.
        vector<vector<f32>> lakeLvls(riverCount);
        vector<vector<f32>> oldSurfaces(riverCount);
        const auto coveringLakeLevel = [&](f32 x, f32 z) -> f32 {
            for (const Lake& lake : out.hydro.lakes) {
                if (lake.dug || lake.mask.empty() ||
                    lake.maskWidth == 0 || x < lake.minX ||
                    x > lake.maxX || z < lake.minZ || z > lake.maxZ) {
                    continue;
                }
                const u32 mx = glm::min(
                    static_cast<u32>(glm::max(
                        (x - lake.minX) / lake.maskTexel + 0.5f,
                        0.0f)),
                    lake.maskWidth - 1);
                const u32 mz = glm::min(
                    static_cast<u32>(glm::max(
                        (z - lake.minZ) / lake.maskTexel + 0.5f,
                        0.0f)),
                    lake.maskHeight - 1);
                if (lake.mask[static_cast<size_t>(mz) *
                                  lake.maskWidth +
                              mx]) {
                    return lake.level;
                }
            }
            return -1.0e18f;
        };
        vector<f32> raw;
        for (size_t r = 0; r < riverCount; ++r) {
            const River& river = rivers[r];
            const size_t count = river.points.size();
            raw.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const RiverPoint& pt = river.points[i];
                f32 floor = fineAt(pt.x, pt.z);
                const RiverPoint& fw =
                    river.points[i + 1 < count ? i + 1 : i];
                const RiverPoint& bw = river.points[i > 0 ? i - 1 : i];
                const f32 dx = fw.x - bw.x;
                const f32 dz = fw.z - bw.z;
                const f32 len = std::sqrt(dx * dx + dz * dz);
                if (len > 0.5f) {
                    const f32 px = -dz / len;
                    const f32 pz = dx / len;
                    for (const f32 off :
                         { -1.4f, -1.2f, -1.0f, -0.75f, -0.5f, 0.5f,
                           0.75f, 1.0f, 1.2f, 1.4f }) {
                        const f32 d = off * pt.halfWidth;
                        floor = glm::min(
                            floor, fineAt(pt.x + px * d, pt.z + pz * d));
                    }
                }
                raw[i] = floor;
            }
            vector<f32>& f3 = floors[r];
            f3.resize(count);
            for (size_t i = 0; i < count; ++i) {
                f32 lo = raw[i];
                for (size_t k = (i >= 2 ? i - 2 : 0);
                     k <= glm::min(i + 2, count - 1); ++k) {
                    lo = glm::min(lo, raw[k]);
                }
                f3[i] = lo;
            }
            // Forward slope-limited envelope of floor + pond depth:
            // the fill's ceiling. min() keeps it at the local floor's
            // pond level; the recovery term lets it climb back only
            // gently after a low stretch, so the tunnel under a rising
            // ridge holds for the ridge's whole length.
            vector<f32>& bound = bounds[r];
            bound.resize(count);
            if (count > 0) {
                bound[0] = f3[0] + kPoolMax;
                for (size_t i = 1; i < count; ++i) {
                    const f32 ds = std::hypot(
                        river.points[i].x - river.points[i - 1].x,
                        river.points[i].z - river.points[i - 1].z);
                    bound[i] =
                        glm::min(f3[i] + kPoolMax,
                                 bound[i - 1] + kBoundRecover * ds);
                }
            }
            vector<f32>& lv = lakeLvls[r];
            lv.resize(count);
            for (size_t i = 0; i < count; ++i) {
                lv[i] = coveringLakeLevel(river.points[i].x,
                                          river.points[i].z);
            }
            vector<f32>& olds = oldSurfaces[r];
            olds.resize(count);
            for (size_t i = 0; i < count; ++i) {
                olds[i] = river.points[i].surface;
            }
        }
        // Junction links (the data has none): a course's mouth within
        // confluence reach of another course drains INTO it — its
        // boundary level is the receiver's recomputed surface there,
        // so tributaries land ON their stem instead of pooling from
        // their own floor.
        struct Link {
            i32 receiver { -1 };
            size_t recvIdx { 0 };
        };
        vector<Link> links(riverCount);
        for (size_t r = 0; r < riverCount; ++r) {
            if (rivers[r].points.empty()) {
                continue;
            }
            const RiverPoint& tail = rivers[r].points.back();
            f32 bestD2 = 1.0e18f;
            for (size_t o = 0; o < riverCount; ++o) {
                if (o == r || rivers[o].points.empty()) {
                    continue;
                }
                const vector<RiverPoint>& opts = rivers[o].points;
                for (size_t j = 0; j < opts.size(); ++j) {
                    const f32 reach = glm::max(
                        (tail.halfWidth + opts[j].halfWidth) * 2.2f,
                        15.0f);
                    const f32 dx = opts[j].x - tail.x;
                    const f32 dz = opts[j].z - tail.z;
                    const f32 d2 = dx * dx + dz * dz;
                    if (d2 < bestD2 && d2 <= reach * reach) {
                        bestD2 = d2;
                        links[r].receiver = static_cast<i32>(o);
                        links[r].recvIdx = j;
                    }
                }
            }
        }
        // Receivers fill before their tributaries; a coincidental
        // proximity cycle drops the link whose mouth floor sits
        // highest (that course falls back to its own floor).
        vector<u8> filled(riverCount, 0);
        vector<size_t> order;
        order.reserve(riverCount);
        while (order.size() < riverCount) {
            bool progressed = false;
            for (size_t r = 0; r < riverCount; ++r) {
                if (filled[r]) {
                    continue;
                }
                const i32 recv = links[r].receiver;
                if (recv < 0 || filled[static_cast<size_t>(recv)]) {
                    filled[r] = 1;
                    order.push_back(r);
                    progressed = true;
                }
            }
            if (!progressed) {
                size_t cut = riverCount;
                f32 hi = -1.0e18f;
                for (size_t r = 0; r < riverCount; ++r) {
                    if (filled[r] || links[r].receiver < 0) {
                        continue;
                    }
                    const f32 h = floors[r].empty() ? -1.0e18f
                                                    : floors[r].back();
                    if (cut == riverCount || h > hi) {
                        hi = h;
                        cut = r;
                    }
                }
                if (cut == riverCount) {
                    break;
                }
                links[cut].receiver = -1;
            }
        }
        for (const size_t r : order) {
            River& river = rivers[r];
            const size_t count = river.points.size();
            if (count == 0) {
                continue;
            }
            const vector<f32>& f3 = floors[r];
            const vector<f32>& bound = bounds[r];
            // Mouth boundary: the sea for estuaries (the routed trace
            // ended at a sea cell), the receiver's surface at a
            // junction, nothing at a window-border outlet, and a
            // 3-point median at a plain interior stop (one pit at the
            // very mouth must not sink the whole tail).
            const RiverPoint& tail = river.points.back();
            f32 bndry = -1.0e18f;
            const bool seaMouth =
                tail.surface <= hydrology.seaLevel + 0.5f;
            const bool borderMouth =
                tail.x <= out.window.originX + winTexel ||
                tail.x >= winMaxX - winTexel ||
                tail.z <= out.window.originZ + winTexel ||
                tail.z >= winMaxZ - winTexel;
            if (links[r].receiver >= 0) {
                bndry = rivers[static_cast<size_t>(links[r].receiver)]
                            .points[links[r].recvIdx]
                            .surface;
            } else if (!seaMouth && !borderMouth && count >= 3) {
                const f32 a = f3[count - 1];
                const f32 b = f3[count - 2];
                const f32 c = f3[count - 3];
                bndry = glm::max(glm::min(a, b),
                                 glm::min(glm::max(a, b), c)) -
                        kEdgeTuck;
            }
            // The head of a lake outlet never rises above its lake:
            // pooling past the sill would put the river ABOVE the
            // sheet it springs from.
            const bool hasLake =
                river.lakeFed != 0 && river.sourceLake >= 0 &&
                static_cast<size_t>(river.sourceLake) <
                    out.hydro.lakes.size();
            const f32 lakeLevel =
                hasLake ? out.hydro.lakes[static_cast<size_t>(
                                              river.sourceLake)]
                              .level
                        : 0.0f;
            vector<f32> headArc;
            if (hasLake) {
                headArc.resize(count, 0.0f);
                for (size_t k = 1; k < count; ++k) {
                    headArc[k] =
                        headArc[k - 1] +
                        std::hypot(
                            river.points[k].x - river.points[k - 1].x,
                            river.points[k].z - river.points[k - 1].z);
                }
            }
            const vector<f32>& lv = lakeLvls[r];
            f32 level = glm::max(
                glm::max(glm::max(glm::min(f3[count - 1],
                                           bound[count - 1]),
                                  lv[count - 1]) -
                             kEdgeTuck,
                         bndry),
                hydrology.seaLevel);
            river.points[count - 1].surface = level;
            for (size_t i = count - 1; i-- > 0;) {
                const f32 own =
                    glm::max(glm::min(f3[i], bound[i]), lv[i]) -
                    kEdgeTuck;
                f32 next = glm::max(own, level);
                if (hasLake && headArc[i] < 30.0f) {
                    next = glm::min(next, glm::max(lakeLevel, level));
                }
                level = next;
                river.points[i].surface = level;
            }
        }
        // Discovered pools: where the fill pooled the surface above
        // the local floor, the real water body is WIDER than the
        // ribbon — the far water paints it while the sim pin stops at
        // the ribbon footprint (the dry-band defect, measured on
        // ~half the network when pools stayed ribbon-only). A pooled
        // reach IS a lake: it is emitted as a masked natural lake and
        // the existing machinery owns it from there — S5d-bis digs
        // the bed, the nappe renders it far and near, pinLakes
        // reservoirs it, the ribbon is cut under it at emission.
        {
            constexpr f32 kPoolMinRise = 1.0f;
            constexpr size_t kPoolMinRun = 3;
            constexpr f32 kPoolMaskTexel = 8.0f;
            constexpr f32 kPoolMaxReach = 320.0f;
            constexpr u32 kPoolMinCells = 12;
            const auto lakeCovers = [](const Lake& lake, f32 x,
                                       f32 z) {
                if (lake.mask.empty() || lake.maskWidth == 0 ||
                    x < lake.minX || x > lake.maxX || z < lake.minZ ||
                    z > lake.maxZ) {
                    return false;
                }
                const u32 mx = glm::min(
                    static_cast<u32>(glm::max(
                        (x - lake.minX) / lake.maskTexel + 0.5f, 0.0f)),
                    lake.maskWidth - 1);
                const u32 mz = glm::min(
                    static_cast<u32>(glm::max(
                        (z - lake.minZ) / lake.maskTexel + 0.5f, 0.0f)),
                    lake.maskHeight - 1);
                return lake.mask[static_cast<size_t>(mz) *
                                     lake.maskWidth +
                                 mx] != 0;
            };
            const size_t preExistingLakes = out.hydro.lakes.size();
            for (size_t r = 0; r < riverCount; ++r) {
                River& river = rivers[r];
                const size_t count = river.points.size();
                const vector<f32>& f3 = floors[r];
                const vector<f32>& bound = bounds[r];
                size_t i = 0;
                while (i < count) {
                    if (river.points[i].surface - f3[i] <=
                        kPoolMinRise) {
                        ++i;
                        continue;
                    }
                    const f32 level = river.points[i].surface;
                    size_t j = i;
                    while (j + 1 < count &&
                           river.points[j + 1].surface - f3[j + 1] >
                               kPoolMinRise &&
                           std::abs(river.points[j + 1].surface -
                                    level) < 0.01f) {
                        ++j;
                    }
                    const size_t runLen = j - i + 1;
                    const f32 midX =
                        river.points[(i + j) / 2].x;
                    const f32 midZ =
                        river.points[(i + j) / 2].z;
                    bool inExisting = false;
                    for (size_t l = 0; l < preExistingLakes; ++l) {
                        if (lakeCovers(out.hydro.lakes[l], midX,
                                       midZ)) {
                            inExisting = true;
                            break;
                        }
                    }
                    if (runLen < kPoolMinRun || inExisting) {
                        i = j + 1;
                        continue;
                    }
                    // Priority-flood the pool's footprint on an 8 m
                    // world-aligned lattice (deterministic and
                    // seam-stable), bounded to the run's bbox + reach.
                    // Cells flood in ascending ground order; reaching
                    // the bound means the fine terrain offers a drain
                    // the 16 m routing never saw — the pool is FALSE
                    // above that saddle, so the water level is LOWERED
                    // to it (real water would run out there) and only
                    // the contained basin is kept.
                    f32 rMinX = river.points[i].x;
                    f32 rMaxX = rMinX;
                    f32 rMinZ = river.points[i].z;
                    f32 rMaxZ = rMinZ;
                    for (size_t k = i; k <= j; ++k) {
                        rMinX = glm::min(rMinX, river.points[k].x);
                        rMaxX = glm::max(rMaxX, river.points[k].x);
                        rMinZ = glm::min(rMinZ, river.points[k].z);
                        rMaxZ = glm::max(rMaxZ, river.points[k].z);
                    }
                    const i32 gx0 = static_cast<i32>(std::floor(
                        (rMinX - kPoolMaxReach) / kPoolMaskTexel));
                    const i32 gx1 = static_cast<i32>(std::ceil(
                        (rMaxX + kPoolMaxReach) / kPoolMaskTexel));
                    const i32 gz0 = static_cast<i32>(std::floor(
                        (rMinZ - kPoolMaxReach) / kPoolMaskTexel));
                    const i32 gz1 = static_cast<i32>(std::ceil(
                        (rMaxZ + kPoolMaxReach) / kPoolMaskTexel));
                    const i32 gw = gx1 - gx0 + 1;
                    const i32 gh = gz1 - gz0 + 1;
                    vector<u8> seen(static_cast<size_t>(gw) * gh, 0);
                    struct PoolCell {
                        f32 ground;
                        i32 cx;
                        i32 cz;
                        bool operator>(const PoolCell& o) const {
                            return ground > o.ground;
                        }
                    };
                    std::priority_queue<PoolCell, vector<PoolCell>,
                                        std::greater<PoolCell>>
                        heap;
                    const auto push = [&](i32 cx, i32 cz) {
                        if (cx < gx0 || cx > gx1 || cz < gz0 ||
                            cz > gz1) {
                            return;
                        }
                        const size_t idx =
                            static_cast<size_t>(cz - gz0) * gw +
                            static_cast<size_t>(cx - gx0);
                        if (seen[idx]) {
                            return;
                        }
                        seen[idx] = 1;
                        const f32 g = fineAt(
                            static_cast<f32>(cx) * kPoolMaskTexel,
                            static_cast<f32>(cz) * kPoolMaskTexel);
                        if (g >= level - 0.1f) {
                            return;
                        }
                        heap.push({ g, cx, cz });
                    };
                    for (size_t k = i; k <= j; ++k) {
                        push(static_cast<i32>(std::floor(
                                 river.points[k].x / kPoolMaskTexel)),
                             static_cast<i32>(std::floor(
                                 river.points[k].z /
                                 kPoolMaskTexel)));
                    }
                    vector<PoolCell> popped;
                    f32 spill = level;
                    bool escaped = false;
                    while (!heap.empty()) {
                        const PoolCell c = heap.top();
                        heap.pop();
                        if (c.cx == gx0 || c.cx == gx1 ||
                            c.cz == gz0 || c.cz == gz1) {
                            // The lowest way out: everything above
                            // this saddle drains through it.
                            spill = c.ground;
                            escaped = true;
                            break;
                        }
                        popped.push_back(c);
                        push(c.cx - 1, c.cz);
                        push(c.cx + 1, c.cz);
                        push(c.cx, c.cz - 1);
                        push(c.cx, c.cz + 1);
                    }
                    const f32 poolLevel =
                        escaped ? spill - 0.1f : level;
                    if (escaped) {
                        // Re-seat the run on the corrected level: the
                        // monotone max keeps the profile sound, the
                        // pooled plateau simply sits lower.
                        f32 lvl = river.points[j + 1 < count ? j + 1
                                                             : j]
                                      .surface;
                        if (j + 1 >= count) {
                            lvl = -1.0e18f;
                        }
                        const vector<f32>& lvv = lakeLvls[r];
                        for (size_t k = j + 1; k-- > i;) {
                            const f32 own =
                                glm::max(glm::min(f3[k], bound[k]),
                                         lvv[k]) -
                                kEdgeTuck;
                            lvl = glm::max(
                                glm::max(own, glm::min(lvl,
                                                       poolLevel)),
                                hydrology.seaLevel);
                            river.points[k].surface = lvl;
                        }
                        // Upstream of the run keeps monotonicity: its
                        // levels were >= the old pool, which only
                        // dropped.
                        for (size_t k = i; k-- > 0;) {
                            if (river.points[k].surface >= lvl) {
                                break;
                            }
                            lvl = glm::max(river.points[k].surface,
                                           lvl);
                            river.points[k].surface = lvl;
                        }
                    }
                    u32 cells = 0;
                    i32 wMinX = gx1, wMaxX = gx0, wMinZ = gz1,
                        wMaxZ = gz0;
                    for (const PoolCell& c : popped) {
                        if (c.ground >= poolLevel - 0.1f) {
                            continue;
                        }
                        ++cells;
                        wMinX = glm::min(wMinX, c.cx);
                        wMaxX = glm::max(wMaxX, c.cx);
                        wMinZ = glm::min(wMinZ, c.cz);
                        wMaxZ = glm::max(wMaxZ, c.cz);
                    }
                    if (cells >= kPoolMinCells) {
                        // One dry ring around the covered cells: the
                        // mask border is shore (S5d-bis and the mesh
                        // perimeter probes both expect it).
                        Lake pool;
                        pool.dug = 0;
                        pool.level = poolLevel;
                        pool.cells = cells;
                        pool.catchment = river.points[j].area;
                        pool.maskTexel = kPoolMaskTexel;
                        pool.minX = static_cast<f32>(wMinX - 1) *
                                    kPoolMaskTexel;
                        pool.minZ = static_cast<f32>(wMinZ - 1) *
                                    kPoolMaskTexel;
                        pool.maxX = static_cast<f32>(wMaxX + 1) *
                                    kPoolMaskTexel;
                        pool.maxZ = static_cast<f32>(wMaxZ + 1) *
                                    kPoolMaskTexel;
                        pool.maskWidth =
                            static_cast<u32>(wMaxX - wMinX + 3);
                        pool.maskHeight =
                            static_cast<u32>(wMaxZ - wMinZ + 3);
                        pool.mask.assign(
                            static_cast<size_t>(pool.maskWidth) *
                                pool.maskHeight,
                            0);
                        for (const PoolCell& c : popped) {
                            if (c.ground >= poolLevel - 0.1f) {
                                continue;
                            }
                            pool.mask[static_cast<size_t>(
                                          c.cz - wMinZ + 1) *
                                          pool.maskWidth +
                                      static_cast<size_t>(
                                          c.cx - wMinX + 1)] = 1;
                        }
                        out.hydro.lakes.push_back(std::move(pool));
                    }
                    i = j + 1;
                }
            }
        }
        // Cascade coalescing: a real benched descent (lake-outlet
        // pierce ramps, terraced gorges) re-emerges from the fine
        // floor as stacked 5-10 m lips with 8-25 m treads — a
        // staircase of slabs. Successive falls separated by SHORT
        // treads are merged into ONE steep ramp from the cluster's
        // top to its bottom: the carve digs a continuous chute
        // through the benches, and the shader already dresses steep
        // ribbons as whitewater. Endpoints keep their levels, so
        // monotonicity is untouched. Runs AFTER pool discovery (the
        // spill-lowering must see the un-ramped flats) and never
        // ramps through a point a nappe covers: a lake pour-over
        // stays a single frank lip from the sheet.
        {
            constexpr f32 kFallStep = 1.5f;  // a lip worth merging (m)
            constexpr f32 kTreadMax = 30.0f; // tread that still chains
            // Plunge pools: the carve can only DIG from the surface,
            // so a fall's concave foot kept its raw ground and the
            // chute ended on a dry shelf. Each merged cascade emits a
            // dug pond at its foot — S5d digs the dish, the nappe
            // renders it, pinLakes holds it: the existing pond
            // machinery, like the discovered pools.
            constexpr f32 kPlungeTexel = 8.0f;
            vector<Lake> plunges;
            const auto underLake = [&](const RiverPoint& pt) {
                for (const Lake& lake : out.hydro.lakes) {
                    if (lake.mask.empty() || lake.maskWidth == 0 ||
                        pt.x < lake.minX || pt.x > lake.maxX ||
                        pt.z < lake.minZ || pt.z > lake.maxZ) {
                        continue;
                    }
                    const u32 mx = glm::min(
                        static_cast<u32>(glm::max(
                            (pt.x - lake.minX) / lake.maskTexel,
                            0.0f)),
                        lake.maskWidth - 1);
                    const u32 mz = glm::min(
                        static_cast<u32>(glm::max(
                            (pt.z - lake.minZ) / lake.maskTexel,
                            0.0f)),
                        lake.maskHeight - 1);
                    if (lake.mask[static_cast<size_t>(mz) *
                                      lake.maskWidth +
                                  mx]) {
                        return true;
                    }
                }
                return false;
            };
            for (River& river : rivers) {
                const size_t count = river.points.size();
                if (count < 3) {
                    continue;
                }
                vector<u8> wet(count, 0);
                for (size_t k = 0; k < count; ++k) {
                    wet[k] = underLake(river.points[k]) ? 1 : 0;
                }
                vector<f32> arc(count, 0.0f);
                for (size_t k = 1; k < count; ++k) {
                    arc[k] = arc[k - 1] +
                             std::hypot(river.points[k].x -
                                            river.points[k - 1].x,
                                        river.points[k].z -
                                            river.points[k - 1].z);
                }
                size_t k = 0;
                while (k + 1 < count) {
                    if (wet[k] || wet[k + 1] ||
                        river.points[k].surface -
                                river.points[k + 1].surface <
                            kFallStep) {
                        ++k;
                        continue;
                    }
                    // Cluster start: chain further lips while treads
                    // stay short.
                    const size_t top = k;
                    size_t bottom = k + 1;
                    size_t lastLipEnd = k + 1;
                    size_t lips = 1;
                    size_t m = k + 1;
                    while (m + 1 < count && !wet[m + 1] &&
                           arc[m + 1] - arc[lastLipEnd] <= kTreadMax) {
                        if (river.points[m].surface -
                                river.points[m + 1].surface >=
                            kFallStep) {
                            bottom = m + 1;
                            lastLipEnd = m + 1;
                            ++lips;
                        }
                        ++m;
                    }
                    if (lips >= 2) {
                        const f32 sTop = river.points[top].surface;
                        const f32 sBot = river.points[bottom].surface;
                        const f32 span =
                            glm::max(arc[bottom] - arc[top], 0.001f);
                        for (size_t q = top + 1; q < bottom; ++q) {
                            river.points[q].surface =
                                sTop + (sBot - sTop) *
                                           (arc[q] - arc[top]) / span;
                        }
                    }
                    // One plunge pool per fall worth the name (also
                    // the single-lip ones): foot under a sheet keeps
                    // the sheet.
                    if (river.points[top].surface -
                                river.points[bottom].surface >=
                            4.0f &&
                        bottom < count && !wet[bottom]) {
                        const RiverPoint& fp = river.points[bottom];
                        Lake pond;
                        pond.dug = 1;
                        pond.level = fp.surface - 0.1f;
                        const f32 radius =
                            glm::max(fp.halfWidth * 1.2f, 8.0f);
                        pond.minX = fp.x - radius;
                        pond.maxX = fp.x + radius;
                        pond.minZ = fp.z - radius;
                        pond.maxZ = fp.z + radius;
                        pond.maskTexel = kPlungeTexel;
                        pond.maskWidth =
                            static_cast<u32>(std::lround(
                                2.0f * radius / kPlungeTexel)) +
                            1;
                        pond.maskHeight = pond.maskWidth;
                        pond.mask.assign(
                            static_cast<size_t>(pond.maskWidth) *
                                pond.maskHeight,
                            0);
                        pond.cells = 0;
                        for (u32 mr = 0; mr < pond.maskHeight; ++mr) {
                            for (u32 mc = 0; mc < pond.maskWidth;
                                 ++mc) {
                                const f32 px =
                                    pond.minX +
                                    static_cast<f32>(mc) *
                                        kPlungeTexel;
                                const f32 pz =
                                    pond.minZ +
                                    static_cast<f32>(mr) *
                                        kPlungeTexel;
                                if ((px - fp.x) * (px - fp.x) +
                                        (pz - fp.z) * (pz - fp.z) >
                                    radius * radius) {
                                    continue;
                                }
                                // The sheet only covers ground that
                                // can HOLD it (the hydrology ponds'
                                // own clip rule): an unclipped disc
                                // on a fall's slope painted chamfered
                                // slabs and crest skirts over thin
                                // air — "triangles" at every foot
                                // (measured dev).
                                if (fineAt(px, pz) >
                                    pond.level + 0.5f) {
                                    continue;
                                }
                                pond.mask[static_cast<size_t>(mr) *
                                              pond.maskWidth +
                                          mc] = 1;
                                ++pond.cells;
                            }
                        }
                        // A foot with no basin to hold (steep chute
                        // running straight on) gets no sheet at all.
                        if (pond.cells >= 4) {
                            pond.catchment = fp.area;
                            plunges.push_back(std::move(pond));
                        }
                    }
                    k = bottom;
                }
            }
            for (Lake& pond : plunges) {
                out.hydro.lakes.push_back(std::move(pond));
            }
        }
        // Tributaries were filled against their receiver's surface
        // BEFORE the pool re-seat and the cascade coalescing moved it:
        // a lowered stem left hanging lips at confluences. Re-clamp
        // each linked course, receivers first — lowering ONLY, so the
        // ramps and the re-seats the later passes produced survive.
        for (const size_t r : order) {
            if (links[r].receiver < 0 || rivers[r].points.empty()) {
                continue;
            }
            River& river = rivers[r];
            const size_t count = river.points.size();
            const vector<f32>& f3 = floors[r];
            const vector<f32>& bound = bounds[r];
            const vector<f32>& lv = lakeLvls[r];
            f32 down =
                rivers[static_cast<size_t>(links[r].receiver)]
                    .points[links[r].recvIdx]
                    .surface;
            for (size_t k = count; k-- > 0;) {
                const f32 own =
                    glm::max(glm::min(f3[k], bound[k]), lv[k]) -
                    kEdgeTuck;
                const f32 cand = glm::max(
                    glm::max(own, down), hydrology.seaLevel);
                if (river.points[k].surface <= cand + 0.001f) {
                    break; // already at or under: upstream unchanged
                }
                river.points[k].surface = cand;
                down = cand;
            }
        }
        // Seam softening: past the map rect, fineAt reads the
        // NEIGHBOUR'S published ground — already carved — so the
        // re-seat there dug under beds a second time and stepped the
        // water at map lines. Blend the recomputed surface back to
        // the routing surface over the overlap approach; the
        // neighbour's own bake owns those points in ITS rect.
        {
            const f32 rectMinX =
                out.window.originX + params.waterMargin;
            const f32 rectMinZ =
                out.window.originZ + params.waterMargin;
            const f32 rectMaxX = winMaxX - params.waterMargin;
            const f32 rectMaxZ = winMaxZ - params.waterMargin;
            for (size_t r = 0; r < riverCount; ++r) {
                River& river = rivers[r];
                const vector<f32>& olds = oldSurfaces[r];
                for (size_t i = 0; i < river.points.size(); ++i) {
                    RiverPoint& pt = river.points[i];
                    const f32 dOut = glm::max(
                        glm::max(rectMinX - pt.x, pt.x - rectMaxX),
                        glm::max(rectMinZ - pt.z, pt.z - rectMaxZ));
                    if (dOut <= 0.0f) {
                        continue;
                    }
                    f32 w = glm::clamp(dOut / 96.0f, 0.0f, 1.0f);
                    w = w * w * (3.0f - 2.0f * w);
                    pt.surface =
                        glm::mix(pt.surface, olds[i], w);
                }
            }
        }
        // Every dug pond was leveled from the pre-recompute surfaces
        // (makePond: surface - 0.1) — hairpin ponds and orphaned
        // junctions included, not just the re-linked tails: sweep ALL
        // of them onto the lowest recomputed surface that actually
        // runs through their footprint, so no pond disc floats above
        // its own river.
        for (Lake& lake : out.hydro.lakes) {
            if (!lake.dug) {
                continue;
            }
            const f32 m = lake.maskTexel + 8.0f;
            f32 lo = 1.0e18f;
            for (const River& river : rivers) {
                for (const RiverPoint& pt : river.points) {
                    if (pt.x < lake.minX - m || pt.x > lake.maxX + m ||
                        pt.z < lake.minZ - m ||
                        pt.z > lake.maxZ + m) {
                        continue;
                    }
                    lo = glm::min(lo, pt.surface);
                }
            }
            if (lo < 1.0e17f) {
                lake.level = glm::min(lake.level, lo - 0.1f);
            }
        }
        // A ford on a rapids reach would cap the carve to a wading
        // shelf mid-chute: fords stay on quiet water only.
        for (River& river : rivers) {
            if (river.fords.empty() || river.points.size() < 2) {
                continue;
            }
            const vector<RiverPoint>& pts = river.points;
            std::erase_if(river.fords, [&](const Vec2& ford) {
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    const f32 sx = pts[i + 1].x - pts[i].x;
                    const f32 sz = pts[i + 1].z - pts[i].z;
                    const f32 segLen2 =
                        glm::max(sx * sx + sz * sz, 1.0e-6f);
                    const f32 slope =
                        (pts[i].surface - pts[i + 1].surface) /
                        std::sqrt(segLen2);
                    if (slope <= 0.08f) {
                        continue;
                    }
                    const f32 t = glm::clamp(
                        ((ford.x - pts[i].x) * sx +
                         (ford.y - pts[i].z) * sz) /
                            segLen2,
                        0.0f, 1.0f);
                    const f32 dx = ford.x - (pts[i].x + t * sx);
                    const f32 dz = ford.y - (pts[i].z + t * sz);
                    if (dx * dx + dz * dz < 12.0f * 12.0f) {
                        return true;
                    }
                }
                return false;
            });
        }
    }
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
                if (lake.mask.empty() || lake.maskWidth == 0) {
                    continue;
                }
                if (pt.x < lake.minX || pt.x > lake.maxX ||
                    pt.z < lake.minZ || pt.z > lake.maxZ) {
                    continue;
                }
                const u32 mx = glm::min(
                    static_cast<u32>(glm::max(
                        (pt.x - lake.minX) / lake.maskTexel + 0.5f, 0.0f)),
                    lake.maskWidth - 1);
                const u32 mz = glm::min(
                    static_cast<u32>(glm::max(
                        (pt.z - lake.minZ) / lake.maskTexel + 0.5f, 0.0f)),
                    lake.maskHeight - 1);
                if (lake.mask[static_cast<size_t>(mz) *
                                  lake.maskWidth +
                              mx]) {
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
        // ONE mask texel of reach: the original head-to-lake
        // adjacency was 8 m (the hydro grid) — a receded lake whose
        // nearest covered cell sits farther is a COINCIDENCE, not a
        // supply (measured dev at the spawn col: a pocket of the
        // receded lake 32 m from the head, walled off by its own
        // rim, kept the orphan fleuve alive at 2 texels of reach).
        const f32 reach = lake.maskTexel;
        if (x < lake.minX - reach || x > lake.maxX + reach ||
            z < lake.minZ - reach || z > lake.maxZ + reach) {
            continue;
        }
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const f32 px = x + static_cast<f32>(dx) * reach;
                const f32 pz = z + static_cast<f32>(dz) * reach;
                if (px < lake.minX || px > lake.maxX ||
                    pz < lake.minZ || pz > lake.maxZ) {
                    continue;
                }
                const u32 mx = glm::min(
                    static_cast<u32>(glm::max(
                        (px - lake.minX) / lake.maskTexel + 0.5f,
                        0.0f)),
                    lake.maskWidth - 1);
                const u32 mz = glm::min(
                    static_cast<u32>(glm::max(
                        (pz - lake.minZ) / lake.maskTexel + 0.5f,
                        0.0f)),
                    lake.maskHeight - 1);
                if (lake.mask[static_cast<size_t>(mz) *
                                  lake.maskWidth +
                              mx] != 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace render::terraingen
