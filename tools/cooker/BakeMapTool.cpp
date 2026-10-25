#include "BakeMapTool.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "game/AllForms.hpp"
#include "game/TerrainGenTuning.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainBakeStreamer.hpp"
#include "world/terrain/MapRecords.hpp"
#include "world/terrain/TerrainRegions.hpp"

namespace cooker {

namespace {

// Max |baseHeight(A) - baseHeight(B)| over the shared band of two
// adjacent slices (the map-proto metric — the acceptance number).
f32 bandDivergence(const render::TerrainRegion& ra,
                   const render::TerrainRegion& rb, bool axisZ,
                   f32 border, f32 alongMin, f32 alongMax, f32 margin) {
    f32 bandMax = 0.0f;
    for (f32 d = -margin + 2.0f; d <= margin - 2.0f; d += 2.0f) {
        for (f32 s = alongMin + 2.0f; s < alongMax; s += 4.0f) {
            const f32 x = axisZ ? s : border + d;
            const f32 z = axisZ ? border + d : s;
            const f32 ha = render::terrain::baseHeight(ra, x, z);
            const f32 hb = render::terrain::baseHeight(rb, x, z);
            bandMax = glm::max(bandMax, std::abs(ha - hb));
        }
    }
    return bandMax;
}

std::optional<render::TerrainRegion> readSlice(
    const std::filesystem::path& mapDir, i32 tx, i32 tz) {
    char stem[64];
    std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u.trg", tx, tz,
                  render::terraingen::kTileBakeVersion);
    return world::readTrgFile(mapDir / stem);
}

// The seam instrument of two ADJACENT maps (`--pair`): the two bakes
// never share a window, so the shared overlap band (±overlapMargin) is
// where their divergence is measured — heights as RMS + max, water as
// orphan river points (a course on one side with no counterpart within
// `kPairWaterMatch` on the other) and the surface delta of the matched
// ones.
constexpr f32 kPairWaterMatch = 50.0f;

struct PairWater {
    u32 pointsA { 0 };
    u32 pointsB { 0 };
    u32 orphans { 0 };
    f32 orphanWidestHalf { 0.0f };
    f32 surfaceDeltaMax { 0.0f };
};

void collectLinePoints(const std::filesystem::path& mapDir, i32 tx,
                       i32 tz, bool vertical, f32 lineAt, f32 band,
                       vector<render::terraingen::RiverPoint>& out) {
    char stem[64];
    std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u", tx, tz,
                  render::terraingen::kTileBakeVersion);
    vector<render::terraingen::Lake> lakes;
    vector<render::terraingen::River> rivers;
    if (!game::readWaterFile(mapDir / (str { stem } + ".twb"), lakes,
                             rivers)) {
        return;
    }
    for (const auto& river : rivers) {
        for (const auto& pt : river.points) {
            const f32 across = vertical ? pt.x : pt.z;
            if (std::abs(across - lineAt) <= band) {
                out.push_back(pt);
            }
        }
    }
}

PairWater pairWater(const vector<render::terraingen::RiverPoint>& a,
                    const vector<render::terraingen::RiverPoint>& b) {
    PairWater out;
    out.pointsA = static_cast<u32>(a.size());
    out.pointsB = static_cast<u32>(b.size());
    const auto match = [&](const auto& from, const auto& to) {
        for (const auto& p : from) {
            f32 best = 1.0e30f;
            f32 surface = 0.0f;
            for (const auto& q : to) {
                const f32 d = std::hypot(p.x - q.x, p.z - q.z);
                if (d < best) {
                    best = d;
                    surface = q.surface;
                }
            }
            if (best > kPairWaterMatch) {
                ++out.orphans;
                out.orphanWidestHalf =
                    glm::max(out.orphanWidestHalf, p.halfWidth);
            } else {
                out.surfaceDeltaMax = glm::max(
                    out.surfaceDeltaMax, std::abs(p.surface - surface));
            }
        }
    };
    match(a, b);
    match(b, a);
    return out;
}

