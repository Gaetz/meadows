#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Tileable 3D Perlin-Worley noise (the Hillaire TileableVolumeNoise
// channels), baked ONCE in compute at first frame and shared by every
// raymarched medium: ground-mist erosion and the volumetric sky clouds
// (docs/RENDERING.md §8). R = Perlin-Worley base shape (the sky), G = mid-frequency Worley FBM (the mist erosion), B =
// high-frequency Worley FBM (near detail), A unused. Gated on
// compute + volume caps; without them consumers keep their analytic
// fallback (volumetric_media.glsl fbm3).
class NoiseVolume {
public:
    static constexpr u32 kSize = 128; // 128³ RGBA8 = 8 MB

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);

    // Records the one-shot bake dispatch on first call (needs a command
    // buffer, hence not done in create()).
    void bakeIfNeeded(rhi::CommandBuffer& cmd);

    // Sampler group (binding 9, repeat wrap — the noise tiles).
    rhi::BindGroupHandle bindGroup() const { return sampleGroup.get(); }
    bool ready() const { return baked; }

private:
    rhi::UniqueTexture texture;
    rhi::UniqueSampler sampler;
    rhi::UniqueBindGroup sampleGroup;
    rhi::UniqueBindGroup bakeGroup;
    rhi::UniquePipeline bakePipeline;
    bool baked { false };
};

} // namespace render
