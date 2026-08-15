#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/BakeMailbox.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// The low-frequency "region shading" band of the terrain material system
// (docs/TERRAIN-TEXTURING.md): biome attributes and baked masks resolved
// to CONTINUOUS fields (ids never blend — the resolve happens here, so the
// GPU's bilinear filtering is legitimate), worker-baked over the camera
// ring as two RGBA8 maps (the TerrainLightMap pattern):
//   T0: rgb = macro tint multiplier (neutral white until the tint brick
//       composes it), a = wetness
//   T1: r = biome rockiness, g = snow-line offset ((v*255-128)*8 m),
//       b = sandiness, a = baked beach mask
// terrain.frag samples them at units 4/5 via uTerrainShadeMapInfo; the CPU
// truth is terrain::regionShadingAt — bake and mirrors share it.
class TerrainShadeMap {
public:
    static constexpr u32 kSize = 512;     // ~6 m texels over the span
    static constexpr f32 kSpan = 3072.0f; // covers the max terrain ring

    void create(rhi::Device& device, core::JobSystem& jobs);
    void destroy(rhi::Device& device);

    // Pump finished bakes (upload) + kick a new one when the focus strayed
    // or the terrain content changed (TerrainBase republished).
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& focus);

    // {centerX, centerZ, 1/span, valid}.
    Vec4 info() const {
        return { center.x, center.y, 1.0f / kSpan,
                 mailbox.ready() ? 1.0f : 0.0f };
    }
    rhi::BindGroupHandle bindGroup() const { return group; }
    bool ready() const { return mailbox.ready(); }

private:
    struct Baked {
        vector<u8> t0;
        vector<u8> t1;
        Vec2 center {};
        u64 stamp { 0 };
        u64 gen { 0 };
    };

    BakeMailbox<Baked> mailbox;
    rhi::TextureHandle texture0 {};
    rhi::TextureHandle texture1 {};
    rhi::SamplerHandle sampler {};
    rhi::BindGroupHandle group {};
    Vec2 center {};
    u64 bakedStamp { 0 };
};

} // namespace render
