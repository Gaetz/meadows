// The asset trade: generated tree meshes and their LOD twins, scanned
// prop meshes/albedos, bark textures, the async reseed and the
// showcase override. Split from VegetationSystem.cpp (one TU per
// trade).
#include "engine/render/landscape/VegetationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/assets/VertexAoCache.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/landscape/TreeGenerator.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"


namespace render {

using core::hashU32;

void VegetationSystem::rebuildLeafMask(rhi::Device& device) {
    constexpr u32 kMaskSize = 256;
    // Full mip chain when the device can fill it — alpha-tested cards
    // shrink to a few pixels at the ring edge; base-level-only sampling
    // there is pure shimmer.
    const bool mips = device.caps().mipmapGeneration;
    const u32 mipLevels =
        mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(kMaskSize)))
             : 1;
    // ATLAS, kLeafStyleCount tiles in a row: each SPECIES claims the
    // slot `leafStyle` and rasters it from its own leaf params + shape;
    // the card's flag bias picks the tile in tree.vert /
    // shadow_prop.vert. Slot 0 defaults to the live global params;
    // unclaimed slots copy slot 0 (a mis-set style never shows holes).
    // The per-slot SEASON table (autumn tint + seasonality) rides the
    // same claim pass — the shaders read it from the frame UBO.
    array<std::optional<ColonizedTreeParams>, kLeafStyleCount> claims {};
    claims[0] = colonizedTreeParams;
    const auto claim = [&](const TreeSpecies& species) {
        if (!species.colonized) {
            return;
        }
        const u32 slot = static_cast<u32>(
            glm::clamp(species.params.leafStyle, 0, kLeafStyleCount - 1));
        claims[slot] = species.params;
    };
    for (u32 i = 0; i < kTreeVariants; ++i) {
        if (treeSpecies[i]) {
            claim(*treeSpecies[i]);
        }
    }
    if (bushSpecies) {
        claim(*bushSpecies);
    }
    vector<u8> pixels(static_cast<size_t>(kMaskSize) * kMaskSize *
                      kLeafStyleCount * 4);
    const u32 atlasWidth = kMaskSize * kLeafStyleCount;
    for (u32 slot = 0; slot < static_cast<u32>(kLeafStyleCount); ++slot) {
        const ColonizedTreeParams& p =
            claims[slot] ? *claims[slot] : *claims[0];
        leafSeasonTable[slot] = { p.autumnTint.x, p.autumnTint.y,
                                  p.autumnTint.z, p.seasonality };
        const vector<u8> tile = generateLeafMaskPixels(
            kMaskSize, meshSeed, p, claims[slot] ? p.leafShape : 0);
        for (u32 row = 0; row < kMaskSize; ++row) {
            std::memcpy(&pixels[(static_cast<size_t>(row) * atlasWidth +
                                 slot * kMaskSize) *
                                4],
                        &tile[static_cast<size_t>(row) * kMaskSize * 4],
                        static_cast<size_t>(kMaskSize) * 4);
        }
    }
    leafMask = { device, device.createTexture(
        { .width = atlasWidth,
          .height = kMaskSize,
          .mipLevels = mipLevels,
          .format = rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear },
        pixels.data()) };
    if (mips) {
        device.generateMipmaps(leafMask.get());
    }
    if (leafMaskSampler.get().id == 0) {
        leafMaskSampler = { device,
                            device.createSampler(
                                { .mipmapFilter = mips }) }; // linear clamp
    }
    leafMaskGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = leafMask.get(),
                         .sampler = leafMaskSampler.get() },
                       // Same layout as the textured-prop groups: a flat
                       // normal fills the map slot (cards don't use it),
                       // and the bark slot (inert without the flag).
                       { .binding = 12,
                         .texture = flatNormalHandle(device),
                         .sampler = leafMaskSampler.get() },
                       { .binding = 13,
                         .texture = flatNormalHandle(device),
                         .sampler = leafMaskSampler.get() },
                       { .binding = 14,
                         .texture = flatNormalHandle(device),
                         .sampler = leafMaskSampler.get() } } }) };
    // Tree bark groups reference the leaf mask just recreated.
    rebuildTreeBarkGroups(device);
}

