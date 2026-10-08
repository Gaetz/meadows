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
        lap("borders+dimple");
        imprintMasterChannels(out.sim, macro, imprintKeep, controls,
                              macroParams, network, params.imprint,
                              params.hydrology.widthCoef,
                              params.hydrology.widthExponent,
                              params.hydrology.fleuveWidthScale);
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
        // A plateau's escarpment is a designed wall too: dissected
        // lightly, never carved to the plain (a carve that depended on
        // the window's drainage).
        if (i < macro.scarp.size()) {
            cut = glm::mix(cut, 4.0f * params.calmCut, macro.scarp[i]);
        }
        if (imprintKeep[i] > 0.0f || ridgeFactor[i] > 0.2f) {
            cut = params.roughCut;
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

MapHydrology extractMapHydrology(const TileBakeParams& params,
                                 const TileStage1& mapS1, i32 mapX,
                                 i32 mapZ, i32 tilesPerSide,
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
    out.ground.resize(out.window.cells());
    for (u32 row = 0; row < out.window.n; ++row) {
        for (u32 col = 0; col < out.window.n; ++col) {
            out.ground[static_cast<size_t>(row) * out.window.n + col] =
                bilinearWorld(mapS1.sim, mapS1.eroded,
                              out.window.x(col), out.window.z(row));
        }
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return out; // partial: caller discards
    }
    HydrologyParams hydrology = params.hydrology;
    hydrology.seaLevel = params.macro.seaLevel;
    out.hydro = extractHydrology(out.window, out.ground, hydrology);
    // Tier classification (S4 tail): the master network's TRUE drainage
    // areas, once for the whole map.
    {
        ProceduralControlParams cp = params.controls;
        cp.seed = params.worldSeed;
        MasterNetworkParams network = params.network;
        network.seaLevel = params.macro.seaLevel;
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
    return out;
}

TileBakeResult bakeMapSlice(const TileBakeParams& params, i32 tx,
                            i32 tz, const TileStage1& mapS1,
                            const MapHydrology& mapHydro,
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

    // --- Finalize the slice against the SHARED map surface and
    // hydrology (the slice finalize prologue, self = the map).
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
    if (cancelled()) {
        return {};
    }
    const FinalizeResult fine = finalizeTerrain(
        sim, self.eroded, macroFields, hydro, window, finalize,
        params.worldSeed,
        character.fineScale.empty() ? nullptr : &character.fineScale,
        self.deposit.empty() ? nullptr : &self.deposit, cancel);

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
    // slice holding its bbox center, full stop (no anchor machinery:
    // every slice sees the same basin at the same level). Rivers are
    // clipped to this slice's kept rect as in the windowed path.
    const f32 tileMaxX = tileMinX + params.tileSize;
    const f32 tileMaxZ = tileMinZ + params.tileSize;
    for (const Lake& lake : hydro.lakes) {
        const f32 cx = (lake.minX + lake.maxX) * 0.5f;
        const f32 cz = (lake.minZ + lake.maxZ) * 0.5f;
        if (cx < tileMinX || cx >= tileMaxX || cz < tileMinZ ||
            cz >= tileMaxZ) {
            continue;
        }
        out.lakes.push_back(lake);
    }
    // Re-validate against the FINAL ground; outside this slice's rect
    // the coarse map surface stands in for the wall, so a cross-slice
    // lake keeps its far half.
    reconcileLakesWithTerrain(out.lakes, out.region, &mapHydro.window,
                              &mapHydro.ground);
    vector<Lake> allLakes = hydro.lakes;
    reconcileLakesWithTerrain(allLakes, out.region, &mapHydro.window,
                              &mapHydro.ground);
    const f32 keepMaxX = keepMinX + keepSpan;
    const f32 keepMaxZ = keepMinZ + keepSpan;
    const auto inKeep = [&](const RiverPoint& pt) {
        return pt.x >= keepMinX && pt.x <= keepMaxX && pt.z >= keepMinZ &&
               pt.z <= keepMaxZ;
    };
    for (const River& river : hydro.rivers) {
        if (river.lakeFed != 0 && !river.points.empty() &&
            !lakeReachesPoint(allLakes, river.points.front().x,
                              river.points.front().z)) {
            continue;
        }
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
        River run;
        run.tier = river.tier;
        for (const RiverPoint& pt : river.points) {
            if (inKeep(pt)) {
                run.points.push_back(pt);
                continue;
            }
            if (run.points.size() >= 2) {
                emit(std::move(run));
                run = River {};
            }
            run.points.clear();
            run.tier = river.tier;
        }
        if (run.points.size() >= 2) {
            emit(std::move(run));
        }
    }
    reconcileRiversWithTerrain(out.rivers, out.lakes, out.region,
                               params.finalize);
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
    const MapHydrology hydro =
        extractMapHydrology(solo, mapS1, tx, tz, 1, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return {};
    }
    return bakeMapSlice(solo, tx, tz, mapS1, hydro, cancel);
}

namespace {

// Bilinear FINAL ground from the published region; outside its rect
// = +inf wall — both reconcile passes stay conservative there.
f32 finalGroundAt(const render::TerrainRegion& region, f32 x, f32 z) {
    const f32 fx = (x - region.originX) / region.texelSize;
    const f32 fz = (z - region.originZ) / region.texelSize;
    if (fx < 0.0f || fz < 0.0f ||
        fx > static_cast<f32>(region.width) - 1.001f ||
        fz > static_cast<f32>(region.height) - 1.001f) {
        return 1.0e9f;
    }
    const u32 c = static_cast<u32>(fx);
    const u32 r = static_cast<u32>(fz);
    const f32 tx = fx - static_cast<f32>(c);
    const f32 tz = fz - static_cast<f32>(r);
    const auto at = [&](u32 cc, u32 rr) {
        return region.heights[static_cast<size_t>(rr) * region.width +
                              cc];
    };
    return glm::mix(glm::mix(at(c, r), at(c + 1, r), tx),
                    glm::mix(at(c, r + 1), at(c + 1, r + 1), tx), tz);
}

} // namespace

void reconcileLakesWithTerrain(vector<Lake>& lakes,
                               const render::TerrainRegion& region,
                               const GridSpec* fallbackSpec,
                               const vector<f32>* fallbackGround) {
    if (region.heights.empty() || region.width < 2 ||
        region.height < 2 || region.texelSize <= 0.0f) {
        return;
    }
    const auto groundAt = [&](f32 x, f32 z) {
        const f32 g = finalGroundAt(region, x, z);
        if (g < 0.9e9f || !fallbackSpec || !fallbackGround) {
            return g;
        }
        // Outside the published rect: the coarse map surface keeps the
        // flood honest there instead of walling it — a cross-slice
        // lake keeps the half beyond its owner's rect.
        const f32 maxX =
            fallbackSpec->originX +
            static_cast<f32>(fallbackSpec->n - 1) *
                fallbackSpec->texelSize;
        const f32 maxZ =
            fallbackSpec->originZ +
            static_cast<f32>(fallbackSpec->n - 1) *
                fallbackSpec->texelSize;
        if (x < fallbackSpec->originX || x > maxX ||
            z < fallbackSpec->originZ || z > maxZ) {
            return g; // beyond the map window too: the wall stands
        }
        return bilinearWorld(*fallbackSpec, *fallbackGround, x, z);
    };
    vector<Lake> split; // extra enclosure components, appended after
    for (size_t li = 0; li < lakes.size();) {
        Lake& lake = lakes[li];
        const i32 mw = static_cast<i32>(lake.maskWidth);
        const i32 mh = static_cast<i32>(lake.maskHeight);
        if (lake.mask.empty() || mw <= 0 || mh <= 0) {
            ++li;
            continue; // maskless authored pond: not this pass's call
        }
        const f32 mt = lake.maskTexel;
        // Flood window = the mask bbox PADDED well past it: the bbox
        // bounds the WATERLINE, so the enclosing rim lies just
        // outside it — an unpadded flood saw escapes all along the
        // shoreline and drained the whole lake (bench: 1 of 8954
        // cells kept). Beyond the published rect groundAt returns a
        // wall (conservative).
        const i32 pad = 16;
        const i32 w = mw + 2 * pad;
        const i32 h = mh + 2 * pad;
        const f32 wx0 = lake.minX - static_cast<f32>(pad) * mt;
        const f32 wz0 = lake.minZ - static_cast<f32>(pad) * mt;
        const size_t cells = static_cast<size_t>(w) * h;
        vector<f32> ground(cells);
        for (i32 r = 0; r < h; ++r) {
            for (i32 c = 0; c < w; ++c) {
                ground[static_cast<size_t>(r) * w + c] =
                    groundAt(wx0 + static_cast<f32>(c) * mt,
                             wz0 + static_cast<f32>(r) * mt);
            }
        }
        const auto pIdx = [&](i32 mc, i32 mr) {
            return static_cast<size_t>(mr + pad) * w +
                   static_cast<size_t>(mc + pad);
        };
        // Priority flood from the window border: fill[i] = the level
        // water AT i must reach to escape — per-cell ENCLOSURE on the
        // final ground.
        vector<f32> fill(cells, 1.0e9f);
        vector<u8> seen(cells, 0);
        using Node = std::pair<f32, u32>;
        std::priority_queue<Node, vector<Node>, std::greater<Node>>
            heap;
        const auto seed = [&](size_t i) {
            if (!seen[i]) {
                fill[i] = ground[i];
                seen[i] = 1;
                heap.push({ fill[i], static_cast<u32>(i) });
            }
        };
        for (i32 c = 0; c < w; ++c) {
            seed(static_cast<size_t>(c));
            seed(static_cast<size_t>(h - 1) * w + c);
        }
        for (i32 r = 0; r < h; ++r) {
            seed(static_cast<size_t>(r) * w);
            seed(static_cast<size_t>(r) * w + (w - 1));
        }
        const i32 dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 },
                                 { 0, -1 } };
        while (!heap.empty()) {
            const Node top = heap.top();
            heap.pop();
            const size_t i = top.second;
            if (top.first > fill[i] + 1.0e-6f) {
                continue; // stale heap entry
            }
            const i32 c = static_cast<i32>(i % static_cast<size_t>(w));
            const i32 r = static_cast<i32>(i / static_cast<size_t>(w));
            for (const auto& d : dirs) {
                const i32 nc = c + d[0];
                const i32 nr = r + d[1];
                if (nc < 0 || nr < 0 || nc >= w || nr >= h) {
                    continue;
                }
                const size_t j = static_cast<size_t>(nr) * w +
                                 static_cast<size_t>(nc);
                const f32 nf = glm::max(ground[j], fill[i]);
                if (!seen[j] || nf < fill[j] - 1.0e-6f) {
                    fill[j] = nf;
                    seen[j] = 1;
                    heap.push({ nf, static_cast<u32>(j) });
                }
            }
        }
        // COMPONENT loop (brick 3): the hydrology mask can hold
        // several real sub-basins the carves separated — the old
        // single re-cut kept only the deepest one and silently lost
        // the others' water. Each pass claims one enclosure component
        // among the still-PENDING original cells (its own volume-max
        // level, its own guards); the first refits the lake in place,
        // the rest are emitted as their own lakes (tight bbox).
        vector<u8> pending(lake.mask);
        // Window-level claim: a later (lower) component must neither
        // mark nor re-walk cells an earlier one already owns — two
        // overlapping masks at different levels would make the
        // runtime dedup drop one of them.
        vector<u8> claimedWin(cells, 0);
        vector<Lake> pieces;
        constexpr u32 kMaxComponents = 4;
        for (u32 comp = 0; comp < kMaxComponents; ++comp) {
            // Level = the candidate (a pending cell's fill, floored
            // to 0.25 m, capped at the old level) retaining the
            // LARGEST WATER VOLUME over the pending cells. The
            // max-fill cell kept a one-cell perched pocket and
            // dropped the lake; the deepest claimed cell was a
            // phantom-finger cell down the gorge and dragged the
            // level to the canyon floor (both bench-measured on the
            // real lake 5).
            vector<f32> cands;
            for (i32 r = 0; r < mh; ++r) {
                for (i32 c = 0; c < mw; ++c) {
                    if (pending[static_cast<size_t>(r) * mw + c] ==
                        0) {
                        continue;
                    }
                    // Clamped to the OLD level: a basin still fully
                    // enclosed above it keeps its baked level (fills
                    // above the claim are not an excuse to drop it).
                    const f32 fv =
                        glm::min(fill[pIdx(c, r)], lake.level);
                    if (fv < 1.0e8f) {
                        cands.push_back(std::floor(fv * 4.0f) * 0.25f);
                    }
                }
            }
            std::sort(cands.begin(), cands.end());
            cands.erase(std::unique(cands.begin(), cands.end()),
                        cands.end());
            f32 level = -1.0e9f;
            f64 bestVol = 0.0;
            for (const f32 cand : cands) {
                f64 vol = 0.0;
                for (i32 r = 0; r < mh; ++r) {
                    for (i32 c = 0; c < mw; ++c) {
                        if (pending[static_cast<size_t>(r) * mw +
                                    c] == 0) {
                            continue;
                        }
                        const size_t i = pIdx(c, r);
                        if (fill[i] >= cand - 0.01f &&
                            ground[i] < cand) {
                            vol += static_cast<f64>(cand - ground[i]);
                        }
                    }
                }
                if (vol > bestVol) {
                    bestVol = vol;
                    level = cand;
                }
            }
            level = glm::min(level, lake.level);
            // Deepest ENCLOSED pending cell = this component's BFS
            // root; a basin breached below ~0.5 m of real depth ends
            // the loop (the remaining pockets are shallower still).
            size_t root = 0;
            f32 rootGround = 1.0e9f;
            for (i32 r = 0; r < mh; ++r) {
                for (i32 c = 0; c < mw; ++c) {
                    if (pending[static_cast<size_t>(r) * mw + c] ==
                        0) {
                        continue;
                    }
                    const size_t i = pIdx(c, r);
                    if (fill[i] >= level - 0.01f &&
                        ground[i] < rootGround) {
                        rootGround = ground[i];
                        root = i;
                    }
                }
            }
            if (bestVol <= 0.0 || rootGround > 1.0e8f ||
                level - rootGround < 0.5f) {
                break;
            }
            // Re-cut: a cell belongs to the component iff it is UNDER
            // the level AND enclosed at it (fill >= level — mere
            // "connected under the level" painted a flat sheet down
            // the descending gorge: baked water on the hillside,
            // measured dev), connected to the root, and inside the
            // stored bbox.
            vector<u8> mask(static_cast<size_t>(mw) * mh, 0);
            vector<u32> stack;
            stack.push_back(static_cast<u32>(root));
            vector<u8> visited(cells, 0);
            visited[root] = 1;
            u32 kept = 0;
            while (!stack.empty()) {
                const size_t i = stack.back();
                stack.pop_back();
                const i32 c =
                    static_cast<i32>(i % static_cast<size_t>(w));
                const i32 r =
                    static_cast<i32>(i / static_cast<size_t>(w));
                const i32 mc = c - pad;
                const i32 mr = r - pad;
                if (mc >= 0 && mr >= 0 && mc < mw && mr < mh) {
                    mask[static_cast<size_t>(mr) * mw + mc] = 1;
                    ++kept;
                }
                for (const auto& d : dirs) {
                    const i32 nc = c + d[0];
                    const i32 nr = r + d[1];
                    if (nc < 0 || nr < 0 || nc >= w || nr >= h) {
                        continue;
                    }
                    const size_t j = static_cast<size_t>(nr) * w +
                                     static_cast<size_t>(nc);
                    if (visited[j] == 0 && claimedWin[j] == 0 &&
                        ground[j] < level - 0.02f &&
                        fill[j] >= level - 0.01f) {
                        visited[j] = 1;
                        stack.push_back(static_cast<u32>(j));
                    }
                }
            }
            if (kept == 0) {
                break;
            }
            for (size_t i = 0; i < cells; ++i) {
                if (visited[i] != 0) {
                    claimedWin[i] = 1;
                }
            }
            // Consume the claimed cells (and the root's own cell —
            // guaranteed masked, so every pass shrinks pending).
            u32 consumed = 0;
            for (size_t m = 0; m < mask.size(); ++m) {
                if (mask[m] != 0 && pending[m] != 0) {
                    pending[m] = 0;
                    ++consumed;
                }
            }
            // Recovered SUB-basins under ~0.1 ha are specks (the B8
            // ruling: no puddle-sized basins) — consumed but not
            // emitted. The main component keeps v67 parity whatever
            // its size.
            if (pieces.empty() || kept >= 4) {
                Lake piece = lake;
                piece.level = level;
                piece.mask = std::move(mask);
                piece.cells = kept;
                pieces.push_back(std::move(piece));
            }
            if (consumed == 0) {
                break; // safety: never loop without progress
            }
        }
        if (pieces.empty()) {
            lakes.erase(lakes.begin() + static_cast<i32>(li));
            continue;
        }
        // First (largest-volume) component refits the lake in place —
        // bit-identical to the pre-split behavior when the mask holds
        // a single basin. Extra components get a TIGHT bbox (their
        // shared-dims mask is mostly empty) and join the list after
        // the outer loop (never re-processed).
        lake = std::move(pieces.front());
        for (size_t p = 1; p < pieces.size(); ++p) {
            Lake& piece = pieces[p];
            i32 c0 = mw, c1 = -1, r0 = mh, r1 = -1;
            for (i32 r = 0; r < mh; ++r) {
                for (i32 c = 0; c < mw; ++c) {
                    if (piece.mask[static_cast<size_t>(r) * mw + c] !=
                        0) {
                        c0 = glm::min(c0, c);
                        c1 = glm::max(c1, c);
                        r0 = glm::min(r0, r);
                        r1 = glm::max(r1, r);
                    }
                }
            }
            const i32 nw = c1 - c0 + 1;
            const i32 nh = r1 - r0 + 1;
            vector<u8> cropped(static_cast<size_t>(nw) * nh, 0);
            for (i32 r = 0; r < nh; ++r) {
                for (i32 c = 0; c < nw; ++c) {
                    cropped[static_cast<size_t>(r) * nw + c] =
                        piece.mask[static_cast<size_t>(r + r0) * mw +
                                   (c + c0)];
                }
            }
            piece.minX += static_cast<f32>(c0) * mt;
            piece.minZ += static_cast<f32>(r0) * mt;
            piece.maxX = piece.minX + static_cast<f32>(nw - 1) * mt;
            piece.maxZ = piece.minZ + static_cast<f32>(nh - 1) * mt;
            piece.maskWidth = static_cast<u32>(nw);
            piece.maskHeight = static_cast<u32>(nh);
            piece.mask = std::move(cropped);
            split.push_back(std::move(piece));
        }
        ++li;
    }
    lakes.insert(lakes.end(), std::make_move_iterator(split.begin()),
                 std::make_move_iterator(split.end()));
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

void reconcileRiversWithTerrain(vector<River>& rivers,
                                const vector<Lake>& lakes,
                                const render::TerrainRegion& region,
                                const FinalizeParams& finalize) {
    if (region.heights.empty() || region.width < 2 ||
        region.height < 2 || region.texelSize <= 0.0f) {
        return;
    }
    // Mirror of LakeSurface::covers on the bake-side Lake.
    const auto inLake = [](const Lake& lake, f32 x, f32 z) {
        if (x < lake.minX || x > lake.maxX || z < lake.minZ ||
            z > lake.maxZ) {
            return false;
        }
        if (lake.mask.empty() || lake.maskWidth == 0) {
            return true;
        }
        const u32 mx = glm::min(
            static_cast<u32>(glm::max(
                (x - lake.minX) / lake.maskTexel + 0.5f, 0.0f)),
            lake.maskWidth - 1);
        const u32 mz = glm::min(
            static_cast<u32>(glm::max(
                (z - lake.minZ) / lake.maskTexel + 0.5f, 0.0f)),
            lake.maskHeight - 1);
        return lake.mask[static_cast<size_t>(mz) * lake.maskWidth +
                         mx] != 0;
    };
    for (River& river : rivers) {
        const f32 tierCap =
            river.tier == 0
                ? finalize.streamDepthMax
                : (river.tier == 1 ? finalize.riverDepthMax
                                   : finalize.fleuveDepthMax);
        // The profile is REBUILT from the mouth UPSTREAM: surface =
        // final bed + the tier's carved depth, RAISED by the
        // downstream water (backwater — a rising lip pools the water
        // behind it). A downstream running-MIN did the opposite: one
        // node clamped low over a local pothole dragged the whole
        // reach UNDER every bed rise — a torrent flowing inside its
        // channel walls, invisible from above, blue screen below
        // (measured dev at (9622, 4131)). Monotone downhill holds by
        // construction and the water always rides ABOVE its bed.
        f32 next = -1.0e9f;
        for (size_t k = river.points.size(); k-- > 0;) {
            RiverPoint& pt = river.points[k];
            const f32 ground = finalGroundAt(region, pt.x, pt.z);
            f32 s;
            if (ground < 1.0e8f) {
                const f32 bed = glm::clamp(
                    finalize.riverDepthCoef * 2.0f * pt.halfWidth,
                    finalize.riverDepthMin, tierCap);
                s = ground + bed;
            } else {
                s = pt.surface; // outside the rect: keep the bake
            }
            for (const Lake& lake : lakes) {
                if (inLake(lake, pt.x, pt.z)) {
                    // Crossing a reconciled lake: the surface there
                    // IS the lake's (possibly lowered) level.
                    s = lake.level;
                    break;
                }
            }
            s = glm::max(s, next); // backwater from downstream
            next = s;
            pt.surface = s;
        }
    }
}

} // namespace render::terraingen
