#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/BakeMailbox.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// Long-range terrain sun shadows + skylighting, ONE
// worker-baked map (the cloud-map pattern): a 512² texture over ~1.5 km
// around the focus, R = sun visibility (the height function marched
// toward the sun — mountains cast on valleys far beyond the CSM),
// G = sky openness (8 azimuth horizons — valley floors get less sky
// ambient). Re-baked on a worker when the QUANTIZED sun steps (the
// cascade hysteresis, ~8 real seconds) or the focus strays; sampled by
// terrain/mesh/skinned at texture unit 7 via uTerrainLightInfo.
class TerrainLightMap {
public:
    // 1024 -> ~3 m texels: at grazing sun the long-shadow fronts in R
    // band at texel scale (they ride into the GI through the inject and
    // the direct term alike) — ~6 m texels showed on sunset slopes. The
    // span must cover the max terrain ring (45 chunks = 2880 m).
    static constexpr u32 kSize = 1024;
    static constexpr f32 kSpan = 3072.0f; // meters covered

    void create(rhi::Device& device, core::JobSystem& jobs);
    void destroy(rhi::Device& device);

    // Pump finished bakes (upload) + kick a new one when sun/focus moved.
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& focus, const Vec3& sunDirection);

    // {centerX, centerZ, 1/span, 0} — the scene owns .w (the strength).
    Vec4 info() const {
        return { center.x, center.y, 1.0f / kSpan, 0.0f };
    }
    rhi::BindGroupHandle bindGroup() const { return group; }
    bool ready() const { return mailbox.ready(); }

private:
    struct Baked {
        vector<u8> pixels; // RGBA8, R = sun visibility, G = sky openness
        Vec2 center {};
        Vec3 sun {};
        u64 gen { 0 };
    };

    BakeMailbox<Baked> mailbox;
    rhi::TextureHandle texture {};
    rhi::SamplerHandle sampler {};
    rhi::BindGroupHandle group {};
    Vec2 center {};
    Vec3 bakedSun { 0.0f, 1.0f, 0.0f };
};

} // namespace render
