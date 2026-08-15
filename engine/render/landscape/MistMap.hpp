#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/BakeMailbox.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// Valley data for the ground-mist raymarch (docs/RENDERING.md §3.5), ONE
// worker-baked map (the TerrainLightMap pattern): a 256² RGBA8 texture over
// ~2 km around the camera, R = mist-top height (box-blurred terrain height —
// the "water table" the mist pools under, quantized to 2 m over a fixed
// window anchored at sea level), G = valleyness (how far the terrain sits
// below that smoothed surface — hollows 1, ridges 0). Sun-independent and
// center-snapped to the texel grid, so overlap texels of consecutive bakes
// are bit-identical: the swap can never pop and needs no crossfade.
// Sampled by mist.frag at binding 8 via uMistMapInfo.
class MistMap {
public:
    static constexpr u32 kSize = 256;
    static constexpr f32 kSpan = 2048.0f; // meters covered (8 m texels)
    // R quantization window: [seaLevel + kTopOffset, +kTopRange] (meters).
    static constexpr f32 kTopOffset = -64.0f;
    static constexpr f32 kTopRange = 512.0f;
    // G normalization: hollow depth (meters) that maps to valleyness 1.
    // 16 m: ordinary vales read as misty, not only deep gorges.
    static constexpr f32 kValleyDepth = 16.0f;

    void create(rhi::Device& device, core::JobSystem& jobs);
    void destroy(rhi::Device& device);

    // Pump finished bakes (upload) + kick a new one when the camera
    // strays past a quarter span or the terrain inputs change.
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& focus);

    // {centerX, centerZ, 1/span, max mist-top Y} — .w bounds the
    // raymarch's horizontal slab clip.
    Vec4 info() const { return { center.x, center.y, 1.0f / kSpan, maxTop }; }
    rhi::BindGroupHandle bindGroup() const { return group; }
    bool ready() const { return mailbox.ready(); }

private:
    struct Baked {
        vector<u8> pixels; // RGBA8, R = mist-top height, G = valleyness
        Vec2 center {};
        f32 maxTop { 0.0f };
        u32 seed { 0 };
        f32 seaLevel { 0.0f };
        u64 gen { 0 };
    };

    BakeMailbox<Baked> mailbox;
    rhi::TextureHandle texture {};
    rhi::SamplerHandle sampler {};
    rhi::BindGroupHandle group {};
    Vec2 center {};
    f32 maxTop { 0.0f };
    u32 bakedSeed { 0 };
    f32 bakedSeaLevel { 0.0f };
};

} // namespace render
