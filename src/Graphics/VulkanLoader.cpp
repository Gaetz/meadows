#include "VulkanLoader.h"

#include "stb_image.h"
#include <iostream>

#include "Renderer.h"
#include "VulkanInit.hpp"
#include "Types.h"
#include <glm/gtx/quaternion.hpp>
#include  "../BasicServices/Log.h"

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>

#include "BasicServices/File.h"
#include "fastgltf/core.hpp"

using services::Log;

namespace graphics {
    std::optional<vector<sptr<MeshAsset>>> loadGltfMeshes(Renderer *engine, const str& filePath) {

        Log::Debug("Loading glTF file: %s", filePath.c_str());
        std::filesystem::path path = services::File::getFileSystemPath(filePath);

        // Load the glTF file into memory
        auto dataResult = fastgltf::GltfDataBuffer::FromPath(path);
        if (!dataResult) {
            Log::Error("Failed to load glTF file from path: %s", fastgltf::getErrorName(dataResult.error()).data());
            return {};
        }

        constexpr auto gltfOptions = fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;

        fastgltf::Asset gltf;
        fastgltf::Parser parser {};

        auto load = parser.loadGltfBinary(dataResult.get(), path.parent_path(), gltfOptions);
        if (load) {
            gltf = std::move(load.get());
        } else {
            Log::Debug("Failed to load glTF: %s", fastgltf::getErrorName(load.error()).data());
            return {};
        }

            std::vector<std::shared_ptr<MeshAsset>> meshes;

    // Use the same vectors for all meshes so that the memory doesnt reallocate as often
    vector<uint32_t> indices;
    vector<Vertex> vertices;
    for (fastgltf::Mesh& mesh : gltf.meshes) {
        MeshAsset newmesh;

        newmesh.name = mesh.name;

        // clear the mesh arrays each mesh, we dont want to merge them by error
        indices.clear();
        vertices.clear();

        for (auto&& p : mesh.primitives) {
            GeoSurface newSurface;
            newSurface.startIndex = static_cast<u32>(indices.size());
            newSurface.count = static_cast<u32>(gltf.accessors[p.indicesAccessor.value()].count);

            size_t initial_vtx = vertices.size();

            // load indexes
            {
                fastgltf::Accessor& indexaccessor = gltf.accessors[p.indicesAccessor.value()];
                indices.reserve(indices.size() + indexaccessor.count);

                fastgltf::iterateAccessor<std::uint32_t>(gltf, indexaccessor,
                    [&](std::uint32_t idx) {
                        indices.push_back(idx + initial_vtx);
                    });
            }

            // load vertex positions
            {
                fastgltf::Accessor& posAccessor = gltf.accessors[p.findAttribute("POSITION")->accessorIndex];
                vertices.resize(vertices.size() + posAccessor.count);

                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor,
                    [&](glm::vec3 v, size_t index) {
                        Vertex newvtx;
                        newvtx.position = v;
                        newvtx.normal = { 1, 0, 0 };
                        newvtx.color = glm::vec4 { 1.f };
                        newvtx.uvX = 0;
                        newvtx.uvY = 0;
                        vertices[initial_vtx + index] = newvtx;
                    });
            }

            // load vertex normals
            auto normals = p.findAttribute("NORMAL");
            if (normals != p.attributes.end()) {

                fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normals->accessorIndex],
                    [&](glm::vec3 v, size_t index) {
                        vertices[initial_vtx + index].normal = v;
                    });
            }

            // load UVs
            auto uv = p.findAttribute("TEXCOORD_0");
            if (uv != p.attributes.end()) {

                fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uv->accessorIndex],
                    [&](glm::vec2 v, size_t index) {
                        vertices[initial_vtx + index].uvX = v.x;
                        vertices[initial_vtx + index].uvY = v.y;
                    });
            }

            // load vertex colors
            auto colors = p.findAttribute("COLOR_0");
            if (colors != p.attributes.end()) {

                fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[colors->accessorIndex],
                    [&](glm::vec4 v, size_t index) {
                        vertices[initial_vtx + index].color = v;
                    });
            }
            newmesh.surfaces.push_back(newSurface);
        }

        // display the vertex normals
        if (constexpr bool OverrideColors = false) {
            for (Vertex& vtx : vertices) {
                vtx.color = glm::vec4(vtx.normal, 1.f);
            }
        }
        newmesh.meshBuffers = engine->uploadMesh(indices, vertices);

        meshes.emplace_back(std::make_shared<MeshAsset>(std::move(newmesh)));
    }

    return meshes;


    }
}