void VegetationSystem::createVariantMeshes(rhi::Device& device,
                                           u32 terrainSeed) {
    // Ambient grounding is BAKED into the vertex
    // colors — canopy interiors and rock creases darken with zero
    // runtime cost. Content-keyed DISK CACHE (same store as the glTF
    // bakes): these 17 synchronous bakes cost ~a minute in an
    // unoptimized Debug build — once per geometry now, then loads.
    // [cpp-tuning] strengths below.
    const std::filesystem::path aoCacheDir =
        platform::executableDir() / "data" / "cache" / "ao";
    const auto baked = [&](MeshData mesh, f32 strength) {
        assets::applyContentKeyedVertexAo(mesh, aoCacheDir, strength);
        return mesh;
    };
    for (u32 i = 0; i < kVariantCount; ++i) {
        if (const auto it = meshOverrides.find(i);
            it != meshOverrides.end()) {
            uploadVariantMesh(device, i, baked(it->second.high, 0.55f));
            if (!it->second.low.vertices.empty()) {
                uploadLowDetailMesh(device, i,
                                    baked(it->second.low, 0.55f));
            }
            if (!it->second.ultra.vertices.empty()) {
                uploadUltraDetailMesh(device, i,
                                      baked(it->second.ultra, 0.55f));
            }
            continue;
        }
        const u32 seed = hashU32(terrainSeed) + i * 977u;
        if (i < kFirstRock) {
            const TreeSpecies species = speciesFor(i);
            const auto tree = [&](u32 lod) {
                return species.colonized
                           ? generateColonizedTree(seed, lod,
                                                   species.params)
                           : generateTree(seed, lod, species.lobes);
            };
            uploadVariantMesh(device, i, baked(tree(2), 0.6f));
            uploadLowDetailMesh(device, i, baked(tree(1), 0.6f));
            // Bare-icosahedron lobes (~150 tris/tree) for the far
            // ring — same seed, same composition, facets invisible there.
            uploadUltraDetailMesh(device, i, baked(tree(0), 0.6f));
            if (species.colonized) {
                // Hero-near twin: 24-sided wood for the camera chunk.
                uploadNearDetailMesh(device, i, baked(tree(3), 0.6f));
                // Far-cascade caster: solid metaball blobs, no AO bake
                // (depth-only) — see generateColonizedTreeShadowProxy.
                uploadShadowProxyMesh(
                    device, i,
                    generateColonizedTreeShadowProxy(seed,
                                                     species.params));
            }
        } else if (i < kFirstBush || i >= kFirstDebris) {
            // Rocks — and the debris slots' PLACEHOLDER until the scene's
            // scanned-prop overrides land (meshOverrides above wins).
            uploadVariantMesh(device, i, baked(generateRock(seed), 0.5f));
        } else {
            if (bushSpecies) {
                // A knee-high colonized canopy: real branches + card
                // foliage, three LODs like the trees.
                const auto bush = [&](u32 lod) {
                    return bushSpecies->colonized
                               ? generateColonizedTree(
                                     seed, lod, bushSpecies->params)
                               : generateTree(seed, lod,
                                              bushSpecies->lobes);
                };
                uploadVariantMesh(device, i, baked(bush(2), 0.55f));
                uploadLowDetailMesh(device, i, baked(bush(1), 0.55f));
                uploadUltraDetailMesh(device, i, baked(bush(0), 0.55f));
            } else {
                uploadVariantMesh(device, i,
                                  baked(generateBush(seed), 0.55f));
            }
        }
    }
    // Textured plants: the reset above dropped their albedo bind groups
    // with the meshes — re-create them from the kept CPU copies.
    for (const auto& [variant, albedo] : albedoOverrides) {
        (void)albedo;
        uploadVariantAlbedo(device, variant);
    }
    rebuildTreeBarkGroups(device);
}

