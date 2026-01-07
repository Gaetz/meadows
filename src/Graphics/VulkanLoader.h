#pragma once
#include <filesystem>

#include "Buffer.h"
#include "Defines.h"

namespace graphics {
    struct GeoSurface {
        u32 startIndex;
        u32 count;
    };

    struct MeshAsset {
        str name;

        vector<GeoSurface> surfaces;
        GPUMeshBuffers meshBuffers;
    };

    class Renderer;

    std::optional<vector<sptr<MeshAsset>>> loadGltfMeshes(Renderer* engine, const str& filePath);
}