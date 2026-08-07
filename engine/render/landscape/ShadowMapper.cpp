#include "engine/render/landscape/ShadowMapper.hpp"

#include <cmath>

#include "engine/rhi/Device.hpp"

namespace render {

namespace {
// View-distance boundaries of the cascade slices (meters). The last slice
// reaches the ultra tree ring, so distant canopies still ground themselves;
// it casts with the cheap solid shadow proxies (VegetationSystem).
constexpr f32 kSplits[ShadowMapper::kCascadeCount + 1] = { 0.5f, 90.0f,
                                                           320.0f, 1600.0f };
// How far behind the slice the light's near plane sits: tall terrain and
// trees outside the slice still cast into it.
constexpr f32 kCasterReach = 350.0f;
} // namespace

void ShadowMapper::create(rhi::Device& device, u32 resolution) {
    resolution_ = glm::clamp(resolution, 512u, 8192u);
    shadowMap = device.createTexture(
        { .width = resolution_,
          .height = resolution_,
          .arrayLayers = kCascadeCount,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    compareSampler = device.createSampler(
        { .minFilter = rhi::FilterMode::Linear,
          .magFilter = rhi::FilterMode::Linear,
          .compare = rhi::CompareFunc::LessEqual }); // hardware PCF fetches
    for (u32 i = 0; i < kCascadeCount; ++i) {
        framebuffers_[i] = device.createFramebuffer(
            { .depthAttachment = { .texture = shadowMap, .arrayLayer = i } });
        cascadeUbos[i] =
            device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                  .size = sizeof(Mat4),
                                  .dynamic = true },
                                nullptr);
        casterGroups[i] = device.createBindGroup(
            { .entries = { { .binding = 1, .buffer = cascadeUbos[i] } } });
    }
    receiverGroup = device.createBindGroup(
        { .entries = { { .binding = 1,
                         .texture = shadowMap,
                         .sampler = compareSampler } } });
}

void ShadowMapper::destroy(rhi::Device& device) {
    device.destroyBindGroup(receiverGroup);
    for (u32 i = 0; i < kCascadeCount; ++i) {
        device.destroyBindGroup(casterGroups[i]);
        device.destroyBuffer(cascadeUbos[i]);
        device.destroyFramebuffer(framebuffers_[i]);
    }
    device.destroySampler(compareSampler);
    device.destroyTexture(shadowMap);
    *this = ShadowMapper {};
}

ShadowMapper::Cascades
ShadowMapper::computeCascades(const Camera3D& camera, f32 aspect,
                              const Vec3& sunDirection) const {
    Cascades result;
    const Vec3 forward = camera.forward();
    const Vec3 right = camera.right();
    const Vec3 up = glm::normalize(glm::cross(right, forward));
    const f32 tanHalfFovY = std::tan(camera.fovY * 0.5f);
    const f32 tanHalfFovX = tanHalfFovY * aspect;

    for (u32 i = 0; i < kCascadeCount; ++i) {
        const f32 near = kSplits[i];
        const f32 far = kSplits[i + 1];
        result.splitFar[i] = far;

        // Bounding sphere of the frustum slice: constant size under camera
        // rotation, so the ortho extent (and texel density) never pulses.
        Vec3 corners[8];
        u32 corner = 0;
        for (const f32 d : { near, far }) {
            for (const f32 sx : { -1.0f, 1.0f }) {
                for (const f32 sy : { -1.0f, 1.0f }) {
                    corners[corner++] = camera.position + forward * d +
                                        right * (sx * tanHalfFovX * d) +
                                        up * (sy * tanHalfFovY * d);
                }
            }
        }
        Vec3 center { 0.0f };
        for (const Vec3& c : corners) {
            center += c;
        }
        center /= 8.0f;
        f32 radius = 0.0f;
        for (const Vec3& c : corners) {
            radius = glm::max(radius, glm::length(c - center));
        }
        radius = std::ceil(radius); // quantize so the extent is stable

        // The light camera sits on the SUN's side (sunDirection points toward
        // the sun), looking back at the slice.
        const Mat4 view =
            glm::lookAt(center + sunDirection * (radius + kCasterReach),
                        center, Vec3 { 0.0f, 1.0f, 0.0f });
        Mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.0f,
                               2.0f * radius + kCasterReach);

        // Texel snap: shift the projection so world origin lands on a texel
        // boundary — shadows stay rock solid while the camera translates.
        const f32 effRes =
            static_cast<f32>(resolution_) * cascadeViewportScale(i);
        Mat4 viewProj = proj * view;
        const Vec4 origin = viewProj * Vec4 { 0.0f, 0.0f, 0.0f, 1.0f } *
                            (effRes * 0.5f);
        const Vec2 rounded { std::round(origin.x), std::round(origin.y) };
        const Vec2 offset =
            (rounded - Vec2 { origin.x, origin.y }) * (2.0f / effRes);
        proj[3][0] += offset.x;
        proj[3][1] += offset.y;

        result.viewProj[i] = proj * view;
        result.texelWorld[i] = (2.0f * radius) / effRes;
    }
    return result;
}

void ShadowMapper::updateCascadeUbos(rhi::Device& device,
                                     const Cascades& cascades) {
    for (u32 i = 0; i < kCascadeCount; ++i) {
        device.updateBuffer(cascadeUbos[i], &cascades.viewProj[i],
                            sizeof(Mat4), 0);
    }
}

} // namespace render