// The command line: <gameDir> <mapX> <mapZ> [tilesPerSide] then the
// optional flags — "--" disables the border transitions (calibration
// bakes), "--export-plugin <name>" also emits the map as an ordinary
// §5 plugin, "--pair <x> <z>" measures the seam with an adjacent map.
struct BakeMapArgs {
    std::filesystem::path gameDir;
    i32 mapX { 0 };
    i32 mapZ { 0 };
    i32 tilesPerSide { game::kMapTilesPerSide };
    bool borders { true };
    const char* exportName { nullptr };
    bool pair { false };
    i32 pairX { 0 };
    i32 pairZ { 0 };
};

// What the steps share once the game's params are resolved: the slice
// lattice of the map and where its cache lives.
struct BakeMapContext {
    BakeMapArgs args;
    data::FormTypeRegistry formTypes;
    render::terraingen::TileBakeParams params;
    std::filesystem::path cacheDir;
    std::filesystem::path mapDir;
    i32 tx0 { 0 };
    i32 tz0 { 0 };
};

bool parseArgs(char** argv, int argc, BakeMapArgs& args) {
    args.gameDir = argv[2];
    args.mapX = std::atoi(argv[3]);
    args.mapZ = std::atoi(argv[4]);
    const bool tilesGiven = argc >= 6 && argv[5][0] != '-';
    args.tilesPerSide =
        tilesGiven ? std::atoi(argv[5]) : game::kMapTilesPerSide;
    if (args.tilesPerSide < 2 || args.tilesPerSide > 8) {
        LOG_ERROR("bake-map: tilesPerSide must be 2-8");
        return false;
    }
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            args.borders = false;
        } else if (std::strcmp(argv[i], "--export-plugin") == 0 &&
                   i + 1 < argc) {
            args.exportName = argv[++i];
        } else if (std::strcmp(argv[i], "--pair") == 0 && i + 2 < argc) {
            args.pair = true;
            args.pairX = std::atoi(argv[++i]);
            args.pairZ = std::atoi(argv[++i]);
        }
    }
    if (args.pair &&
        std::abs(args.pairX - args.mapX) + std::abs(args.pairZ - args.mapZ) !=
            1) {
        LOG_ERROR("bake-map: --pair needs an ADJACENT map");
        return false;
    }
    return true;
}

// Game bake params (the pre-bake resolution path: the SAME plugin-
// resolved tuning the game bakes with).
bool loadGameParams(BakeMapContext& ctx) {
    const BakeMapArgs& args = ctx.args;
    const auto dataDir = args.gameDir / "data";
    data::FormTypeRegistry& formTypes = ctx.formTypes;
    game::registerAllFormTypes(formTypes);
    data::PluginConfig pluginConfig;
    if (const auto loaded =
            data::loadPluginConfigFile(dataDir / "plugins.toml")) {
        pluginConfig = *loaded;
    } else {
        pluginConfig = data::defaultConfigFromDirectory(dataDir / "base");
        for (auto& entry : pluginConfig.entries) {
            entry.file = "base/" + entry.file;
        }
    }
    data::PluginStack stack =
        data::loadPluginStack(dataDir, pluginConfig, formTypes);
    if (stack.plugins.empty()) {
        LOG_ERROR("bake-map: no plugins under {} — wrong gameDir?",
                  dataDir.string());
        return false;
    }
    data::FormDatabase forms;
    data::resolve(data::pointersOf(stack), formTypes, forms);
    const data::LandscapeTuningForm tuning =
        data::resolveLandscapeTuning(forms);

    render::terraingen::TileBakeParams& params = ctx.params;
    params = game::makeTerrainBakeParams(
        tuning, data::resolveTerrainGenTuning(forms), &forms);
    params.mapGrid.valid = args.borders; // MapBaker fills the spec

    ctx.cacheDir = args.gameDir / "terrain-cache" /
                   std::to_string(tuning.terrainSeed);
    ctx.mapDir = game::mapCacheDir(ctx.cacheDir, args.mapX, args.mapZ);
    ctx.tx0 = args.mapX * args.tilesPerSide;
    ctx.tz0 = args.mapZ * args.tilesPerSide;
    render::terraingen::setMasterNetworkCacheDir(ctx.cacheDir);
    LOG_INFO("bake-map: seed {} | map ({}, {}) = {}x{} slices "
             "({:.0f} m)",
             params.worldSeed, args.mapX, args.mapZ, args.tilesPerSide,
             args.tilesPerSide,
             params.tileSize * static_cast<f32>(args.tilesPerSide));
    return true;
}

