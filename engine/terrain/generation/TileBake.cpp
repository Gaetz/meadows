#include "engine/terrain/generation/TileBake.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>

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

TileStage1 bakeTileStage1(const TileBakeParams& params, i32 tx, i32 tz) {
    TileStage1 out;
    out.sim = simSpecFor(params, tx, tz);

    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    MacroParams macroParams = params.macro;
    macroParams.hillChainWavelength = controlParams.hillChainWavelength;
    const MacroResult macro =
        synthesizeMacro(controls, out.sim, macroParams, params.worldSeed);
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
    }
    const FluvialResult eroded = erodeFluvial(
        out.sim, macro.height, macro.uplift, fluvial,
        keep.empty() ? nullptr : &keep,
        character.erodibility.empty() ? nullptr : &character.erodibility,
        character.capacityScale.empty() ? nullptr
                                        : &character.capacityScale);

    ThermalParams thermal = params.thermal;
    thermal.seaLevel = params.macro.seaLevel;
    ThermalResult relaxed = erodeThermal(
        out.sim, eroded.height, thermal,
        character.talusScale.empty() ? nullptr : &character.talusScale);

    out.eroded = std::move(relaxed.height);
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
    const std::function<const TileStage1*(i32, i32)>& stage1At) {
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

    HydrologyParams hydrology = params.hydrology;
    hydrology.seaLevel = params.macro.seaLevel;
    HydrologyResult hydro = extractHydrology(window, composite, hydrology);

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
    const FinalizeResult fine = finalizeTerrain(
        sim, self.eroded, macroFields, hydro, window, finalize,
        params.worldSeed,
        character.fineScale.empty() ? nullptr : &character.fineScale,
        self.deposit.empty() ? nullptr : &self.deposit);

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
    const f32 keepMaxX = keepMinX + keepSpan;
    const f32 keepMaxZ = keepMinZ + keepSpan;
    const auto inKeep = [&](const RiverPoint& pt) {
        return pt.x >= keepMinX && pt.x <= keepMaxX && pt.z >= keepMinZ &&
               pt.z <= keepMaxZ;
    };
    for (const River& river : hydro.rivers) {
        River run;
        for (const RiverPoint& pt : river.points) {
            if (inKeep(pt)) {
                run.points.push_back(pt);
                continue;
            }
            if (run.points.size() >= 2) {
                out.rivers.push_back(std::move(run));
            }
            run.points.clear();
        }
        if (run.points.size() >= 2) {
            out.rivers.push_back(std::move(run));
        }
    }
    return out;
}

TileBakeResult bakeTile(const TileBakeParams& params, i32 tx, i32 tz) {
    TileStage1 stage1s[3][3];
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            stage1s[dz + 1][dx + 1] =
                bakeTileStage1(params, tx + dx, tz + dz);
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
                          });
}

} // namespace render::terraingen