VegetationSystem::TreeSilhouette VegetationSystem::treeSilhouette() const {
    Vec3 sum { 0.0f };
    u32 count = 0;
    for (const Vec3& bounds : treeBounds) {
        if (bounds.x > 0.1f) {
            sum += bounds;
            ++count;
        }
    }
    if (count == 0) {
        return {}; // defaults until the first variant lands
    }
    const Vec3 mean = sum / static_cast<f32>(count);
    const f32 scale = (kTreeScaleMin + kTreeScaleMax) * 0.5f;
    return { mean.x * scale,
             glm::clamp(2.0f * mean.y / glm::max(mean.x, 0.1f), 0.4f,
                        1.4f),
             glm::clamp(mean.z / glm::max(mean.x, 0.1f), 0.05f, 0.7f) };
}

namespace {

// One GPU mesh level = (vertex buffer, index buffer, count) — the five
// upload entry points differ only in WHICH VariantMesh triple they fill.
void uploadMeshBuffers(rhi::Device& device, const MeshData& mesh,
                       rhi::UniqueBuffer& vertexBuffer,
                       rhi::UniqueBuffer& indexBuffer, u32& indexCount) {
    indexCount = static_cast<u32>(mesh.indices.size());
    vertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    indexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

} // namespace

void VegetationSystem::uploadVariantMesh(rhi::Device& device, u32 variant,
                                         const MeshData& mesh) {
    if (variant < kTreeVariants && !mesh.vertices.empty()) {
        // Silhouette measurement (see TreeSilhouette): height, widest
        // radial extent, and where the crown starts (first height with
        // real width — everything below is bare trunk).
        f32 maxY = 0.0f;
        f32 maxR = 0.0f;
        for (const MeshVertex& v : mesh.vertices) {
            maxY = glm::max(maxY, v.position.y);
            maxR = glm::max(maxR, glm::length(Vec2 { v.position.x,
                                                     v.position.z }));
        }
        f32 crownStart = maxY;
        for (const MeshVertex& v : mesh.vertices) {
            if (glm::length(Vec2 { v.position.x, v.position.z }) >
                maxR * 0.35f) {
                crownStart = glm::min(crownStart, v.position.y);
            }
        }
        treeBounds[variant] = { maxY, maxR, crownStart };
    }
    uploadMeshBuffers(device, mesh, variantMeshes[variant].vertexBuffer,
                      variantMeshes[variant].indexBuffer,
                      variantMeshes[variant].indexCount);
}

void VegetationSystem::uploadLowDetailMesh(rhi::Device& device, u32 variant,
                                           const MeshData& mesh) {
    uploadMeshBuffers(device, mesh,
                      variantMeshes[variant].lowVertexBuffer,
                      variantMeshes[variant].lowIndexBuffer,
                      variantMeshes[variant].lowIndexCount);
}

void VegetationSystem::uploadUltraDetailMesh(rhi::Device& device, u32 variant,
                                             const MeshData& mesh) {
    uploadMeshBuffers(device, mesh,
                      variantMeshes[variant].ultraVertexBuffer,
                      variantMeshes[variant].ultraIndexBuffer,
                      variantMeshes[variant].ultraIndexCount);
}

void VegetationSystem::uploadNearDetailMesh(rhi::Device& device,
                                            u32 variant,
                                            const MeshData& mesh) {
    uploadMeshBuffers(device, mesh,
                      variantMeshes[variant].nearVertexBuffer,
                      variantMeshes[variant].nearIndexBuffer,
                      variantMeshes[variant].nearIndexCount);
}

void VegetationSystem::uploadShadowProxyMesh(rhi::Device& device,
                                             u32 variant,
                                             const MeshData& mesh) {
    uploadMeshBuffers(device, mesh,
                      variantMeshes[variant].casterVertexBuffer,
                      variantMeshes[variant].casterIndexBuffer,
                      variantMeshes[variant].casterIndexCount);
}

void VegetationSystem::overrideVariantMesh(rhi::Device& device, u32 variant,
                                           MeshData mesh, MeshData low,
                                           MeshData ultra) {
    if (variant >= kVariantCount || mesh.vertices.empty() ||
        mesh.indices.empty()) {
        return;
    }
    // U3-7: the reset frees every detail level through its wrappers.
    variantMeshes[variant] = {};
    uploadVariantMesh(device, variant, mesh);
    if (!low.vertices.empty() && !low.indices.empty()) {
        uploadLowDetailMesh(device, variant, low);
    }
    if (!ultra.vertices.empty() && !ultra.indices.empty()) {
        uploadUltraDetailMesh(device, variant, ultra);
    }
    meshOverrides[variant] = { std::move(mesh), std::move(low),
                               std::move(ultra) };
}

void VegetationSystem::setVariantAlbedo(rhi::Device& device, u32 variant,
                                        u32 width, u32 height,
                                        vector<u8> rgba, u32 normalWidth,
                                        u32 normalHeight,
                                        vector<u8> normalRgba) {
    if (variant >= kVariantCount || rgba.size() <
                                        static_cast<size_t>(width) *
                                            height * 4) {
        return;
    }
    if (normalRgba.size() <
        static_cast<size_t>(normalWidth) * normalHeight * 4) {
        normalWidth = 0;
        normalHeight = 0;
        normalRgba.clear();
    }
    albedoOverrides[variant] = { width,        height,
                                 std::move(rgba), normalWidth,
                                 normalHeight, std::move(normalRgba) };
    uploadVariantAlbedo(device, variant);
}

rhi::TextureHandle VegetationSystem::flatNormalHandle(rhi::Device& device) {
    if (flatNormal.get().id == 0) {
        const u8 up[4] = { 128, 128, 255, 255 };
        flatNormal = { device, device.createTexture(
                                   { .width = 1, .height = 1 }, up) };
    }
    return flatNormal.get();
}

void VegetationSystem::setBarkTextures(rhi::Device& device,
                                       BarkImage oakAlbedo,
                                       BarkImage oakNrmHeight,
                                       BarkImage pineAlbedo,
                                       BarkImage pineNrmHeight) {
    barkImages[0] = std::move(oakAlbedo);
    barkImages[1] = std::move(pineAlbedo);
    barkNrmImages[0] = std::move(oakNrmHeight);
    barkNrmImages[1] = std::move(pineNrmHeight);
    const bool mips = device.caps().mipmapGeneration;
    const auto upload = [&](const BarkImage& src, bool srgb,
                            rhi::UniqueTexture& out) {
        if (src.rgba.size() <
            static_cast<size_t>(src.width) * src.height * 4) {
            return;
        }
        const u32 mipLevels =
            mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(
                       glm::max(src.width, src.height))))
                 : 1;
        out = { device, device.createTexture(
            { .width = src.width,
              .height = src.height,
              .mipLevels = mipLevels,
              .format = srgb ? rhi::TextureFormat::SRGBA8
                             : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear },
            src.rgba.data()) };
        if (mips) {
            device.generateMipmaps(out.get());
        }
    };
    for (u32 b = 0; b < 2; ++b) {
        upload(barkImages[b], true, barkTextures[b]);
        upload(barkNrmImages[b], false, barkNrmTextures[b]);
    }
    if (barkSampler.get().id == 0) {
        barkSampler = { device, device.createSampler(
            { .mipmapFilter = mips,
              .addressU = rhi::AddressMode::Repeat,
              .addressV = rhi::AddressMode::Repeat }) };
    }
    rebuildTreeBarkGroups(device);
}

