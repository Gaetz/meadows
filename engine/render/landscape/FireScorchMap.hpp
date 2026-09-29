#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// The look of burnt ground (docs/FIRE-RENDER.md F1): a bed of coals —
// dark coal bodies split by cracks that glow while the cell burns and
// as its embers cool, the coals turning to ash as they die; the char
// colour; the grass cull. Live knobs (render panel "Fire"); bed
// intensity 0 = the plain scorch look (the A/B toggle).
struct FireLook {
    f32 bedIntensity { 3.0f };  // HDR emissive scale of the cracks (0 = off)
    f32 pulse { 0.35f };        // the coals' breathing amplitude
    f32 erode { 0.3f };         // noise amplitude eroding the front
    f32 coalScale { 6.0f };     // metres per noise tile (coal size ~ /16)
    f32 ash { 0.7f };           // how grey cooled coals turn
    Vec3 emberHot { 1.0f, 0.72f, 0.35f };   // fresh flames
    Vec3 emberCold { 0.85f, 0.2f, 0.04f };  // dying embers
    Vec3 charColor { 0.05f, 0.04f, 0.035f };
    f32 grassCull { 0.9f };     // charred past this = no blade
    // The flipbook flames (fxflame.frag): HDR boost on the sheet's
    // colour, and an optional posterize (0 = the sheet as is, N = bands
    // of brightness, the stylized look).
    f32 flameBoost { 3.0f };
    f32 flamePosterize { 0.0f };
    // The heat haze (fxhaze.frag): screen-space refraction amplitude (a
    // fraction of the screen; 0 = off).
    f32 hazeStrength { 0.012f };
    // The fire's LIGHTS (docs/FIRE-RENDER.md F3): the front's cells
    // aggregated per 8 m tile into point lights for the clustered path
    // (and the GI, which takes the nearest). Intensity 0 = no lights.
    f32 lightIntensity { 0.7f };     // per burning cell in the tile
    f32 lightMaxIntensity { 8.0f };  // a tile's cap
    f32 lightRadius { 5.0f };        // + 2 m x sqrt(cells)
    f32 lightFlicker { 0.35f };
    i32 lightCount { 16 };           // nearest tiles that get a light
    Vec3 lightColor { 1.0f, 0.6f, 0.28f };
};

// The fire field's render mask (chantier ESPRITS E3): one texel per 2 m
// cell of the fire window — R = scorch (0 untouched .. 1 burnt), G =
// ember glow (fireGlow), B = the canopy burnt there (a tree's foliage
// gone, 0..1) — sampled by terrain, grass and tree shaders at unit 10
// through uFireScorchInfo (firemask.glsl) — the pool-map
// idiom: a fresh texture per landed fire job, a 1x1 zero placeholder
// while nothing burnt.
class FireScorchMap {
public:
    FireLook look;

    void create(rhi::Device& device);
    void destroy(rhi::Device& device);

    // A landed mask: n x n bytes each over the window at `origin`,
    // `texel` apart (cell (col, row) centered on origin + (col, row) *
    // texel). `glow` may be empty (no rim).
    void upload(rhi::Device& device, const vector<u8>& scorch,
                const vector<u8>& glow, const vector<u8>& canopy, u32 n,
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
    vector<u8> staging; // RGBA8 texels
};

} // namespace render
