#pragma once
#include <filesystem>

#include "Buffer.h"
#include "Types.h"

namespace graphics {
    struct GLTFMaterial {
        MaterialInstance data;
    };

    struct GeoSurface {
        u32 startIndex;
        u32 count;
        sptr<GLTFMaterial> material;
    };

    struct MeshAsset {
        str name;

        vector<GeoSurface> surfaces;
        GPUMeshBuffers meshBuffers;
    };

    class Renderer;

    std::optional<vector<sptr<MeshAsset>>> loadGltfMeshes(Renderer* engine, const str& filePath);
}