void VegetationSystem::rebuildTreeBarkGroups(rhi::Device& device) {
    if (!barkLoaded() || leafMask.get().id == 0) {
        return;
    }
    for (u32 i = 0; i < kTreeVariants; ++i) {
        const u32 pick = glm::min<u32>(variantBark[i], 1u);
        const rhi::TextureHandle bark =
            barkTextures[pick].get().id != 0 ? barkTextures[pick].get()
                                             : barkTextures[0].get();
        const rhi::TextureHandle nrm =
            barkNrmTextures[pick].get().id != 0
                ? barkNrmTextures[pick].get()
                : flatNormalHandle(device);
        variantMeshes[i].albedoGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = leafMask.get(),
                             .sampler = leafMaskSampler.get() },
                           { .binding = 12,
                             .texture = flatNormalHandle(device),
                             .sampler = leafMaskSampler.get() },
                           { .binding = 13,
                             .texture = bark,
                             .sampler = barkSampler.get() },
                           { .binding = 14,
                             .texture = nrm,
                             .sampler = barkSampler.get() } } }) };
    }
}

void VegetationSystem::uploadVariantAlbedo(rhi::Device& device,
                                           u32 variant) {
    const auto it = albedoOverrides.find(variant);
    if (it == albedoOverrides.end()) {
        return;
    }
    const AlbedoOverride& src = it->second;
    VariantMesh& mesh = variantMeshes[variant];
    const bool mips = device.caps().mipmapGeneration;
    const u32 mipLevels =
        mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(
                   glm::max(src.width, src.height))))
             : 1;
    mesh.albedo = { device, device.createTexture(
        { .width = src.width,
          .height = src.height,
          .mipLevels = mipLevels,
          .format = rhi::TextureFormat::SRGBA8,
          .filter = rhi::FilterMode::Linear },
        src.rgba.data()) };
    if (mips) {
        device.generateMipmaps(mesh.albedo.get());
    }
    if (leafMaskSampler.get().id == 0) {
        leafMaskSampler = { device, device.createSampler(
                                        { .mipmapFilter = mips }) };
    }
    // Normal map (linear RGBA8, GL +Y) or the shared flat fallback.
    rhi::TextureHandle normalTex = flatNormalHandle(device);
    if (!src.normalRgba.empty()) {
        const u32 nMips =
            mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(
                       glm::max(src.normalWidth, src.normalHeight))))
                 : 1;
        mesh.normalMap = { device, device.createTexture(
            { .width = src.normalWidth,
              .height = src.normalHeight,
              .mipLevels = nMips,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear },
            src.normalRgba.data()) };
        if (mips) {
            device.generateMipmaps(mesh.normalMap.get());
        }
        normalTex = mesh.normalMap.get();
    }
    if (propAlbedoSampler.get().id == 0) {
        propAlbedoSampler = { device, device.createSampler(
            { .mipmapFilter = mips,
              .addressU = rhi::AddressMode::Repeat,
              .addressV = rhi::AddressMode::Repeat }) };
    }
    // Same layout as leafMaskGroup — the tree pipeline binds either
    // interchangeably as group 1 (binding 7 = the bark slot, dummy here:
    // textured props never raise the bark flag). REPEAT sampler: scan
    // uvs may exceed [0,1].
    mesh.albedoGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = mesh.albedo.get(),
                         .sampler = propAlbedoSampler.get() },
                       { .binding = 12,
                         .texture = normalTex,
                         .sampler = propAlbedoSampler.get() },
                       { .binding = 13,
                         .texture = mesh.albedo.get(),
                         .sampler = propAlbedoSampler.get() },
                       { .binding = 14,
                         .texture = normalTex,
                         .sampler = propAlbedoSampler.get() } } }) };
}

