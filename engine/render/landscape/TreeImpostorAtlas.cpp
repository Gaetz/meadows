#include "engine/render/landscape/TreeImpostorAtlas.hpp"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/Log.hpp"

namespace render {

void TreeImpostorAtlas::create(rhi::Device& device, bool hdr) {
    const u32 width = kViews * kTile;
    const u32 height = VegetationSystem::kTreeVariants * kTile;
    atlas = { device, device.createTexture(
        { .width = width,
          .height = height,
          .format = hdr ? rhi::TextureFormat::RGBA16F
                        : rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    depth = { device, device.createTexture(
        { .width = width,
          .height = height,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_RenderAttachment },
        nullptr) };
    framebuffer = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = atlas } },
          .depthAttachment = { .texture = depth } }) };
    sampler = { device, device.createSampler({}) };
    atlasGroup = { device, device.createBindGroup(
        { .entries = { { .binding = kAtlasUnit,
                         .texture = atlas,
                         .sampler = sampler } } }) };
    for (u32 v = 0; v < kViews; ++v) {
        viewUbos[v] = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Uniform,
              .size = sizeof(FrameUniforms),
              .dynamic = true },
            nullptr) };
        viewGroups[v] = { device, device.createBindGroup(
            { .entries = { { .binding = 0, .buffer = viewUbos[v] } } }) };
    }
    instances = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = VegetationSystem::kTreeVariants *
                  sizeof(VegetationSystem::Instance),
          .dynamic = true },
        nullptr) };
    bakedGeneration = 0;
}

void TreeImpostorAtlas::destroy(rhi::Device& /*device*/) {
    instances.reset();
    for (u32 v = 0; v < kViews; ++v) {
        viewGroups[v].reset();
        viewUbos[v].reset();
    }
    atlasGroup.reset();
    sampler.reset();
    framebuffer.reset();
    depth.reset();
    atlas.reset();
    bakedGeneration = 0;
}

void TreeImpostorAtlas::bake(
    rhi::CommandBuffer& cmd, rhi::Device& device, VegetationSystem& vegetation,
    const FrameUniforms& base, rhi::BindGroupHandle shadowGroup,
    const vector<std::pair<u32, rhi::BindGroupHandle>>& passGroups,
    u64 generation) {
    if (framebuffer.get().id == 0) {
        return;
    }
    // One instance per variant at the origin, scaled to its fit: a
    // neutral tint roll (the far shader re-rolls per tree), no yaw,
    // no sway phase, a fade end far beyond the bake camera.
    const auto& bounds = vegetation.treeBoundsAll();
    array<VegetationSystem::Instance, VegetationSystem::kTreeVariants> list {};
    u32 baked = 0;
    for (u32 v = 0; v < VegetationSystem::kTreeVariants; ++v) {
        const f32 fit = glm::max(bounds[v].x, 2.0f * bounds[v].y);
        const f32 scale = fit > 0.01f ? 1.0f / fit : 0.0f;
        list[v] = { { 0.0f, 0.0f, 0.0f, scale }, { 0.0f, 0.5f, 0.0f, 1.0e6f } };
        baked += vegetation.treeVariantReady(v) && scale > 0.0f ? 1 : 0;
    }
    device.updateBuffer(instances, list.data(), sizeof(list), 0);

    // The bake frame: orthographic camera per view, white ambient and
    // no sun (lit = albedo), no fog nor horizon closure, no local
    // lights, no ground anchor, no season, no wind, no fire, no
    // cascade tint, no mirror flip.
    const Vec3 centre { 0.0f, 0.5f, 0.0f };
    for (u32 e = 0; e < kElevations; ++e) {
        for (u32 a = 0; a < kAzimuths; ++a) {
            const f32 az = static_cast<f32>(a) * 6.2831853f /
                           static_cast<f32>(kAzimuths);
            const f32 el = static_cast<f32>(e) * kElevationStep;
            const Vec3 dir { std::sin(az) * std::cos(el), std::sin(el),
                             std::cos(az) * std::cos(el) };
            const Vec3 eye = centre + dir * 10.0f;
            const Mat4 view = glm::lookAt(eye, centre, Vec3 { 0.0f, 1.0f, 0.0f });
            // Reversed-Z (near at ndc 1): far/near swapped in the box.
            const Mat4 proj = glm::ortho(-kBoxHalf, kBoxHalf, -kBoxHalf,
                                         kBoxHalf, 20.0f, 0.0f);
            FrameUniforms u = base;
            u.viewProj = proj * view;
            u.invViewProj = glm::inverse(u.viewProj);
            u.cameraPos = { eye.x, eye.y, eye.z, 1.0f };
            u.sunColor = Vec4 { 0.0f };
            u.ambientColor = { 1.0f, 1.0f, 1.0f, base.ambientColor.w };
            u.fogInfo = Vec4 { 0.0f };
            u.fogLayerInfo.y = 0.0f;
            u.fogSunInfo = Vec4 { 0.0f };
            u.clusterInfo.x = 0.0f;
            u.terrainShadeMapInfo.w = 0.0f;
            u.seasonInfo = Vec4 { 0.0f };
            u.windInfo = Vec4 { 0.0f };
            u.fireScorchInfo.w = 0.0f;
            u.postInfo.z = 0.0f;
            u.leafLodInfo.z = 0.0f;
            u.stormInfo = Vec4 { 0.0f };
            const u32 i = e * kAzimuths + a;
            device.updateBuffer(viewUbos[i], &u, sizeof(u), 0);
        }
    }

    cmd.beginRenderPass({ .framebuffer = framebuffer,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 0.0f },
                          .depthLoadOp = rhi::LoadOp::Clear,
                          .clearDepth = 0.0f, // reversed-Z far
                          .depthStoreOp = rhi::StoreOp::DontCare });
    for (const auto& [set, group] : passGroups) {
        if (group.id != 0) {
            cmd.setBindGroup(set, group);
        }
    }
    for (u32 i = 0; i < kViews; ++i) {
        for (u32 v = 0; v < VegetationSystem::kTreeVariants; ++v) {
            if (!vegetation.treeVariantReady(v) || list[v].positionScale.w <= 0.0f) {
                continue;
            }
            cmd.setViewport(i * kTile, v * kTile, kTile, kTile);
            cmd.setScissor(i * kTile, v * kTile, kTile, kTile);
            vegetation.drawTreeVariantBake(
                cmd, viewGroups[i].get(), shadowGroup, v, instances.get(),
                static_cast<u64>(v) * sizeof(VegetationSystem::Instance));
        }
    }
    cmd.clearScissor();
    cmd.endRenderPass();
    bakedGeneration = generation;
    LOG_INFO("TreeImpostorAtlas: {} variant(s) x {} views baked ({}x{})",
             baked, kViews, kViews * kTile,
             VegetationSystem::kTreeVariants * kTile);
}

} // namespace render
