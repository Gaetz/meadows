#include "LoadedGLTF.h"
#include <glm/gtx/transform.hpp>
#include "VulkanLoader.h"

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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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

    vk::Filter extractFilter(fastgltf::Filter filter) {
        switch (filter) {
            // nearest samplers
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::NearestMipMapLinear:
            return vk::Filter::eNearest;

            // linear samplers
        case fastgltf::Filter::Linear:
        case fastgltf::Filter::LinearMipMapNearest:
        case fastgltf::Filter::LinearMipMapLinear:
        default:
            return vk::Filter::eLinear;
        }
    }

    vk::SamplerMipmapMode extractMipmapMode(fastgltf::Filter filter) {
        switch (filter) {
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::LinearMipMapNearest:
            return vk::SamplerMipmapMode::eNearest;

        case fastgltf::Filter::NearestMipMapLinear:
        case fastgltf::Filter::LinearMipMapLinear:
        default:
            return vk::SamplerMipmapMode::eLinear;
        }
    }

    std::optional<Image> loadImage(Renderer* engine, fastgltf::Asset& asset, fastgltf::Image& image) {
        std::optional<Image> newImage;

        int width, height, nrChannels;

        std::visit(fastgltf::visitor {
            [](auto& arg) {},
            [&](fastgltf::sources::URI& filePath) {
                assert(filePath.fileByteOffset == 0); // We don't support offsets with stbi
                assert(filePath.uri.isLocalPath()); // We're only testing local files

                const std::string path(filePath.uri.path().begin(), filePath.uri.path().end());
                unsigned char* data = stbi_load(path.c_str(), &width, &height, &nrChannels, 4);
                if (data) {
                    vk::Extent3D imagesize;
                    imagesize.width = width;
                    imagesize.height = height;
                    imagesize.depth = 1;

                    newImage = Image(engine->getContext(), *engine->getImmediateSubmitter(), data, imagesize, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled);

                    stbi_image_free(data);
                }
            },
            [&](fastgltf::sources::Vector& vector) {
                unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(vector.bytes.data()), static_cast<int>(vector.bytes.size()), &width, &height, &nrChannels, 4);
                if (data) {
                    vk::Extent3D imagesize;
                    imagesize.width = width;
                    imagesize.height = height;
                    imagesize.depth = 1;

                    newImage = Image(engine->getContext(), *engine->getImmediateSubmitter(), data, imagesize, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled);

                    stbi_image_free(data);
                }
            },
            [&](fastgltf::sources::BufferView& view) {
                auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                auto& buffer = asset.buffers[bufferView.bufferIndex];

                std::visit(fastgltf::visitor {
                    [](auto& arg) {},
                    [&](fastgltf::sources::Array& array) {
                        unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(array.bytes.data() + bufferView.byteOffset), static_cast<int>(bufferView.byteLength), &width, &height, &nrChannels, 4);
                        if (data) {
                            vk::Extent3D imagesize;
                            imagesize.width = width;
                            imagesize.height = height;
                            imagesize.depth = 1;

                            newImage = Image(engine->getContext(), *engine->getImmediateSubmitter(), data, imagesize, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled);

                            stbi_image_free(data);
                        }
                    },
                    [&](fastgltf::sources::Vector& vector) {
                        unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(vector.bytes.data() + bufferView.byteOffset), static_cast<int>(bufferView.byteLength), &width, &height, &nrChannels, 4);
                        if (data) {
                            vk::Extent3D imagesize;
                            imagesize.width = width;
                            imagesize.height = height;
                            imagesize.depth = 1;

                            newImage = Image(engine->getContext(), *engine->getImmediateSubmitter(), data, imagesize, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled);

                            stbi_image_free(data);
                        }
                    }
                }, buffer.data);
            },
        }, image.data);

        // If loading failed, return empty optional
        if (!newImage.has_value()) {
            Log::Error("Failed to load texture for glTF");
             return {};
        }

        return newImage;
    }

    std::optional<sptr<LoadedGLTF>> loadGltf(Renderer* engine, const str& filePath) {
        Log::Debug("Loading glTF scene: %s", filePath.c_str());

        sptr<LoadedGLTF> scene = std::make_shared<LoadedGLTF>();
        scene->creator = engine;
        LoadedGLTF& file = *scene;

        std::filesystem::path path = services::File::getFileSystemPath(filePath);

        auto dataResult = fastgltf::GltfDataBuffer::FromPath(path);
        if (!dataResult) {
            Log::Error("Failed to load glTF file: %s", fastgltf::getErrorName(dataResult.error()).data());
            return {};
        }

        constexpr auto gltfOptions = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble | fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;

        fastgltf::Asset gltf;
        fastgltf::Parser parser {};

        auto type = fastgltf::determineGltfFileType(dataResult.get());
        if (type == fastgltf::GltfType::glTF) {
            auto load = parser.loadGltf(dataResult.get(), path.parent_path(), gltfOptions);
            if (load) {
                gltf = std::move(load.get());
            } else {
                Log::Error("Failed to load glTF: %s", fastgltf::getErrorName(load.error()).data());
                return {};
            }
        } else if (type == fastgltf::GltfType::GLB) {
            auto load = parser.loadGltfBinary(dataResult.get(), path.parent_path(), gltfOptions);
            if (load) {
                gltf = std::move(load.get());
            } else {
                Log::Error("Failed to load glTF: %s", fastgltf::getErrorName(load.error()).data());
                return {};
            }
        } else {
            Log::Error("Failed to determine glTF container");
            return {};
        }

        // Initialize descriptor pool
        vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
            { vk::DescriptorType::eCombinedImageSampler, 3 },
            { vk::DescriptorType::eUniformBuffer, 3 },
            { vk::DescriptorType::eStorageBuffer, 1 }
        };

        file.descriptorPool = DescriptorAllocatorGrowable(engine->getContext()->getDevice(), static_cast<u32>(gltf.materials.size()), sizes);

        // Load Samplers
        for (fastgltf::Sampler& sampler : gltf.samplers) {
            vk::SamplerCreateInfo samplInfo {};
            samplInfo.maxLod = VK_LOD_CLAMP_NONE;
            samplInfo.minLod = 0;

            samplInfo.magFilter = extractFilter(sampler.magFilter.value_or(fastgltf::Filter::Nearest));
            samplInfo.minFilter = extractFilter(sampler.minFilter.value_or(fastgltf::Filter::Nearest));
            samplInfo.mipmapMode = extractMipmapMode(sampler.minFilter.value_or(fastgltf::Filter::Nearest));

            vk::Sampler newSampler = engine->getContext()->getDevice().createSampler(samplInfo);
            file.samplers.push_back(newSampler);
        }

        // Temporary arrays for indices
        vector<sptr<MeshAsset>> meshes;
        vector<sptr<Node>> nodes;
        vector<Image> images;
        vector<sptr<GLTFMaterial>> materials;

        // Load Images
        for (fastgltf::Image& image : gltf.images) {
            std::optional<Image> img = loadImage(engine, gltf, image);

            if (img.has_value()) {
                images.push_back(*img);
                file.images[image.name.c_str()] = *img; 
            } else {
                // we failed to load, so let's give the slot a default white image to not crash
                images.push_back(engine->errorCheckerboardImage);
                Log::Error("gltf failed to load texture: %s", image.name.c_str());
            }
        }
        
        file.materialDataBuffer = Buffer(engine->getContext(), sizeof(pipelines::GLTFMetallicRoughness::MaterialConstants) * gltf.materials.size(),
            vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU);

        int dataIndex = 0;
        auto* sceneMaterialConstants = static_cast<pipelines::GLTFMetallicRoughness::MaterialConstants*>(file.materialDataBuffer.info.pMappedData);

        for (fastgltf::Material& mat : gltf.materials) {
            sptr<GLTFMaterial> newMat = std::make_shared<GLTFMaterial>();
            materials.push_back(newMat);
            file.materials[mat.name.c_str()] = newMat;

            pipelines::GLTFMetallicRoughness::MaterialConstants constants;
            constants.colorFactors.x = mat.pbrData.baseColorFactor[0];
            constants.colorFactors.y = mat.pbrData.baseColorFactor[1];
            constants.colorFactors.z = mat.pbrData.baseColorFactor[2];
            constants.colorFactors.w = mat.pbrData.baseColorFactor[3];

            constants.metalRoughFactors.x = mat.pbrData.metallicFactor;
            constants.metalRoughFactors.y = mat.pbrData.roughnessFactor;

            sceneMaterialConstants[dataIndex] = constants;

            MaterialPass passType = MaterialPass::MainColor;
            if (mat.alphaMode == fastgltf::AlphaMode::Blend) {
                passType = MaterialPass::Transparent;
            }

            pipelines::GLTFMetallicRoughness::MaterialResources materialResources;
            materialResources.colorImage = engine->whiteImage;
            materialResources.colorSampler = engine->defaultSamplerLinear;
            materialResources.metalRoughImage = engine->whiteImage;
            materialResources.metalRoughSampler = engine->defaultSamplerLinear;
            materialResources.dataBuffer = file.materialDataBuffer.buffer;
            materialResources.dataBufferOffset = dataIndex * sizeof(pipelines::GLTFMetallicRoughness::MaterialConstants);

            if (mat.pbrData.baseColorTexture.has_value()) {
                size_t img = gltf.textures[mat.pbrData.baseColorTexture.value().textureIndex].imageIndex.value();
                size_t sampler = gltf.textures[mat.pbrData.baseColorTexture.value().textureIndex].samplerIndex.value();

                materialResources.colorImage = images[img];
                materialResources.colorSampler = file.samplers[sampler];
            }

            newMat->data = engine->metalRoughMaterial.writeMaterial(engine->getContext()->getDevice(), passType, materialResources, &file.descriptorPool);

            dataIndex++;
        }

        // Load Meshes
        vector<uint32_t> indices;
        vector<Vertex> vertices;

        for (fastgltf::Mesh& mesh : gltf.meshes) {
            sptr<MeshAsset> newMesh = std::make_shared<MeshAsset>();
            meshes.push_back(newMesh);
            file.meshes[mesh.name.c_str()] = newMesh;
            newMesh->name = mesh.name;

            indices.clear();
            vertices.clear();

            for (auto&& p : mesh.primitives) {
                GeoSurface newSurface;
                newSurface.startIndex = static_cast<u32>(indices.size());
                newSurface.count = static_cast<u32>(gltf.accessors[p.indicesAccessor.value()].count);

                size_t initialVtx = vertices.size();

                // Load indices
                {
                    fastgltf::Accessor& indexAccessor = gltf.accessors[p.indicesAccessor.value()];
                    indices.reserve(indices.size() + indexAccessor.count);
                    fastgltf::iterateAccessor<std::uint32_t>(gltf, indexAccessor, [&](std::uint32_t idx) {
                        indices.push_back(idx + static_cast<u32>(initialVtx));
                    });
                }

                // Load vertex positions
                {
                    fastgltf::Accessor& posAccessor = gltf.accessors[p.findAttribute("POSITION")->accessorIndex];
                    vertices.resize(vertices.size() + posAccessor.count);
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor, [&](glm::vec3 v, size_t index) {
                        Vertex newvtx;
                        newvtx.position = v;
                        newvtx.normal = { 1, 0, 0 };
                        newvtx.color = glm::vec4 { 1.f };
                        newvtx.uvX = 0;
                        newvtx.uvY = 0;
                        vertices[initialVtx + index] = newvtx;
                    });
                }

                // Load normals
                auto normals = p.findAttribute("NORMAL");
                if (normals != p.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normals->accessorIndex], [&](glm::vec3 v, size_t index) {
                        vertices[initialVtx + index].normal = v;
                    });
                }

                // Load UVs
                auto uv = p.findAttribute("TEXCOORD_0");
                if (uv != p.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uv->accessorIndex], [&](glm::vec2 v, size_t index) {
                        vertices[initialVtx + index].uvX = v.x;
                        vertices[initialVtx + index].uvY = v.y;
                    });
                }

                // Load Colors
                auto colors = p.findAttribute("COLOR_0");
                if (colors != p.attributes.end()) {
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[colors->accessorIndex], [&](glm::vec4 v, size_t index) {
                        vertices[initialVtx + index].color = v;
                    });
                }

                if (p.materialIndex.has_value()) {
                    newSurface.material = materials[p.materialIndex.value()];
                } else {
                    newSurface.material = materials[0];
                }

                // Calculate bounds from vertex positions
                glm::vec3 minPos = vertices[initialVtx].position;
                glm::vec3 maxPos = vertices[initialVtx].position;
                for (size_t i = initialVtx; i < vertices.size(); i++) {
                    minPos = glm::min(minPos, vertices[i].position);
                    maxPos = glm::max(maxPos, vertices[i].position);
                }
                newSurface.bounds.origin = (maxPos + minPos) / 2.f;
                newSurface.bounds.extents = (maxPos - minPos) / 2.f;
                newSurface.bounds.sphereRadius = glm::length(newSurface.bounds.extents);

                newMesh->surfaces.push_back(newSurface);
            }

            newMesh->meshBuffers = engine->uploadMesh(indices, vertices);
        }

        // Load Nodes
        for (fastgltf::Node& node : gltf.nodes) {
            sptr<Node> newNode;

            if (node.meshIndex.has_value()) {
                newNode = std::make_shared<MeshNode>();
                static_cast<MeshNode*>(newNode.get())->mesh = meshes[*node.meshIndex];
            } else {
                newNode = std::make_shared<Node>();
            }

            nodes.push_back(newNode);
            file.nodes[node.name.c_str()] = newNode;

            std::visit([&](auto&& arg) {
                // Use C++20 concepts to detect type features instead of names
                if constexpr (requires { arg.translation; }) {
                    // It's TRS
                    glm::vec3 tl(arg.translation[0], arg.translation[1], arg.translation[2]);
                    glm::quat rot(arg.rotation[3], arg.rotation[0], arg.rotation[1], arg.rotation[2]);
                    glm::vec3 sc(arg.scale[0], arg.scale[1], arg.scale[2]);

                    glm::mat4 tm = glm::translate(glm::mat4(1.f), tl);
                    glm::mat4 rm = glm::toMat4(rot);
                    glm::mat4 sm = glm::scale(glm::mat4(1.f), sc);

                    newNode->localTransform = tm * rm * sm;
                } else {
                     // It's Matrix (std::array or similar)
                    memcpy(&newNode->localTransform, arg.data(), sizeof(arg));
                }
            }, node.transform);
        }

        // Set up hierarchy
        for (u32 i = 0; i < gltf.nodes.size(); i++) {
            fastgltf::Node& node = gltf.nodes[i];
            sptr<Node>& sceneNode = nodes[i];

            for (auto& childIndex : node.children) {
                sceneNode->children.push_back(nodes[childIndex]);
                nodes[childIndex]->parent = sceneNode;
            }
        }

        // Find top nodes
        for (auto& node : nodes) {
            if (node->parent.expired()) {
                file.topNodes.push_back(node);
                node->refreshTransform(glm::mat4 { 1.f });
            }
        }

        return scene;
    }
}
