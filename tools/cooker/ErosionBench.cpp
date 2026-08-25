#include "ErosionBench.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#include <glm/glm.hpp>
#include <stb_image_write.h>

#include "engine/core/Log.hpp"
#include "engine/terrain/generation/GridOps.hpp"
#include "engine/terrain/generation/MapExport.hpp"
#include "engine/terrain/generation/TileBake.hpp"

// The dev's ruling: erosion tuning changed the landscape enormously in
// the past — variants stay a MEASURED REINTRODUCTION, never a boost.
// The bench bakes one reference tile under each variant, renders the
// shaded relief, assembles a contact sheet and prints the numbers.

namespace cooker {

namespace {

using namespace render::terraingen;

struct Variant {
    const char* name;
    f32 calmGateLow, calmGateHigh; // 0/0 = legacy damp
    f32 slopeReturn;
    f32 relaxGateLow, relaxGateHigh;
    f32 crestFade;
};

constexpr Variant kVariants[] = {
    { "v0_reference", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    { "v1_halo", 0.35f, 0.7f, 0.0f, 0.5f, 0.85f, 0.0f },
    { "v2_halo_pente", 0.35f, 0.7f, 0.35f, 0.5f, 0.85f, 0.0f },
    { "v3_halo_crete", 0.35f, 0.7f, 0.0f, 0.5f, 0.85f, 0.35f },
    { "v4_complet_leger", 0.35f, 0.7f, 0.35f, 0.5f, 0.85f, 0.35f },
    { "v5_complet_moyen", 0.35f, 0.7f, 0.6f, 0.5f, 0.85f, 0.5f },
};

} // namespace

int erosionBench(char** argv, int argc) {
    using namespace render::terraingen;
    const u32 seed = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    const i32 tx = std::atoi(argv[3]);
    const i32 tz = std::atoi(argv[4]);
    const std::string outDir = argv[5];
    const u32 size = argc >= 7 ? static_cast<u32>(std::atoi(argv[6]))
                               : 1000u;

    constexpr u32 kCols = 3;
    constexpr u32 kRows = 2;
    vector<u8> sheet(static_cast<size_t>(kCols) * size * kRows * size * 3,
                     255);
    vector<f32> reference;

    u32 index = 0;
    for (const Variant& variant : kVariants) {
        TileBakeParams params;
        params.worldSeed = seed;
        params.fineCalmGateLow = variant.calmGateLow;
        params.fineCalmGateHigh = variant.calmGateHigh;
        params.fineSlopeReturn = variant.slopeReturn;
        params.relaxGateLow = variant.relaxGateLow;
        params.relaxGateHigh = variant.relaxGateHigh;
        params.keepCrestFade = variant.crestFade;
        const TileBakeResult baked = bakeTile(params, tx, tz);
        const render::TerrainRegion& region = baked.region;

        // Numbers: delta vs reference, incision proxy (deviation from
        // a ~50 m mean), land-window family census at 250 m.
        if (reference.empty()) {
            reference = region.heights;
        }
        f64 deltaSum = 0.0;
        f64 deltaMax = 0.0;
        u64 deltaCount = 0;
        for (size_t i = 0; i < region.heights.size(); i += 7) {
            const f64 d = std::abs(
                static_cast<f64>(region.heights[i] - reference[i]));
            deltaSum += d;
            deltaMax = glm::max(deltaMax, d);
            ++deltaCount;
        }
        // Incision proxy on a subsampled grid (region texel 2 m; 8 px
        // stride = 16 m samples, mean over +/-3 samples ~ 48 m).
        f64 incSum = 0.0;
        u64 incCount = 0;
        const u32 stride = 8;
        for (u32 row = 24; row + 24 < region.height; row += stride) {
            for (u32 col = 24; col + 24 < region.width; col += stride) {
                const auto at = [&](i32 dc, i32 dr) {
                    return region.heights
                        [static_cast<size_t>(row + dr * stride) *
                             region.width +
                         (col + dc * stride)];
                };
                const f32 h = at(0, 0);
                if (h <= 21.0f) {
                    continue;
                }
                f32 mean = 0.0f;
                for (i32 d = -3; d <= 3; ++d) {
                    mean += at(d, 0) + at(0, d);
                }
                mean /= 14.0f;
                incSum += std::abs(h - mean);
                ++incCount;
            }
        }
        LOG_INFO("{}: mean|dh| vs ref {:.2f} m (max {:.1f}), incision "
                 "~50 m: {:.2f} m",
                 variant.name, deltaCount ? deltaSum / deltaCount : 0.0,
                 deltaMax, incCount ? incSum / incCount : 0.0);

        // Full tile for context, zoomed crop for the sheet: the carved
        // rock lives at 4-8 m — it only reads below ~2 m/px.
        const vector<u8> full = renderRegionRelief(region, 21.0f, size);
        const std::string path =
            outDir + "/" + variant.name + ".png";
        stbi_write_png(path.c_str(), static_cast<int>(size),
                       static_cast<int>(size), 3, full.data(),
                       static_cast<int>(size) * 3);
        const f32 cropX = region.originX + region.spanX() * 0.45f;
        const f32 cropZ = region.originZ + region.spanZ() * 0.45f;
        const vector<u8> pixels = renderRegionRelief(
            region, 21.0f, size, cropX, cropZ, 1800.0f);
        const u32 cellX = (index % kCols) * size;
        const u32 cellY = (index / kCols) * size;
        for (u32 row = 0; row < size; ++row) {
            std::copy_n(
                pixels.begin() + static_cast<ptrdiff_t>(
                                     static_cast<size_t>(row) * size * 3),
                static_cast<size_t>(size) * 3,
                sheet.begin() +
                    static_cast<ptrdiff_t>(
                        (static_cast<size_t>(cellY + row) * kCols * size +
                         cellX) *
                        3));
        }
        ++index;
    }
    const std::string sheetPath = outDir + "/erosion_bench.png";
    stbi_write_png(sheetPath.c_str(), static_cast<int>(kCols * size),
                   static_cast<int>(kRows * size), 3, sheet.data(),
                   static_cast<int>(kCols * size) * 3);
    LOG_INFO("erosion-bench: tile ({}, {}) seed {} -> {} (ordre: v0 ref, "
             "v1 halo, v2 +pente, v3 +crete, v4 complet leger, v5 "
             "complet moyen)",
             tx, tz, seed, sheetPath);
    return 0;
}

} // namespace cooker
