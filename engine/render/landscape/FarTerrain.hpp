#pragma once

#include "engine/core/Clock.hpp"
#include "engine/core/Defines.hpp"
#include "engine/assets/MeshData.hpp"
#include "engine/render/landscape/HeightField.hpp"
#include "engine/render/landscape/BakeMailbox.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"
#include "engine/render/ShaderLibrary.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Distant landscape silhouettes (docs/RENDERING.md §3.6): ONE coarse
// worker-baked grid around the camera (span per Config: 18 km inner
// ring, 57.6 km horizon ring), drawn UNDER the
// streamed near terrain — the same height function sampled at ~62 m
// cells, painted through the REAL weight rule with the splat layers'
// mean albedos (the horizon matches the ground), and raised +
// darkened by the shared forestMask so a canopy fringe silhouettes the
// far ridges. The mesh sinks a few meters inside the streaming ring
// (vertex shader) so its coarse sampling never pokes through the exact
// near chunks; applyFog's horizon closure moves out to reach().
class FarTerrain {
public:
    // Tree impostors: cylindrical-billboard silhouettes scattered by the
    // SAME forestMask/gates as the real trees, fading in where those end
    // (~880 m) and out into the veil.
    static constexpr f32 kTreeNear = 700.0f;
    // Crowns ~0.95x the height meet at this spacing — the canopy reads
    // as a continuous mass with emergent crowns, not isolated lollipops.
    static constexpr f32 kTreeSpacing = 20.0f;

    // One FarTerrain = one camera-centered ring. The defaults are the
    // historical inner ring; the HORIZON outer ring runs the same code
    // at a wider span (see WorldRenderer::farHorizon).
    struct Config {
        f32 span { 18000.0f };  // meters covered (~70 m cells at 256)
        u32 gridN { 256 };      // cells per side
        // Impostor reach (0 = none). far_tree.vert hard-fades at
        // 4200..5200 to match — change both together.
        f32 treeFar { 5200.0f };
        // Constant extra lowering baked into the vertices and NEVER
        // restored: the outer ring rides this far under the inner one
        // so the two never z-fight (invisible at its distances).
        f32 extraSink { 0.0f };
        f32 minTexel { 16.0f }; // heightCoarse footprint for the bake
        const char* probeName { "farTerrain" }; // JobProbe row
    };

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobs, const Config& cfg);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Pump finished bakes + kick a rebake when the camera strays past
    // an eighth of the span or the terrain inputs change.
    // `trees` = the measured real-tree silhouette (VegetationSystem)
    // — impostor size/shape and the canopy raise derive from it.
    // `layerAlbedos` = the semantic layer means
    // (TerrainSystem::layerAlbedoBase) — the far vertices are painted
    // through the REAL weight rule with them, so the horizon matches
    // the streamed ground materials.
    // `field` (nullable): the shared height pyramid — the half-cell
    // grid samples it (L1 where it covers, L2 beyond) instead of the
    // pointwise function; the tree impostors read the half-grid either
    // way. Null = exact path.
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& focus,
                const VegetationSystem::TreeSilhouette& trees,
                const array<Vec3, 5>& layerAlbedos,
                sptr<const HeightField::Snapshot> field = nullptr);

    // Draw in the main opaque pass, BEFORE the near terrain (depth does
    // the layering). `cloudMapGroup` at its own slot (the Vulkan rule).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle cloudMapGroup);

    // Impostor grounding: true = bilinear height/slope from the bake's
    // own half-grid (35 m — sub-texel shifts at >1 km); false = the
    // exact analytic function (5 height() per surviving candidate).
    // Synced from RenderTuning before update(); a change re-bakes via
    // the tree-height digest on the next content/stray trigger.
    bool impostorsFromGrid { true };

    bool ready() const { return mailbox.ready(); }
    // The horizon-closure distance this mesh supports (conservative:
    // half-span minus the rebake stray).
    f32 reach() const { return config.span * 0.42f; }

    struct TreeInstance {
        Vec4 positionScale; // xyz = ground point, w = tree height (m)
        Vec4 params;        // x = crown seed, y = tint jitter, zw free
    };

private:
    struct Baked {
        vector<MeshVertex> vertices;
        vector<TreeInstance> trees;
        u32 seed { 0 };
        f32 seaLevel { 0.0f };
        u64 contentStamp { 0 };
        u64 gen { 0 };
    };

    Config config;
    BakeMailbox<Baked> mailbox;
    rhi::UniqueBuffer vertexBuffer;
    rhi::UniqueBuffer indexBuffer;
    rhi::UniqueBuffer treeBuffer;
    rhi::UniquePipeline pipeline;
    rhi::UniquePipeline treePipeline;
    ShaderLibrary::Watch shaderWatch;
    u32 indexCount { 0 };
    u32 treeCount { 0 };
    Vec2 center {};
    u32 bakedSeed { 0 };
    u64 bakedContentStamp { 0 };
    f32 bakedTreeHeight { 0.0f };
    f32 bakedSeaLevel { 0.0f };
    bool bakedFromGrid { true };
    // Content-stamp coalescing: a publish burst bumps the stamp once
    // per tile — this 18 km bake waits for a second of stamp quiet and
    // fires ONCE per burst (stray/seed/sea stay immediate).
    u64 lastSeenContentStamp { 0 };
    core::TimePoint contentQuietSince {};
};

} // namespace render