void VegetationSystem::destroyVariantMeshes(rhi::Device& device) {
    (void)device; // U3-7: assignment frees through the wrappers
    for (VariantMesh& variant : variantMeshes) {
        variant = {};
    }
}

VegetationSystem::TreeSpecies VegetationSystem::speciesFor(
    u32 slot) const {
    if (slot < kTreeVariants && treeSpecies[slot]) {
        return *treeSpecies[slot];
    }
    return { colonizationTrees, lobeTreeParams, colonizedTreeParams };
}

void VegetationSystem::reseedVariantMeshesAsync(core::JobSystem& jobs,
                                                u32 seed) {
    reseedJobs = &jobs;
    if (reseedJob) {
        // Coalesce: the in-flight job lands first, then relaunches with
        // the LATEST params (knob storms collapse into two passes).
        reseedQueued = true;
        reseedQueuedSeed = seed;
        return;
    }
    meshSeed = seed;
    auto job = std::make_shared<ReseedJob>();
    job->seed = seed;
    job->total = 0;
    for (u32 i = 0; i < kTreeVariants; ++i) {
        job->species[i] = speciesFor(i);
        job->total += job->species[i].colonized ? 5u : 3u;
    }
    job->aoCacheDir =
        platform::executableDir() / "data" / "cache" / "ao";
    reseedJob = job;
    jobs.enqueue([job] {
        // Pure CPU (mesh generation + content-keyed AO bake) — the
        // MeshCache decode-worker pattern; only the sptr is captured.
        const auto baked = [&](MeshData mesh) {
            assets::applyContentKeyedVertexAo(mesh, job->aoCacheDir, 0.6f);
            return mesh;
        };
        for (u32 i = 0; i < kTreeVariants; ++i) {
            const TreeSpecies& species = job->species[i];
            const u32 variantSeed = hashU32(job->seed) + i * 977u;
            const u32 lodCount = species.colonized ? 4u : 3u;
            for (u32 lod = 0; lod < lodCount; ++lod) {
                job->lods[i][lod] = baked(
                    species.colonized
                        ? generateColonizedTree(variantSeed, lod,
                                                species.params)
                        : generateTree(variantSeed, lod, species.lobes));
                job->completed.fetch_add(1, std::memory_order_release);
            }
            if (species.colonized) {
                job->casters[i] = generateColonizedTreeShadowProxy(
                    variantSeed, species.params);
                job->completed.fetch_add(1, std::memory_order_release);
            }
        }
        job->done.store(true, std::memory_order_release);
    });
}

