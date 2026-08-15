#include "engine/render/landscape/TerrainLightMap.hpp"

#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

// One texel of the bake: sun visibility + sky openness at (x, z).
// Pure CPU over the deterministic height function (patches included) —
// worker-safe by construction.
void bakeTexel(const TerrainParams& params, f32 x, f32 z, const Vec3& sun,
               u8& outSun, u8& outSky) {
    const f32 h0 = terrain::height(params, x, z) + 1.2f; // eye-ish height

    // R — sun visibility: march toward the sun; blocked when the terrain
    // rises above the ray. Soft edge: accumulate how far above.
    f32 sunVis = 1.0f;
    if (sun.y > 0.01f) {
        const f32 horiz =
            std::sqrt(glm::max(sun.x * sun.x + sun.z * sun.z, 1e-6f));
        const f32 slopeUp = sun.y / horiz; // rise per horizontal meter
        f32 t = 10.0f;
        for (u32 i = 0; i < 24; ++i) {
            const f32 rise = terrain::height(params, x + sun.x / horiz * t,
                                             z + sun.z / horiz * t) -
                             (h0 + slopeUp * t);
            if (rise > 0.0f) {
                sunVis = 0.0f;
                break;
            }
            t *= 1.22f; // ~10 m -> ~1 km, geometric
        }
    }

    // G — sky openness: 8 azimuth horizon angles, ~200 m reach.
    f32 openness = 0.0f;
    for (u32 a = 0; a < 8; ++a) {
        const f32 angle = static_cast<f32>(a) * (6.2831853f / 8.0f);
        const f32 dx = std::cos(angle);
        const f32 dz = std::sin(angle);
        f32 maxSlope = 0.0f;
        f32 t = 8.0f;
        for (u32 i = 0; i < 8; ++i) {
            const f32 rise =
                terrain::height(params, x + dx * t, z + dz * t) - h0;
            maxSlope = glm::max(maxSlope, rise / t);
            t *= 1.8f;
        }
        // Slope -> fraction of the azimuth's sky band left visible.
        openness += 1.0f - glm::clamp(maxSlope / (1.0f + maxSlope), 0.0f,
                                      1.0f);
    }
    openness /= 8.0f;

    outSun = static_cast<u8>(glm::clamp(sunVis, 0.0f, 1.0f) * 255.0f);
    outSky = static_cast<u8>(glm::clamp(openness, 0.0f, 1.0f) * 255.0f);
}

} // namespace

void TerrainLightMap::create(rhi::Device& device, core::JobSystem& jobSystem) {
    mailbox.create(jobSystem);
    sampler = device.createSampler({});
    // The texture is (re)created per landed bake — the RHI has no
    // texture update, and a rebuild every ~8 s costs nothing.
    (void)device;
}

void TerrainLightMap::destroy(rhi::Device& device) {
    mailbox.reset(); // orphan in-flight bakes
    device.destroyBindGroup(group);
    device.destroySampler(sampler);
    device.destroyTexture(texture);
    group = {};
    sampler = {};
    texture = {};
}

void TerrainLightMap::update(rhi::Device& device, const TerrainParams& params,
                             const Vec3& focus, const Vec3& sunDirection) {
    // 1. Land a finished bake (fresh texture + group — no RHI texture
    // update; a rebuild every sun step is negligible).
    mailbox.drain([&](Baked& done) {
        if (texture.id != 0) {
            device.destroyBindGroup(group);
            device.destroyTexture(texture);
        }
        texture = device.createTexture(
            { .width = kSize,
              .height = kSize,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            done.pixels.data());
        group = device.createBindGroup(
            { .entries = { { .binding = 7,
                             .texture = texture,
                             .sampler = sampler } } });
        center = done.center;
        bakedSun = done.sun;
    });
    if (mailbox.busy()) {
        return;
    }

    // 2. Kick a re-bake when the (already-quantized) sun stepped or the
    // focus left the inner half of the map.
    const Vec2 want { focus.x, focus.z };
    const bool sunMoved = glm::dot(bakedSun, sunDirection) < 0.99995f;
    const bool strayed =
        !mailbox.ready() || glm::distance(want, center) > kSpan * 0.25f;
    if (!sunMoved && !strayed) {
        return;
    }
    mailbox.kick([params, want, sun = sunDirection](Baked& baked) {
        baked.center = want;
        baked.sun = sun;
        baked.pixels.resize(static_cast<size_t>(kSize) * kSize * 4, 255);
        const f32 texel = kSpan / static_cast<f32>(kSize);
        const f32 originX = want.x - kSpan * 0.5f;
        const f32 originZ = want.y - kSpan * 0.5f;
        for (u32 row = 0; row < kSize; ++row) {
            for (u32 col = 0; col < kSize; ++col) {
                u8* px = &baked.pixels[(static_cast<size_t>(row) * kSize +
                                        col) *
                                       4];
                bakeTexel(params,
                          originX + (static_cast<f32>(col) + 0.5f) * texel,
                          originZ + (static_cast<f32>(row) + 0.5f) * texel,
                          sun, px[0], px[1]);
                px[2] = 0;
                px[3] = 255;
            }
        }
    });
}

} // namespace render