bool bakeMainMap(const BakeMapContext& ctx, core::JobSystem& jobs) {
    const BakeMapArgs& args = ctx.args;
    const game::MapBakeStats stats = game::bakeMap(
        ctx.params, args.mapX, args.mapZ, ctx.cacheDir, &jobs,
        args.tilesPerSide, [](u32 landed, u32 total) {
            LOG_INFO("bake-map: [{}/{}] slice landed", landed, total);
        });
    if (stats.slicesWritten != static_cast<u32>(args.tilesPerSide) *
                                   static_cast<u32>(args.tilesPerSide)) {
        LOG_ERROR("bake-map: incomplete bake");
        return false;
    }
    return true;
}

// Interior-border acceptance sweep, read back from the WRITTEN slices
// (two resident at a time — memory-bounded). `worst` is the band max
// over every interior line.
bool reportInteriorBorders(const BakeMapContext& ctx, f32& worst) {
    const i32 tilesPerSide = ctx.args.tilesPerSide;
    const auto& mapDir = ctx.mapDir;
    const f32 tileSize = ctx.params.tileSize;
    const f32 margin = ctx.params.overlapMargin;
    const i32 tx0 = ctx.tx0;
    const i32 tz0 = ctx.tz0;
    worst = 0.0f;
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const i32 tx = tx0 + dx;
            const i32 tz = tz0 + dz;
            const auto a = readSlice(mapDir, tx, tz);
            if (!a) {
                LOG_ERROR("bake-map: cannot re-read slice ({}, {})",
                          tx, tz);
                return false;
            }
            if (dx + 1 < tilesPerSide) {
                if (const auto b = readSlice(mapDir, tx + 1, tz)) {
                    const f32 alongMin =
                        static_cast<f32>(tz) * tileSize;
                    const f32 band = bandDivergence(
                        *a, *b, false,
                        static_cast<f32>(tx + 1) * tileSize, alongMin,
                        alongMin + tileSize, margin);
                    if (band > 2.0f) {
                        LOG_INFO("bake-map: border x={:.0f} z=[{:.0f}.."
                                 "{:.0f}] — band max {:.3f} m",
                                 static_cast<f32>(tx + 1) * tileSize,
                                 alongMin, alongMin + tileSize, band);
                    }
                    worst = glm::max(worst, band);
                }
            }
            if (dz + 1 < tilesPerSide) {
                if (const auto b = readSlice(mapDir, tx, tz + 1)) {
                    const f32 alongMin =
                        static_cast<f32>(tx) * tileSize;
                    const f32 band = bandDivergence(
                        *a, *b, true,
                        static_cast<f32>(tz + 1) * tileSize, alongMin,
                        alongMin + tileSize, margin);
                    if (band > 2.0f) {
                        LOG_INFO("bake-map: border z={:.0f} x=[{:.0f}.."
                                 "{:.0f}] — band max {:.3f} m",
                                 static_cast<f32>(tz + 1) * tileSize,
                                 alongMin, alongMin + tileSize, band);
                    }
                    worst = glm::max(worst, band);
                }
            }
        }
    }
    // With the shared surface AND the shared map hydrology, the only
    // per-slice pass left is the fine erosion (support-bounded, max
    // depth 3.5 m) plus the ±1.1 m relief damp — the gate is their
    // stack (docs/PAYSAGE.md §1.1; windowed tiles measured 200-441 m).
    LOG_INFO("bake-map: worst interior band divergence {:.3f} m "
             "(acceptance <= 5 m — fine-erosion residual)",
             worst);
    return true;
}