f32 VegetationSystem::reseedProgress() const {
    if (!reseedJob) {
        return 1.0f;
    }
    return static_cast<f32>(
               reseedJob->completed.load(std::memory_order_acquire)) /
           static_cast<f32>(glm::max(reseedJob->total, 1u));
}

void VegetationSystem::pumpReseed(rhi::Device& device) {
    if (!reseedJob || !reseedJob->done.load(std::memory_order_acquire)) {
        return;
    }
    const sptr<ReseedJob> job = std::move(reseedJob);
    for (u32 i = 0; i < kTreeVariants; ++i) {
        if (meshOverrides.contains(i)) {
            continue; // authored meshes keep their single level
        }
        variantMeshes[i] = {}; // clears a stale caster on an algo switch
        uploadVariantMesh(device, i, job->lods[i][2]);
        uploadLowDetailMesh(device, i, job->lods[i][1]);
        uploadUltraDetailMesh(device, i, job->lods[i][0]);
        if (job->species[i].colonized) {
            uploadNearDetailMesh(device, i, job->lods[i][3]);
            uploadShadowProxyMesh(device, i, job->casters[i]);
        }
    }
    rebuildLeafMask(device); // its knobs ride the same panel
    if (reseedQueued) {
        reseedQueued = false;
        if (reseedJobs != nullptr) {
            reseedVariantMeshesAsync(*reseedJobs, reseedQueuedSeed);
        }
    }
}

void VegetationSystem::setShowcase(rhi::Device& device,
                                   const vector<Instance>& list) {
    showcaseInstances.reset();
    showcaseCount = static_cast<u32>(list.size());
    if (list.empty()) {
        return;
    }
    showcaseInstances = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = list.size() * sizeof(Instance) },
        list.data()) };
}


} // namespace render
