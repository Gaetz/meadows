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
    macroParams.hillChainWavelength = controlParams.hillChainWavelength;
    MacroResult macro =
        synthesizeMacro(controls, out.sim, macroParams, params.worldSeed);
    // The fleuve imprint — the "authored -> S1 before erosion" slot: the
    // master courses carve their channel, plain and monotone bed into
    // the macro BEFORE the fastscape, which then sculpts around them
    // (imprintKeep). The apron makes neighbours stamp identically.
    vector<f32> imprintKeep(out.sim.cells(), 0.0f);
    {
        MasterNetworkParams network = params.network;
        network.seaLevel = params.macro.seaLevel;
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
            character.erodibility[i] *= 1.0f + plain;
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
            // Calm socles equilibrate flat and shed gentle: HIGHER
            // erodibility lowers the fastscape equilibrium slope
            // (S = U/(k*A^m) — same direction as the corridor and
            // plain factors), a softer talus rounds what remains.
            // LOW socles only — high calm ground is protected by the
            // kCalmKeep instead (its keep is often already capped, so
            // extra erodibility would only pull the plateau down).
            if (i < macro.calm.size()) {
                const f32 calm = macro.calm[i];
                const f32 low =
                    1.0f - glm::smoothstep(
                               150.0f, 400.0f,
                               macro.height[i] - params.macro.seaLevel);
                character.erodibility[i] *= 1.0f + 0.8f * calm * low;
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
    for (size_t i = 0; i < macro.plateau.size(); ++i) {
        // High calm socles resist the carve too (kCalmKeep) — the
        // habitable high ground survives; the analytic mirror in
        // macroHeightAnalytic applies the same formula.
        const f32 calmHigh =
            macro.calm[i] *
            glm::smoothstep(150.0f, 400.0f,
                            macro.height[i] - params.macro.seaLevel);
        keep[i] = glm::min(kPlateauKeepMax,
                           macro.plateau[i] * kPlateauKeepCoef +
                               calmHigh * kCalmKeep);
        if (!crest.empty()) {
            keep[i] *= glm::mix(1.0f - params.keepCrestFade, 1.0f,
                                crest[i]);
        }
        // The imprinted fleuve channel/plain resists the fastscape: the
        // constructed course must survive erosion like a pad would.
        keep[i] = glm::max(keep[i], imprintKeep[i]);
    }
    const FluvialResult eroded = erodeFluvial(
        out.sim, macro.height, macro.uplift, fluvial,
        keep.empty() ? nullptr : &keep,
        character.erodibility.empty() ? nullptr : &character.erodibility,
        character.capacityScale.empty() ? nullptr
                                        : &character.capacityScale,
        cancel);
    if (cancelled()) {
        out.eroded = eroded.height;
        return out; // partial, discarded by the caller
    }

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
        out.eroded =
            roundRidges(out.sim, out.eroded, rounding, &crestWeight);
    }
    // One sediment field: thermal scree + fluvial alluvium.
    out.deposit = std::move(relaxed.deposit);
    if (!eroded.deposit.empty()) {
        for (size_t i = 0; i < out.deposit.size(); ++i) {
            out.deposit[i] += eroded.deposit[i];
        }
    }
    // Calm relaxation: the socle family is calm at the COARSE scale
    // too — the erodibility hooks only tilt the fastscape slopes, they
    // never remove the ravines. Blend toward the ~160 m box mean where
    // the CONTROL family claims the ground (the derived valley-floor
    // calm would be circular here); versants keep full dissection.
    // Sibling of roundRidges: a weighted relaxation, gathers only.
    {
        vector<f32> mean = out.eroded;
        for (u32 pass = 0; pass < 10; ++pass) {
            mean = boxBlur3(out.sim, mean);
        }
        for (size_t i = 0; i < out.eroded.size(); ++i) {
            if (out.eroded[i] <= params.macro.seaLevel) {
                continue;
            }
            const f32 gate =
                params.relaxGateHigh > 0.0f
                    ? glm::smoothstep(params.relaxGateLow,
                                      params.relaxGateHigh,
                                      macro.calm[i])
                    : macro.calm[i];
            out.eroded[i] =
                glm::mix(out.eroded[i], mean[i], 0.75f * gate);
        }
    }
    // Valley floors join the calm-socle family here: they only exist
    // after erosion carved them. A floor = low local relief (mean
    // absolute deviation from a ~160 m box mean) and not the pit of a
    // lake basin (those belong to the water). Pure gathers — the
    // stage-1 determinism contract holds.
    out.calm = macro.calm;
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
    return out;
}