// Mirror calibration report: mean/max (baked - shaped analytic) per
// 100 m analytic band — the data that refits the macroHeightAnalytic
// compression (its constants were fitted to the WINDOWED bake; a
// drifted mirror pops the ground when slices land and draws a long
// straight step at the streaming frontier).
void reportMirrorCalibration(const BakeMapContext& ctx) {
    const i32 tilesPerSide = ctx.args.tilesPerSide;
    const auto& mapDir = ctx.mapDir;
    const render::terraingen::TileBakeParams& params = ctx.params;
    const f32 tileSize = params.tileSize;
    const i32 tx0 = ctx.tx0;
    const i32 tz0 = ctx.tz0;
    const render::terraingen::ProceduralControls controls {
        [&] {
            render::terraingen::ProceduralControlParams cp =
                params.controls;
            cp.seed = params.worldSeed;
            return cp;
        }()
    };
    render::terraingen::MapGridSpec gridSpec = params.mapGrid;
    if (gridSpec.valid) {
        gridSpec.seed = params.worldSeed;
        gridSpec.mapSize =
            tileSize * static_cast<f32>(tilesPerSide);
        gridSpec.seaLevel = params.macro.seaLevel;
    }
    const auto overview = game::loadMapOverview(mapDir);
    const auto fallbackAt = [&](f32 x, f32 z, f32 analytic) {
        if (!overview) {
            return analytic;
        }
        const auto& g = overview->grid;
        const f32 u = (x - g.originX) / g.texelSize;
        const f32 v = (z - g.originZ) / g.texelSize;
        if (u < 0.0f || v < 0.0f ||
            u > static_cast<f32>(g.n - 1) ||
            v > static_cast<f32>(g.n - 1)) {
            return analytic;
        }
        const u32 c0 = glm::min(static_cast<u32>(u), g.n - 2);
        const u32 r0 = glm::min(static_cast<u32>(v), g.n - 2);
        const f32 tu = u - static_cast<f32>(c0);
        const f32 tv = v - static_cast<f32>(r0);
        const auto at = [&](u32 c, u32 r) {
            return overview
                ->heights[static_cast<size_t>(r) * g.n + c];
        };
        return glm::mix(
            glm::mix(at(c0, r0), at(c0 + 1, r0), tu),
            glm::mix(at(c0, r0 + 1), at(c0 + 1, r0 + 1), tu), tv);
    };
    LOG_INFO("bake-map: mirror report vs {}",
             overview ? "the 64 m overview" : "the shaped analytic");
    constexpr u32 kBands = 12;
    f64 sum[kBands] = {};
    f32 worstBand[kBands] = {};
    u32 n[kBands] = {};
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const auto slice =
                readSlice(mapDir, tx0 + dx, tz0 + dz);
            if (!slice) {
                continue;
            }
            for (f32 z = slice->originZ + 128.0f;
                 z < slice->originZ + slice->spanZ() - 128.0f;
                 z += 96.0f) {
                for (f32 x = slice->originX + 128.0f;
                     x < slice->originX + slice->spanX() - 128.0f;
                     x += 96.0f) {
                    const f32 fallback = fallbackAt(
                        x, z,
                        render::terraingen::applyMapGridShape(
                            controls, params.macro, gridSpec, x, z,
                            render::terraingen::
                                macroHeightAnalytic(
                                    controls, params.macro, x,
                                    z)));
                    const f32 rel =
                        fallback - params.macro.seaLevel;
                    if (rel < 0.0f) {
                        continue; // the sea agrees by decree
                    }
                    const u32 band = glm::min(
                        kBands - 1,
                        static_cast<u32>(rel / 100.0f));
                    const f32 delta =
                        render::terrain::baseHeight(*slice, x,
                                                    z) -
                        fallback;
                    sum[band] += delta;
                    worstBand[band] = glm::max(
                        worstBand[band], std::abs(delta));
                    ++n[band];
                }
            }
        }
    }
    for (u32 b = 0; b < kBands; ++b) {
        if (n[b] == 0) {
            continue;
        }
        LOG_INFO("bake-map: mirror band {:>4}-{:<4} m — mean "
                 "delta {:+7.1f} m, max |delta| {:6.1f} m ({} "
                 "samples)",
                 b * 100, (b + 1) * 100,
                 sum[b] / static_cast<f64>(n[b]), worstBand[b],
                 n[b]);
    }
}

