#include "game/MeshCache.hpp"

#include "engine/assets/GltfMesh.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/MeshBuilder.hpp"

namespace game {

namespace {

// The vertex-color magenta box: pending/missing props stay visible.
render::MeshData buildPlaceholderMesh() {
    render::MeshData mesh;
    render::appendBox(mesh, { 0.0f, 0.4f, 0.0f }, { 0.4f, 0.4f, 0.4f },
                      { 1.0f, 0.0f, 1.0f });
    return mesh;
}

} // namespace

MeshCache::MeshCache(rhi::Device& device, const assets::AssetDatabase& assets,
                     core::JobSystem& jobs)
    : device { device }, assets { assets }, jobs { jobs } {
    const render::MeshData box = buildPlaceholderMesh();
    placeholder.vertices = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = box.vertices.size() * sizeof(render::MeshVertex) },
        box.vertices.data());
    placeholder.indices = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = box.indices.size() * sizeof(u32) },
        box.indices.data());
    placeholder.indexCount = static_cast<u32>(box.indices.size());
}

MeshCache::~MeshCache() {
    // No wait: in-flight decodes hold their own ref to `shared` and never
    // touch this cache (TextureCache teardown story).
    clear();
    if (placeholder.vertices.id != 0) {
        device.destroyBuffer(placeholder.vertices);
        device.destroyBuffer(placeholder.indices);
    }
}

const MeshCache::Gpu& MeshCache::resolve(const core::Guid& model) {
    if (!model.isValid()) {
        return placeholder;
    }
    if (const auto it = byGuid.find(model); it != byGuid.end()) {
        return it->second.gpu;
    }

    const auto path = assets.resolve(model);
    if (!path) {
        LOG_WARN("MeshCache: no asset registered for model {}",
                 model.toString());
        byGuid.emplace(model, Entry { placeholder, Residency::Failed });
        return placeholder;
    }

    // First sighting: placeholder now, decode off-thread. The worker only
    // touches the path (file IO + parse, pure CPU) and the shared queue.
    byGuid.emplace(model, Entry { placeholder, Residency::Pending });
    ++pending;
    jobs.enqueue([shared = shared, model, gen = generation, path = *path] {
        auto mesh = assets::loadGltfMesh(path);
        if (mesh) {
            // Prop pivot convention: base on the ground, footprint centered
            // (authored scale kept — references carry the instance scale).
            assets::groundMesh(*mesh);
        }
        shared->decoded.push(Decoded { model, gen, std::move(mesh) });
    });
    return placeholder;
}

u32 MeshCache::pumpUploads() {
    u32 becameResident = 0;
    shared->decoded.drain([&](Decoded&& result) {
        if (result.generation != generation) {
            return; // cancelled by a clear() since it was kicked
        }
        const auto it = byGuid.find(result.model);
        if (it == byGuid.end() || it->second.state != Residency::Pending) {
            return;
        }
        --pending;
        if (!result.mesh || result.mesh->indices.empty()) {
            it->second.state = Residency::Failed; // keeps the placeholder
            return;
        }
        Gpu gpu;
        gpu.vertices = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = result.mesh->vertices.size() *
                      sizeof(render::MeshVertex) },
            result.mesh->vertices.data());
        gpu.indices = device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = result.mesh->indices.size() * sizeof(u32) },
            result.mesh->indices.data());
        gpu.indexCount = static_cast<u32>(result.mesh->indices.size());
        // Retain the CPU side (collision, picking) + local bounds.
        auto cpu = std::make_unique<CpuMesh>();
        cpu->positions.reserve(result.mesh->vertices.size());
        cpu->boundsMin = cpu->boundsMax =
            result.mesh->vertices.empty()
                ? Vec3 { 0.0f }
                : result.mesh->vertices[0].position;
        for (const render::MeshVertex& vertex : result.mesh->vertices) {
            cpu->positions.push_back(vertex.position);
            cpu->boundsMin = glm::min(cpu->boundsMin, vertex.position);
            cpu->boundsMax = glm::max(cpu->boundsMax, vertex.position);
        }
        cpu->indices = result.mesh->indices;
        it->second = Entry { gpu, Residency::Resident, std::move(cpu) };
        ++becameResident;
    });
    return becameResident;
}

const MeshCache::CpuMesh* MeshCache::cpuMesh(const core::Guid& model) const {
    const auto it = byGuid.find(model);
    return it != byGuid.end() && it->second.state == Residency::Resident
               ? it->second.cpu.get()
               : nullptr;
}

void MeshCache::clear() {
    for (auto& [guid, entry] : byGuid) {
        // Only resident entries own buffers; the others hold the shared
        // placeholder (freed once, in the destructor).
        if (entry.state == Residency::Resident) {
            device.destroyBuffer(entry.gpu.vertices);
            device.destroyBuffer(entry.gpu.indices);
        }
    }
    byGuid.clear();
    pending = 0;
    ++generation; // stale in-flight results bounce off on arrival
}

} // namespace game