TileBakeResult bakeTileStage2(
    const TileBakeParams& params, i32 tx, i32 tz,
    const std::function<const TileStage1*(i32, i32)>& stage1At,
    const std::atomic<bool>* cancel) {
    const auto cancelled = [cancel] {
        return cancel && cancel->load(std::memory_order_relaxed);
    };
    const f32 tileMinX = static_cast<f32>(tx) * params.tileSize;
    const f32 tileMinZ = static_cast<f32>(tz) * params.tileSize;
    const TileStage1* center = stage1At(tx, tz);
    // The center stage-1 is the contract; without it there is nothing
    // to finalize.
    ENGINE_ASSERT_MSG(center != nullptr,
                      "bakeTileStage2: center stage-1 is mandatory");
    if (!center) {
        return {};
    }
    const TileStage1& self = *center;
    const GridSpec& sim = self.sim;

    const TileStage1* neighbours[3][3];
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            neighbours[dz + 1][dx + 1] = stage1At(tx + dx, tz + dz);
        }
    }
    const f32 rectMargin = params.overlapMargin;
    const f32 blend = 2.0f * params.overlapMargin;
    // Composite builder: blend the 3x3 stage-1 sims with the SAME
    // weights the runtime uses for the published regions
    // (tile+overlapMargin rects, edgeBlend = 2*overlapMargin). Both
    // neighbours of a border compose the same surface — their water
    // agrees by construction. Reused by the hydrology window AND the
    // wider canonical-basin flood below.
    const auto composeWindow = [&](const GridSpec& win) {
        vector<f32> grid(win.cells());
        for (u32 row = 0; row < win.n; ++row) {
            for (u32 col = 0; col < win.n; ++col) {
                const f32 wx = win.x(col);
                const f32 wz = win.z(row);
                f32 wSum = 0.0f;
                f32 hSum = 0.0f;
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const TileStage1* s1 = neighbours[dz + 1][dx + 1];
                        if (!s1) {
                            continue;
                        }
                        const f32 rMinX =
                            static_cast<f32>(tx + dx) * params.tileSize -
                            rectMargin;
                        const f32 rMinZ =
                            static_cast<f32>(tz + dz) * params.tileSize -
                            rectMargin;
                        const f32 rMaxX = rMinX + params.tileSize +
                                          2.0f * rectMargin;
                        const f32 rMaxZ = rMinZ + params.tileSize +
                                          2.0f * rectMargin;
                        if (wx < rMinX || wx > rMaxX || wz < rMinZ ||
                            wz > rMaxZ) {
                            continue;
                        }
                        const f32 d = glm::min(
                            glm::min(wx - rMinX, rMaxX - wx),
                            glm::min(wz - rMinZ, rMaxZ - wz));
                        const f32 t = glm::clamp(d / blend, 0.0f, 1.0f);
                        const f32 w = t * t * (3.0f - 2.0f * t);
                        if (w <= 0.0f) {
                            continue;
                        }
                        hSum +=
                            w * bilinearWorld(s1->sim, s1->eroded, wx, wz);
                        wSum += w;
                    }
                }
                grid[static_cast<size_t>(row) * win.n + col] =
                    wSum > 0.0f ? hSum / wSum
                                : bilinearWorld(sim, self.eroded, wx, wz);
            }
        }
        return grid;
    };
    // Hydrology window: tile + waterMargin.
    GridSpec window;
    window.texelSize = params.macroTexel;
    window.originX = tileMinX - params.waterMargin;
    window.originZ = tileMinZ - params.waterMargin;
    window.n = texels(params.tileSize + 2.0f * params.waterMargin,
                      params.macroTexel) +
               1;
    const vector<f32> composite = composeWindow(window);
    if (cancelled()) {
        return {}; // shutdown: nothing published
    }

    HydrologyParams hydrology = params.hydrology;
    hydrology.seaLevel = params.macro.seaLevel;
    HydrologyResult hydro = extractHydrology(window, composite, hydrology);
    // Tier classification (S4 tail): fleuve promotion needs the master
    // network's TRUE drainage areas — pure per super cell, identical
    // for every neighbour that sees the same course.
    {
        ProceduralControlParams cp = params.controls;
        cp.seed = params.worldSeed;
        MasterNetworkParams network = params.network;
        network.seaLevel = params.macro.seaLevel;
        const f32 windowMaxX =
            window.originX +
            static_cast<f32>(window.n - 1) * window.texelSize;
        const f32 windowMaxZ =
            window.originZ +
            static_cast<f32>(window.n - 1) * window.texelSize;
        const vector<MasterRiver> master = masterRiversNear(
            ProceduralControls { cp }, params.macro, network,
            window.originX, window.originZ, windowMaxX, windowMaxZ);
        classifyRivers(hydro.rivers, hydrology, params.worldSeed, master);
    }

    // --- Canonical basin resolution. A lake whose mask touches the
    // window rim is a TRUNCATED view of a larger basin: its spill level
    // is a window artifact, and the two neighbours would each publish
    // their own version at different levels (the stacked-sheets bug).
    // Re-flood those basins on a WIDER composite and adopt the wide
    // result; ownership moves from the bbox center to the basin's
    // DEEPEST cell — deterministic and identical on both sides as long
    // as the basin fits the wide window (beyond that, the publish-side
    // overlap suppression is the safety net).
    vector<u8> preOwned; // parallel to hydro.lakes once resolution ran
    {
        const f32 windowMaxX =
            window.originX +
            static_cast<f32>(window.n - 1) * window.texelSize;
        const f32 windowMaxZ =
            window.originZ +
            static_cast<f32>(window.n - 1) * window.texelSize;
        const auto touchesRim = [&](const Lake& lake) {
            const f32 t = window.texelSize * 1.5f;
            return lake.minX <= window.originX + t ||
                   lake.minZ <= window.originZ + t ||
                   lake.maxX >= windowMaxX - t ||
                   lake.maxZ >= windowMaxZ - t;
        };
        bool anyOpen = false;
        for (const Lake& lake : hydro.lakes) {
            anyOpen = anyOpen || touchesRim(lake);
        }
        if (anyOpen) {
            GridSpec wide;
            wide.texelSize = params.macroTexel;
            wide.originX = tileMinX - kBasinResolveMargin;
            wide.originZ = tileMinZ - kBasinResolveMargin;
            wide.n = texels(params.tileSize + 2.0f * kBasinResolveMargin,
                            params.macroTexel) +
                     1;
            const vector<f32> wideComposite = composeWindow(wide);
            const vector<f32> wideFilled =
                priorityFloodFill(wide, wideComposite, hydrology.seaLevel,
                                  hydrology.minSlope);
            const vector<Lake> wideLakes = extractLakes(
                wide, wideComposite, wideFilled, hydrology);
            // Deepest mask cell of a lake on its grid (row-major
            // tie-break: deterministic).
            const auto deepestOf = [](const GridSpec& spec,
                                      const vector<f32>& ground,
                                      const Lake& lake) {
                Vec2 best { lake.minX, lake.minZ };
                f32 lowest = 1.0e30f;
                for (u32 mz = 0; mz < lake.maskHeight; ++mz) {
                    for (u32 mx = 0; mx < lake.maskWidth; ++mx) {
                        if (!lake.mask[static_cast<size_t>(mz) *
                                           lake.maskWidth +
                                       mx]) {
                            continue;
                        }
                        const f32 wx = lake.minX +
                                       static_cast<f32>(mx) *
                                           lake.maskTexel;
                        const f32 wz = lake.minZ +
                                       static_cast<f32>(mz) *
                                           lake.maskTexel;
                        const i32 col = static_cast<i32>(std::lround(
                            (wx - spec.originX) / spec.texelSize));
                        const i32 row = static_cast<i32>(std::lround(
                            (wz - spec.originZ) / spec.texelSize));
                        if (col < 0 || row < 0 ||
                            col >= static_cast<i32>(spec.n) ||
                            row >= static_cast<i32>(spec.n)) {
                            continue;
                        }
                        const f32 h =
                            ground[static_cast<size_t>(row) * spec.n +
                                   col];
                        if (h < lowest) {
                            lowest = h;
                            best = { wx, wz };
                        }
                    }
                }
                return best;
            };
            const auto lakeCovers = [](const Lake& lake, f32 x, f32 z) {
                if (x < lake.minX || x > lake.maxX || z < lake.minZ ||
                    z > lake.maxZ || lake.mask.empty()) {
                    return false;
                }
                const u32 mx = static_cast<u32>(glm::clamp(
                    (x - lake.minX) / lake.maskTexel + 0.5f, 0.0f,
                    static_cast<f32>(lake.maskWidth - 1)));
                const u32 mz = static_cast<u32>(glm::clamp(
                    (z - lake.minZ) / lake.maskTexel + 0.5f, 0.0f,
                    static_cast<f32>(lake.maskHeight - 1)));
                return lake.mask[static_cast<size_t>(mz) *
                                     lake.maskWidth +
                                 mx] != 0;
            };
            const f32 tileEndX = tileMinX + params.tileSize;
            const f32 tileEndZ = tileMinZ + params.tileSize;
            vector<Lake> resolved;
            vector<u8> owned;
            vector<Vec2> anchors;
            for (size_t l = 0; l < hydro.lakes.size(); ++l) {
                Lake& lake = hydro.lakes[l];
                if (!touchesRim(lake)) {
                    resolved.push_back(std::move(lake));
                    owned.push_back(0);
                    continue;
                }
                const Vec2 probe = deepestOf(window, composite, lake);
                const Lake* wideLake = nullptr;
                for (const Lake& candidate : wideLakes) {
                    if (lakeCovers(candidate, probe.x, probe.y)) {
                        wideLake = &candidate;
                        break;
                    }
                }
                if (!wideLake) {
                    continue; // too shallow once widened: gone
                }
                const Vec2 anchor =
                    deepestOf(wide, wideComposite, *wideLake);
                if (anchor.x < tileMinX || anchor.x >= tileEndX ||
                    anchor.y < tileMinZ || anchor.y >= tileEndZ) {
                    continue; // the anchor tile publishes it
                }
                bool duplicate = false;
                for (const Vec2& seen : anchors) {
                    if (std::abs(seen.x - anchor.x) < 1.0f &&
                        std::abs(seen.y - anchor.y) < 1.0f) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue; // one basin touching the rim twice
                }
                anchors.push_back(anchor);
                resolved.push_back(*wideLake);
                owned.push_back(1);
            }
            hydro.lakes = std::move(resolved);
            preOwned = std::move(owned);
        }
    }

    // --- Finalize the CENTER tile against the composed hydrology.
    MacroResult macroFields;
    macroFields.spec = sim;
    macroFields.seaDist = self.seaDist;
    macroFields.biome = self.biome;
    // Biome character for the fine pass (cheap rebuild; stage 1 keeps
    // its own for the coarse erosion). Passability corridors damp the
    // fine ravines too — a pass stays walkable at every scale.
    BiomeCharacter character =
        biomeCharacter(sim, self.biome, params.biomeErosion);
    // The calm family (corridors + socles + valley floors) damps the
    // fine ravines to near zero: the habitable ground stays readable;
    // dissection concentrates on the slopes between socles.
    const vector<f32>& fineDamp =
        self.calm.empty() ? self.gentle : self.calm;
    if (!fineDamp.empty()) {
        if (character.fineScale.empty()) {
            character.fineScale.assign(sim.cells(), 1.0f);
        }
        const u32 simN = sim.n;
        for (size_t i = 0; i < fineDamp.size(); ++i) {
            // Gated damp: the mid-calm halo stops blanketing the
            // slopes (legacy = raw calm).
            const f32 damp =
                params.fineCalmGateHigh > 0.0f
                    ? glm::smoothstep(params.fineCalmGateLow,
                                      params.fineCalmGateHigh,
                                      fineDamp[i])
                    : fineDamp[i];
            character.fineScale[i] *= 1.0f - 0.95f * damp;
            // Measured reintroduction: steep, non-calm ground gets its
            // carve character back.
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
    // Lowland soil creep: the sharp fine gullies belong to orogeny
    // country (and arid badlands via the biome table) — grassland
    // plains and hills keep only half the carve.
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
    // Fine window = kept rect + fine-erosion halo; the rest of the
    // apron only ever existed at coarse resolution and never shipped.
    finalize.fineMinX = keepMinX - kFineErosionHalo;
    finalize.fineMinZ = keepMinZ - kFineErosionHalo;
    finalize.fineSpan = keepSpan + 2.0f * kFineErosionHalo;
    if (cancelled()) {
        return {}; // shutdown: nothing published
    }
    const FinalizeResult fine = finalizeTerrain(
        sim, self.eroded, macroFields, hydro, window, finalize,
        params.worldSeed,
        character.fineScale.empty() ? nullptr : &character.fineScale,
        self.deposit.empty() ? nullptr : &self.deposit, cancel);

    // Crop to tile + overlap margin. The margin ring is shared with the
    // neighbour bakes; height() blends the overlap by edge weight.
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
    // Masks: crop the coarse channels over the same rect.
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
    // Fine-scale runtime detail on top of the 2 m grid; the baked
    // detailAmp channel gates it off near water and beaches.
    region.detailAmplitude = kRegionDetailAmplitude;
    region.detailWavelength = kRegionDetailWavelength;
    region.detailOctaves = kRegionDetailOctaves;

    // --- Steady-water solve (option D, docs/WATER-RESEARCH.md): the
    // equilibrium depth/velocity/flux fields over the published rect,
    // computed on the FINAL cropped heights (the ground the runtime
    // renders — so the water sits exactly on it, and a terrain-patch
    // re-bake re-solves its water for free). Master courses entering
    // the rect inject their upstream discharge; the solver is pure and
    // single-threaded, so the fields are as deterministic as the
    // heights they derive from.
    if (params.solveWater) {
        // Solve on the WHOLE fine window (published rect + the fine-
        // erosion halo) and crop after: the open-border drain then sits
        // ~192 m outside anything that ships, and two neighbours solve
        // the shared margin band with real terrain context on both
        // sides — their levels agree closely (not bit-exactly: the
        // windows differ) where the geometry meets.
        GridSpec wspec;
        wspec.originX = fine.fineSpec.originX;
        wspec.originZ = fine.fineSpec.originZ;
        wspec.texelSize = params.waterSolveTexel;
        const u32 stride = glm::max(
            texels(wspec.texelSize, fine.fineSpec.texelSize), 1u);
        wspec.n = (fine.fineSpec.n - 1) / stride + 1;
        vector<f32> wground(wspec.cells());
        f32 maxGround = -1.0e9f;
        // Block MEAN of the fine samples around each node, not the
        // strided point: point sampling aliased narrow carved beds
        // into whole-cell pits (phantom pools whose level sat under
        // the real 2 m ground — invisible near, ghost sheets far) and
        // cut cliff corners. The mean is the cell's honest floor.
        const i32 half = static_cast<i32>(stride) / 2;
        const i32 fineNn = static_cast<i32>(fine.fineSpec.n);
        for (u32 row = 0; row < wspec.n; ++row) {
            for (u32 col = 0; col < wspec.n; ++col) {
                f32 sum = 0.0f;
                i32 taps = 0;
                for (i32 dz = -half; dz <= half; ++dz) {
                    for (i32 dx = -half; dx <= half; ++dx) {
                        const i32 fr = glm::clamp(
                            static_cast<i32>(row * stride) + dz, 0,
                            fineNn - 1);
                        const i32 fc = glm::clamp(
                            static_cast<i32>(col * stride) + dx, 0,
                            fineNn - 1);
                        sum += fine.height[static_cast<size_t>(fr) *
                                               fine.fineSpec.n +
                                           fc];
                        ++taps;
                    }
                }
                const f32 h = sum / static_cast<f32>(taps);
                wground[static_cast<size_t>(row) * wspec.n + col] = h;
                maxGround = glm::max(maxGround, h);
            }
        }
        WaterSolveParams ws = params.waterSolve;
        ws.seaLevel = params.macro.seaLevel;
        // A fully-submerged rect is the ocean sheet's job — skip.
        if (maxGround > ws.seaLevel + 0.5f) {
            ProceduralControlParams cp = params.controls;
            cp.seed = params.worldSeed;
            MasterNetworkParams network = params.network;
            network.seaLevel = params.macro.seaLevel;
            const f32 wMaxX =
                wspec.originX +
                static_cast<f32>(wspec.n - 1) * wspec.texelSize;
            const f32 wMaxZ =
                wspec.originZ +
                static_cast<f32>(wspec.n - 1) * wspec.texelSize;
            const vector<WaterSource> sources = masterBoundarySources(
                ProceduralControls { cp }, params.macro, network, ws,
                wspec.originX, wspec.originZ, wMaxX, wMaxZ);
            const WaterSolveResult water =
                solveSteadyWater(wspec, wground, ws, &sources);
            // Crop the halo away: ship the published rect only.
            const u32 cropOff =
                texels(keepMinX - wspec.originX, wspec.texelSize);
            const u32 waterN = glm::min(
                texels(keepSpan, wspec.texelSize) + 1,
                wspec.n - glm::min(cropOff, wspec.n - 1));
            const size_t wcells = static_cast<size_t>(waterN) * waterN;
            region.waterWidth = waterN;
            region.waterHeight = waterN;
            region.waterTexel = wspec.texelSize;
            region.waterSurface.assign(wcells, 0.0f);
            region.waterDepth.assign(wcells, 0);
            region.waterVelX.assign(wcells, 0);
            region.waterVelZ.assign(wcells, 0);
            region.waterFlux.assign(wcells, 0);
            for (u32 row = 0; row < waterN; ++row) {
                for (u32 col = 0; col < waterN; ++col) {
                    const size_t i =
                        static_cast<size_t>(row) * waterN + col;
                    const size_t j =
                        static_cast<size_t>(row + cropOff) * wspec.n +
                        (col + cropOff);
                    // Flux survives the dry pass on purpose: it is the
                    // course-trace signal (metadata, map), not a wet
                    // mask.
                    region.waterFlux[i] = static_cast<u8>(glm::clamp(
                        std::log10(1.0f +
                                   glm::max(water.flux[j], 0.0f)) *
                            (255.0f / 4.0f),
                        0.0f, 255.0f));
                    f32 depth = water.depth[j];
                    // Course continuity re-wet (see courseFluxThreshold).
                    if (water.flux[j] > params.courseFluxThreshold) {
                        depth = glm::max(depth, params.courseMinDepth);
                    }
                    if (depth <= 0.0f) {
                        continue;
                    }
                    const f32 surface = wground[j] + depth;
                    // Sea-pinned cells store dry: the ocean sheet
                    // renders them. Estuary water ABOVE sea level keeps
                    // its level.
                    if (wground[j] < ws.seaLevel &&
                        surface <= ws.seaLevel + 0.01f) {
                        continue;
                    }
                    region.waterDepth[i] = static_cast<u16>(glm::clamp(
                        depth * 32.0f + 0.5f, 1.0f, 65535.0f));
                    region.waterSurface[i] = surface;
                    region.waterVelX[i] = static_cast<i8>(glm::clamp(
                        water.velocityX[j] * 10.0f, -127.0f, 127.0f));
                    region.waterVelZ[i] = static_cast<i8>(glm::clamp(
                        water.velocityZ[j] * 10.0f, -127.0f, 127.0f));
                }
            }
        }
    }

    // Water ownership: lakes belong to the tile holding their bbox
    // center; river polylines are CLIPPED to this tile's kept rect (each
    // run is its own ribbon, the neighbour renders the continuation over
    // its own terrain — from the SAME composite, so they line up).
    const f32 tileMaxX = tileMinX + params.tileSize;
    const f32 tileMaxZ = tileMinZ + params.tileSize;
    for (size_t l = 0; l < hydro.lakes.size(); ++l) {
        const Lake& lake = hydro.lakes[l];
        // Canonically-resolved basins already passed the deepest-cell
        // ownership rule; only window-contained lakes use the center.
        if (l >= preOwned.size() || !preOwned[l]) {
            const f32 cx = (lake.minX + lake.maxX) * 0.5f;
            const f32 cz = (lake.minZ + lake.maxZ) * 0.5f;
            if (cx < tileMinX || cx >= tileMaxX || cz < tileMinZ ||
                cz >= tileMaxZ) {
                continue;
            }
        }
        out.lakes.push_back(lake);
    }
    // The flood predates the finalize carves — re-validate every
    // published lake against the FINAL ground (see the header note).
    reconcileLakesWithTerrain(out.lakes, out.region);
    // ALL window lakes reconciled too (not just the owned ones): the
    // river source check below must see a NEIGHBOUR-owned lake in its
    // post-reconcile state. Deterministic per tile; the region rect
    // walls keep it conservative.
    vector<Lake> allLakes = hydro.lakes;
    reconcileLakesWithTerrain(allLakes, out.region);
    const f32 keepMaxX = keepMinX + keepSpan;
    const f32 keepMaxZ = keepMinZ + keepSpan;
    const auto inKeep = [&](const RiverPoint& pt) {
        return pt.x >= keepMinX && pt.x <= keepMaxX && pt.z >= keepMinZ &&
               pt.z <= keepMaxZ;
    };
    for (const River& river : hydro.rivers) {
        // A lake-fed course whose feeding lake RECEDED (reconcile)
        // has no source any more: the whole course is dropped — the
        // hydrology traced it when the lake still overflowed its old
        // spill, and keeping it painted a water slope down a col
        // nothing feeds (measured dev at the spawn col). The sim
        // still creates REAL outflow dynamically if the reconciled
        // lake overflows elsewhere.
        if (river.lakeFed != 0 && !river.points.empty() &&
            !lakeReachesPoint(allLakes, river.points.front().x,
                              river.points.front().z)) {
            continue;
        }
        // Each kept run inherits the course's tier and the fords its
        // points can reach (the ford grid is world-anchored, so both
        // sides of a border keep the same spots).
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
    // Rivers carry pre-finalize surfaces — recolle them to the final
    // ground and the reconciled lakes (see the header note).
    reconcileRiversWithTerrain(out.rivers, out.lakes, out.region,
                               params.finalize);
    return out;
}

TileBakeResult bakeTile(const TileBakeParams& params, i32 tx, i32 tz,
                        const std::atomic<bool>* cancel) {
    TileStage1 stage1s[3][3];
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                return {}; // shutdown: nothing published
            }
            stage1s[dz + 1][dx + 1] =
                bakeTileStage1(params, tx + dx, tz + dz, cancel);
        }
    }
    return bakeTileStage2(params, tx, tz,
                          [&](i32 qx, i32 qz) -> const TileStage1* {
                              const i32 dx = qx - tx;
                              const i32 dz = qz - tz;
                              if (dx < -1 || dx > 1 || dz < -1 ||
                                  dz > 1) {
                                  return nullptr;
                              }
                              return &stage1s[dz + 1][dx + 1];
                          },
                          cancel);
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
                               const render::TerrainRegion& region) {
    if (region.heights.empty() || region.width < 2 ||
        region.height < 2 || region.texelSize <= 0.0f) {
        return;
    }
    const auto groundAt = [&](f32 x, f32 z) {
        return finalGroundAt(region, x, z);
    };
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
        // New level = the candidate (a claimed cell's fill, floored
        // to 0.25 m, capped at the old level) retaining the LARGEST
        // WATER VOLUME. The max-fill cell kept a one-cell perched
        // pocket and dropped the lake; the deepest claimed cell was a
        // phantom-finger cell down the gorge and dragged the level to
        // the canyon floor (both bench-measured on the real lake 5).
        vector<f32> cands;
        for (i32 r = 0; r < mh; ++r) {
            for (i32 c = 0; c < mw; ++c) {
                if (lake.mask[static_cast<size_t>(r) * mw + c] == 0) {
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
                    if (lake.mask[static_cast<size_t>(r) * mw + c] ==
                        0) {
                        continue;
                    }
                    const size_t i = pIdx(c, r);
                    if (fill[i] >= cand - 0.01f && ground[i] < cand) {
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
        // Deepest ENCLOSED claimed cell = the re-cut's BFS root; a
        // basin breached below ~0.5 m of real depth is dropped.
        size_t root = 0;
        f32 rootGround = 1.0e9f;
        for (i32 r = 0; r < mh; ++r) {
            for (i32 c = 0; c < mw; ++c) {
                if (lake.mask[static_cast<size_t>(r) * mw + c] == 0) {
                    continue;
                }
                const size_t i = pIdx(c, r);
                if (fill[i] >= level - 0.01f && ground[i] < rootGround) {
                    rootGround = ground[i];
                    root = i;
                }
            }
        }
        if (bestVol <= 0.0 || rootGround > 1.0e8f ||
            level - rootGround < 0.5f) {
            lakes.erase(lakes.begin() + static_cast<i32>(li));
            continue;
        }
        // Re-cut: a cell belongs to the lake iff it is UNDER the
        // level AND enclosed at it (fill >= level — mere "connected
        // under the level" painted a flat sheet down the descending
        // gorge: baked water on the hillside, measured dev),
        // connected to the root, and inside the stored bbox.
        vector<u8> mask(static_cast<size_t>(mw) * mh, 0);
        vector<u32> stack;
        stack.push_back(static_cast<u32>(root));
        vector<u8> visited(cells, 0);
        visited[root] = 1;
        u32 kept = 0;
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            const i32 c = static_cast<i32>(i % static_cast<size_t>(w));
            const i32 r = static_cast<i32>(i / static_cast<size_t>(w));
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
                if (visited[j] == 0 && ground[j] < level - 0.02f &&
                    fill[j] >= level - 0.01f) {
                    visited[j] = 1;
                    stack.push_back(static_cast<u32>(j));
                }
            }
        }
        if (kept == 0) {
            lakes.erase(lakes.begin() + static_cast<i32>(li));
            continue;
        }
        lake.level = level;
        lake.mask = std::move(mask);
        lake.cells = kept;
        ++li;
    }
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
        f32 prev = 1.0e9f;
        for (RiverPoint& pt : river.points) {
            f32 s = pt.surface;
            const f32 ground = finalGroundAt(region, pt.x, pt.z);
            if (ground < 1.0e8f) {
                // The S5d bed formula: the surface may ride at most
                // one carved-bed depth over the final ground.
                const f32 bed = glm::clamp(
                    finalize.riverDepthCoef * 2.0f * pt.halfWidth,
                    finalize.riverDepthMin, tierCap);
                s = glm::min(s, ground + bed);
            }
            for (const Lake& lake : lakes) {
                if (inLake(lake, pt.x, pt.z)) {
                    // Crossing a reconciled lake: the water surface
                    // there IS the lake's (possibly lowered) level.
                    s = lake.level;
                    break;
                }
            }
            // The monotone-downhill contract survives every clamp.
            s = glm::min(s, prev);
            prev = s;
            pt.surface = s;
        }
    }
}

} // namespace render::terraingen