// --pair: the SEAM instrument of two adjacent maps. Two independent
// bakes meet on their shared overlap band: heights compared as RMS +
// max there, river points matched across the line (orphans = a course
// one bake has and the other does not).
bool reportPairSeam(const BakeMapContext& ctx, core::JobSystem& jobs) {
    const BakeMapArgs& args = ctx.args;
    const i32 mapX = args.mapX;
    const i32 mapZ = args.mapZ;
    const i32 pairX = args.pairX;
    const i32 pairZ = args.pairZ;
    const i32 tilesPerSide = args.tilesPerSide;
    const auto& mapDir = ctx.mapDir;
    const auto& cacheDir = ctx.cacheDir;
    const render::terraingen::TileBakeParams& params = ctx.params;
    const f32 tileSize = params.tileSize;
    const f32 margin = params.overlapMargin;
    const i32 tx0 = ctx.tx0;
    const i32 tz0 = ctx.tz0;
    if (!game::mapBakedAndValid(cacheDir, pairX, pairZ, tilesPerSide,
                                &params)) {
        const game::MapBakeStats statsB = game::bakeMap(
            params, pairX, pairZ, cacheDir, &jobs, tilesPerSide);
        if (statsB.slicesWritten !=
            static_cast<u32>(tilesPerSide) *
                static_cast<u32>(tilesPerSide)) {
            LOG_ERROR("bake-map: incomplete pair bake");
            return false;
        }
    }
    const auto mapDirB = game::mapCacheDir(cacheDir, pairX, pairZ);
    const bool vertical = pairX != mapX;
    const f32 mapSize = tileSize * static_cast<f32>(tilesPerSide);
    const i32 lineIndex =
        vertical ? glm::max(mapX, pairX) : glm::max(mapZ, pairZ);
    const f32 lineAt = static_cast<f32>(lineIndex) * mapSize;
    f64 sum2 = 0.0;
    u64 nSamples = 0;
    f32 worstPair = 0.0f;
    vector<render::terraingen::RiverPoint> ptsA;
    vector<render::terraingen::RiverPoint> ptsB;
    for (i32 k = 0; k < tilesPerSide; ++k) {
        i32 taX, taZ, tbX, tbZ;
        if (vertical) {
            taX = (pairX > mapX ? tx0 + tilesPerSide - 1 : tx0);
            tbX = (pairX > mapX ? pairX * tilesPerSide
                                : pairX * tilesPerSide +
                                      tilesPerSide - 1);
            taZ = tz0 + k;
            tbZ = taZ;
        } else {
            taZ = (pairZ > mapZ ? tz0 + tilesPerSide - 1 : tz0);
            tbZ = (pairZ > mapZ ? pairZ * tilesPerSide
                                : pairZ * tilesPerSide +
                                      tilesPerSide - 1);
            taX = tx0 + k;
            tbX = taX;
        }
        const auto a = readSlice(mapDir, taX, taZ);
        const auto b = readSlice(mapDirB, tbX, tbZ);
        if (!a || !b) {
            LOG_ERROR("bake-map: pair cannot re-read facing slices "
                      "({}, {}) / ({}, {})",
                      taX, taZ, tbX, tbZ);
            return false;
        }
        const f32 alongMin =
            (vertical ? static_cast<f32>(taZ)
                      : static_cast<f32>(taX)) *
            tileSize;
        for (f32 d = -margin + 2.0f; d <= margin - 2.0f; d += 4.0f) {
            for (f32 s = alongMin + 2.0f; s < alongMin + tileSize;
                 s += 4.0f) {
                const f32 x = vertical ? lineAt + d : s;
                const f32 z = vertical ? s : lineAt + d;
                const f32 delta =
                    render::terrain::baseHeight(*a, x, z) -
                    render::terrain::baseHeight(*b, x, z);
                sum2 += static_cast<f64>(delta) *
                        static_cast<f64>(delta);
                ++nSamples;
                worstPair = glm::max(worstPair, std::abs(delta));
            }
        }
        collectLinePoints(mapDir, taX, taZ, vertical, lineAt, margin,
                          ptsA);
        collectLinePoints(mapDirB, tbX, tbZ, vertical, lineAt,
                          margin, ptsB);
    }
    const PairWater water = pairWater(ptsA, ptsB);
    const render::terraingen::ProceduralControls styleControls { [&] {
        render::terraingen::ProceduralControlParams cp =
            params.controls;
        cp.seed = params.worldSeed;
        return cp;
    }() };
    render::terraingen::MapGridSpec styleSpec = params.mapGrid;
    styleSpec.seed = params.worldSeed;
    styleSpec.mapSize = mapSize;
    styleSpec.seaLevel = params.macro.seaLevel;
    const auto style = render::terraingen::mapBorderStyleResolved(
        styleControls, params.macro, styleSpec, lineIndex,
        vertical ? mapZ : mapX, vertical);
    const char* styleName =
        style == render::terraingen::MapEdgeStyle::Sea ? "Sea"
        : style == render::terraingen::MapEdgeStyle::Ridges
            ? "Ridges"
            : "Open";
    const f32 rms =
        nSamples > 0
            ? static_cast<f32>(std::sqrt(
                  sum2 / static_cast<f64>(nSamples)))
            : 0.0f;
    LOG_INFO("bake-map: pair ({}, {})/({}, {}) line {} [{}] — "
             "heights RMS {:.2f} m, max {:.2f} m ({} samples) | "
             "water: {}+{} line points, {} orphans (widest half "
             "{:.1f} m), matched surface delta max {:.2f} m",
             mapX, mapZ, pairX, pairZ,
             vertical ? "vertical" : "horizontal", styleName, rms,
             worstPair, nSamples, water.pointsA, water.pointsB,
             water.orphans, water.orphanWidestHalf,
             water.surfaceDeltaMax);
    return true;
}

