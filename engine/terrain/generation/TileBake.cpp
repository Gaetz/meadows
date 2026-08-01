#include "engine/terrain/generation/TileBake.hpp"

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

f32 bilinearAt(const GridSpec& spec, const vector<f32>& grid, f32 wx,
               f32 wz) {
    const f32 u = glm::clamp((wx - spec.originX) / spec.texelSize, 0.0f,
                             static_cast<f32>(spec.n - 1));
    const f32 v = glm::clamp((wz - spec.originZ) / spec.texelSize, 0.0f,
                             static_cast<f32>(spec.n - 1));
    const u32 u0 = glm::min(static_cast<u32>(u), spec.n - 2);
    const u32 v0 = glm::min(static_cast<u32>(v), spec.n - 2);
    const f32 tu = u - static_cast<f32>(u0);
    const f32 tv = v - static_cast<f32>(v0);
    const auto at = [&](u32 cx, u32 cz) {
        return grid[static_cast<size_t>(cz) * spec.n + cx];
    };
    const f32 a = at(u0, v0) + (at(u0 + 1, v0) - at(u0, v0)) * tu;
    const f32 b =
        at(u0, v0 + 1) + (at(u0 + 1, v0 + 1) - at(u0, v0 + 1)) * tu;
    return a + (b - a) * tv;
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
    // 3x3 box blur, fixed row-major order (deterministic): the ids are
    // nearest-sampled, the blur keeps erosion from stepping at borders.
    const i32 n = static_cast<i32>(spec.n);
    const auto blur = [&](const vector<f32>& src) {
        vector<f32> dst(cells);
        for (i32 z = 0; z < n; ++z) {
            for (i32 x = 0; x < n; ++x) {
                f32 sum = 0.0f;
                i32 count = 0;
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 cx = x + dx;
                        const i32 cz = z + dz;
                        if (cx < 0 || cx >= n || cz < 0 || cz >= n) {
                            continue;
                        }
                        sum += src[static_cast<size_t>(cz) * n + cx];
                        ++count;
                    }
                }
                dst[static_cast<size_t>(z) * n + x] =
                    sum / static_cast<f32>(count);
            }
        }
        return dst;
    };
    out.erodibility = blur(rawErod);
    out.talusScale = blur(rawTalus);
    out.capacityScale = blur(rawCapacity);
    out.fineScale = blur(rawFine);
    return out;
}

TileStage1 bakeTileStage1(const TileBakeParams& params, i32 tx, i32 tz) {
    TileStage1 out;
    out.sim = simSpecFor(params, tx, tz);

    ProceduralControlParams controlParams = params.controls;
    controlParams.seed = params.worldSeed;
    const ProceduralControls controls { controlParams };

    const MacroResult macro =
        synthesizeMacro(controls, out.sim, params.macro,
                        params.worldSeed,
                        controlParams.hillChainWavelength);
    const BiomeCharacter character =
        biomeCharacter(out.sim, macro.biome, params.biomeErosion);

    FluvialParams fluvial = params.fluvial;
    fluvial.seaLevel = params.macro.seaLevel;
    const FluvialResult eroded = erodeFluvial(
        out.sim, macro.height, macro.uplift, fluvial, nullptr,
        character.erodibility.empty() ? nullptr : &character.erodibility,
        character.capacityScale.empty() ? nullptr
                                        : &character.capacityScale);

    ThermalParams thermal = params.thermal;
    thermal.seaLevel = params.macro.seaLevel;
    ThermalResult relaxed = erodeThermal(
        out.sim, eroded.height, thermal,
        character.talusScale.empty() ? nullptr : &character.talusScale);

    out.eroded = std::move(relaxed.height);
    // One sediment field: thermal scree + fluvial alluvium.
    out.deposit = std::move(relaxed.deposit);
    if (!eroded.deposit.empty()) {
        for (size_t i = 0; i < out.deposit.size(); ++i) {
            out.deposit[i] += eroded.deposit[i];
        }
    }
    out.seaDist = std::move(macro.seaDist);
    out.biome = std::move(macro.biome);
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

    // --- COMPOSED hydrology window: tile + waterMargin, blended from
    // the 3x3 stage-1 sims with the SAME weights the runtime uses for
    // the published regions (tile+overlapMargin rects, edgeBlend =
    // 2*overlapMargin). Both neighbours of a border compose the same
    // surface here — their water agrees by construction.
    GridSpec window;
    window.texelSize = params.macroTexel;
    window.originX = tileMinX - params.waterMargin;
    window.originZ = tileMinZ - params.waterMargin;
    window.n = texels(params.tileSize + 2.0f * params.waterMargin,
                      params.macroTexel) +
               1;
    const TileStage1* neighbours[3][3];
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            neighbours[dz + 1][dx + 1] = stage1At(tx + dx, tz + dz);
        }
    }
    const f32 rectMargin = params.overlapMargin;
    const f32 blend = 2.0f * params.overlapMargin;
    vector<f32> composite(window.cells());
    for (u32 row = 0; row < window.n; ++row) {
        for (u32 col = 0; col < window.n; ++col) {
            const f32 wx = window.x(col);
            const f32 wz = window.z(row);
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
                    hSum += w * bilinearAt(s1->sim, s1->eroded, wx, wz);
                    wSum += w;
                }
            }
            composite[static_cast<size_t>(row) * window.n + col] =
                wSum > 0.0f ? hSum / wSum
                            : bilinearAt(sim, self.eroded, wx, wz);
        }
    }

    HydrologyParams hydrology = params.hydrology;
    hydrology.seaLevel = params.macro.seaLevel;
    const HydrologyResult hydro =
        extractHydrology(window, composite, hydrology);

    // --- Finalize the CENTER tile against the composed hydrology.
    MacroResult macroFields;
    macroFields.spec = sim;
    macroFields.seaDist = self.seaDist;
    macroFields.biome = self.biome;
    // Biome character for the fine pass (cheap rebuild; stage 1 keeps
    // its own for the coarse erosion).
    const BiomeCharacter character =
        biomeCharacter(sim, self.biome, params.biomeErosion);
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
    for (const Lake& lake : hydro.lakes) {
        const f32 cx = (lake.minX + lake.maxX) * 0.5f;
        const f32 cz = (lake.minZ + lake.maxZ) * 0.5f;
        if (cx < tileMinX || cx >= tileMaxX || cz < tileMinZ ||
            cz >= tileMaxZ) {
            continue;
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
