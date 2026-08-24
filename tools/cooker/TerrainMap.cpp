#include "TerrainMap.hpp"

#include <cstdio>
#include <cstdlib>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "engine/core/Log.hpp"
#include "engine/terrain/generation/MapExport.hpp"

namespace cooker {

int terrainMap(char** argv, int argc) {
    using namespace render::terraingen;
    const u32 seed = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    const f32 centerX = static_cast<f32>(std::atof(argv[3]));
    const f32 centerZ = static_cast<f32>(std::atof(argv[4]));
    const f32 span = static_cast<f32>(std::atof(argv[5]));
    const char* outPath = argv[6];
    TerrainMapParams params;
    params.centerX = centerX;
    params.centerZ = centerZ;
    params.span = span;
    if (argc >= 8) {
        params.size = static_cast<u32>(std::atoi(argv[7]));
    }

    ProceduralControlParams controlParams;
    controlParams.seed = seed;
    if (argc >= 9) {
        controlParams.continentCarrierWavelength =
            static_cast<f32>(std::atof(argv[8]));
    }
    if (argc >= 10) {
        controlParams.continentCarrierAmp =
            static_cast<f32>(std::atof(argv[9]));
    }
    if (argc >= 11) {
        controlParams.continentLayout = std::atoi(argv[10]) != 0;
    }
    const ProceduralControls controls { controlParams };
    const MacroParams macro;
    const vector<u8> pixels = renderTerrainMap(controls, macro, params);
    const int n = static_cast<int>(params.size);
    if (!stbi_write_png(outPath, n, n, 3, pixels.data(), n * 3)) {
        LOG_ERROR("terrain-map: failed to write {}", outPath);
        return 1;
    }
    LOG_INFO("terrain-map: seed {} center ({}, {}) span {} m -> {} "
             "({}x{}, {} m/px)",
             seed, centerX, centerZ, span, outPath, n, n,
             span / static_cast<f32>(n));
    return 0;
}

} // namespace cooker
