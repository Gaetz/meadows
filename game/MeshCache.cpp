#include "game/MeshCache.hpp"

#include "engine/assets/GltfMesh.hpp"
#include "engine/assets/VertexAo.hpp"
#include "engine/render/MeshBuilder.hpp"

namespace game {

// The vertex-color magenta box: pending/missing props stay visible.
MeshCacheTraits::Payload
MeshCacheTraits::createPlaceholder(rhi::Device& device) {
    render::MeshData box;
    render::appendBox(box, { 0.0f, 0.4f, 0.0f }, { 0.4f, 0.4f, 0.4f },
                      { 1.0f, 0.0f, 1.0f });
    Payload payload;
    payload.gpu.vertices = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = box.vertices.size() * sizeof(render::MeshVertex) },
        box.vertices.data());
    payload.gpu.indices = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = box.indices.size() * sizeof(u32) },
        box.indices.data());
    payload.gpu.indexCount = static_cast<u32>(box.indices.size());
    return payload;
}

void MeshCacheTraits::destroyPlaceholder(rhi::Device& device,
                                         Payload& payload) {
    if (payload.gpu.vertices.id != 0) {
        device.destroyBuffer(payload.gpu.vertices);
        device.destroyBuffer(payload.gpu.indices);
    }
}

// WORKER thread: pure file IO + parse — no GPU, no cache state.
MeshCacheTraits::DecodedData
MeshCacheTraits::decode(const std::filesystem::path& path) {
    auto mesh = assets::loadGltfMesh(path);
    if (mesh) {
        // Prop pivot convention: base on the ground, footprint centered
        // (authored scale kept — references carry the instance scale).
        assets::groundMesh(*mesh);
        // Option B (2026-07-10): ambient grounding baked into the vertex
        // colors on this decode WORKER — kit recesses and prop creases
        // darken for free; screen-space AO left the default look.
        // [cpp-tuning] strength.
        assets::bakeVertexAo(*mesh, 0.5f);
    }
    return mesh;
}

// Main thread: GPU buffers + the retained CPU side (collision, picking).
MeshCacheTraits::Payload MeshCacheTraits::upload(rhi::Device& device,
                                                 DecodedData&& data) {
    Payload payload;
    payload.gpu.vertices = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = data->vertices.size() * sizeof(render::MeshVertex) },
        data->vertices.data());
    payload.gpu.indices = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = data->indices.size() * sizeof(u32) },
        data->indices.data());
    payload.gpu.indexCount = static_cast<u32>(data->indices.size());
    payload.cpu = std::make_unique<CpuMesh>();
    payload.cpu->positions.reserve(data->vertices.size());
    payload.cpu->boundsMin = payload.cpu->boundsMax =
        data->vertices.empty() ? Vec3 { 0.0f } : data->vertices[0].position;
    for (const render::MeshVertex& vertex : data->vertices) {
        payload.cpu->positions.push_back(vertex.position);
        payload.cpu->boundsMin = glm::min(payload.cpu->boundsMin,
                                          vertex.position);
        payload.cpu->boundsMax = glm::max(payload.cpu->boundsMax,
                                          vertex.position);
    }
    payload.cpu->indices = data->indices;
    return payload;
}

void MeshCacheTraits::destroyPayload(rhi::Device& device, Payload& payload) {
    device.destroyBuffer(payload.gpu.vertices);
    device.destroyBuffer(payload.gpu.indices);
}

} // namespace game
