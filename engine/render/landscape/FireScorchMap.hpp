#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// The fire field's render mask (chantier ESPRITS E3): one texel per 2 m
// cell of the fire window, 0 untouched .. 1 burnt, sampled by
// terrain.frag (charred albedo) and grass.frag (blades burn away) at
// unit 10 through uFireScorchInfo — the pool-map idiom: a fresh texture
// per landed fire job, a 1x1 zero placeholder while nothing burnt.
class FireScorchMap {
public:
    void create(rhi::Device& device);
    void destroy(rhi::Device& device);

    // A landed mask: n x n bytes over the window at `origin`, `texel`
    // apart (cell (col, row) centered on origin + (col, row) * texel).
    void upload(rhi::Device& device, const vector<u8>& scorch, u32 n,
                const Vec2& origin, f32 texel);
    // Back to the placeholder (map swap): nothing burnt anywhere.
    void clear(rhi::Device& device);

    // {originX, originZ, 1/texel, n} — n = 0 while cleared.
    Vec4 info() const {
        return { origin.x, origin.y, texel > 0.0f ? 1.0f / texel : 0.0f,
                 static_cast<f32>(cells) };
    }
    rhi::BindGroupHandle bindGroup() const { return group; }

private:
    void rebuildGroup(rhi::Device& device);

    rhi::TextureHandle texture {};
    rhi::SamplerHandle sampler {};
    rhi::BindGroupHandle group {};
    Vec2 origin {};
    f32 texel { 0.0f };
    u32 cells { 0 };
    vector<f32> staging; // R16F initial-data contract: packed f32 per texel
};

} // namespace render