// --export-plugin: the baked map as an ORDINARY §5 mod — slices copied
// under data/mods/terrain/, records staged through stageMapRecords, one
// TOML out. Deterministic guids (map guid = f(seed, coords)) mean a
// re-export PATCHES the same world.
bool exportMapPlugin(BakeMapContext& ctx) {
    const BakeMapArgs& args = ctx.args;
    const i32 mapX = args.mapX;
    const i32 mapZ = args.mapZ;
    const i32 tilesPerSide = args.tilesPerSide;
    const char* exportName = args.exportName;
    const auto& mapDir = ctx.mapDir;
    const render::terraingen::TileBakeParams& params = ctx.params;
    data::FormTypeRegistry& formTypes = ctx.formTypes;
    const i32 tx0 = ctx.tx0;
    const i32 tz0 = ctx.tz0;
    const core::Guid mapGuid = world::mapWorldspaceGuid(
        params.worldSeed, mapX, mapZ);
    const auto modsDir = args.gameDir / "data" / "mods";
    char rel[64];
    std::snprintf(rel, sizeof(rel), "terrain/map_%d_%d", mapX,
                  mapZ);
    std::error_code errc;
    std::filesystem::create_directories(modsDir / rel, errc);

    vector<world::MapSliceRecord> slices;
    vector<data::AssetEntry> assetEntries;
    vector<render::terraingen::Lake> lakes;
    vector<render::terraingen::River> rivers;
    for (i32 dz = 0; dz < tilesPerSide; ++dz) {
        for (i32 dx = 0; dx < tilesPerSide; ++dx) {
            const i32 tx = tx0 + dx;
            const i32 tz = tz0 + dz;
            char stem[64];
            std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u",
                          tx, tz,
                          render::terraingen::kTileBakeVersion);
            const auto trg = mapDir / (str { stem } + ".trg");
            const auto dest = modsDir / rel / (str { stem } + ".trg");
            std::filesystem::copy_file(
                trg, dest,
                std::filesystem::copy_options::overwrite_existing,
                errc);
            if (errc) {
                LOG_ERROR("bake-map: cannot copy {} to the mod",
                          trg.string());
                return false;
            }
            const u32 index = static_cast<u32>(
                dz * tilesPerSide + dx);
            const core::Guid asset =
                world::mapSliceAssetGuid(mapGuid, index);
            slices.push_back(
                { tx, tz, asset,
                  render::terraingen::kRegionDetailAmplitude,
                  render::terraingen::kRegionDetailWavelength,
                  render::terraingen::kRegionDetailOctaves });
            assetEntries.push_back(
                { asset,
                  str { rel } + "/" + stem + ".trg" });
            vector<render::terraingen::Lake> sliceLakes;
            vector<render::terraingen::River> sliceRivers;
            if (game::readWaterFile(mapDir / (str { stem } + ".twb"),
                                    sliceLakes, sliceRivers)) {
                for (auto& lake : sliceLakes) {
                    lakes.push_back(std::move(lake));
                }
                for (auto& river : sliceRivers) {
                    rivers.push_back(std::move(river));
                }
            }
        }
    }

    char mapName[64];
    std::snprintf(mapName, sizeof(mapName), "Map_%d_%d", mapX,
                  mapZ);
    data::FormDatabase stageDb;
    data::EditSession session { stageDb, formTypes };
    world::stageMapRecords(session, stageDb, mapGuid, mapName,
                           mapX, mapZ,
                           params.tileSize *
                               static_cast<f32>(tilesPerSide),
                           params.worldSeed, slices, lakes,
                           rivers, 48.0f);
    data::Plugin plugin = session.exportPlugin(
        core::Guid::combine(mapGuid,
                            core::Guid { 1, 0x6d6170706c756731ull }),
        exportName);
    plugin.assets = std::move(assetEntries);
    const auto pluginPath =
        modsDir / (str { exportName } + ".toml");
    std::ofstream file { pluginPath, std::ios::trunc };
    if (!file) {
        LOG_ERROR("bake-map: cannot write {}",
                  pluginPath.string());
        return false;
    }
    file << data::writePluginToml(plugin, formTypes);
    LOG_INFO("bake-map: exported plugin {} ({} records, {} "
             "assets) — add 'mods/{}.toml' to plugins.toml",
             pluginPath.string(), plugin.records.size(),
             plugin.assets.size(), exportName);
    return true;
}

} // namespace

int bakeMapCmd(char** argv, int argc) {
    BakeMapContext ctx;
    if (!parseArgs(argv, argc, ctx.args) || !loadGameParams(ctx)) {
        return 1;
    }
    core::JobSystem jobs;
    if (!bakeMainMap(ctx, jobs)) {
        return 1;
    }
    f32 worst = 0.0f;
    if (!reportInteriorBorders(ctx, worst)) {
        return 1;
    }
    reportMirrorCalibration(ctx);
    if (ctx.args.pair && !reportPairSeam(ctx, jobs)) {
        return 1;
    }
    if (ctx.args.exportName && !exportMapPlugin(ctx)) {
        return 1;
    }
    return worst <= 5.0f ? 0 : 1;
}

} // namespace cooker
