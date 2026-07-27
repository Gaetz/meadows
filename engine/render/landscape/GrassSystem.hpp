#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/render/landscape/ChunkStreamer.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Grass tuning, exposed in the render
// panel's "Grass" category. The RENDER half is live — uploaded into the
// FrameUbo every frame (uGrassShapeInfo/uGrassLodInfo/uGrass*Color) and
// read by GrassSystem::draw()'s density prefix, which MUST stay on the
// same curve as grass.vert's clip.
struct GrassRenderTuning {
    f32 bladeHeight { 0.95f };    // meters at scale 1
    f32 bladeHalfWidth { 0.03f }; // hand-tuned
    f32 detailNear { 12.5f };     // full quick-grass shading inside...
    f32 detailFar { 25.0f };      // ...flattened LOD beyond
    f32 thinStart { 10.0f };      // density LOD: thinning begins (m)
    f32 thinEnd { 70.0f };        // ...bottoms out at farDensity (m)
    f32 farDensity { 0.20f };     // fraction of blades kept at thinEnd
    f32 widthCompensation { 1.7f }; // far blades widen (same visual mass)
    f32 fadeStart { 140.0f };     // blades start sinking (m)
    f32 fadeEnd { 190.0f };       // gone — must match draw()'s cull (m)
    // Tints MULTIPLY the ground albedo baked at each blade's root
    // (one color source with the terrain splat — the BotW raccord);
    // (1,1,1) = the blade takes exactly the ground color. The defaults
    // are the FLAT-MASS look: the meadow is one solid ground-colored
    // block, blades read only through silhouettes, wind and the tip
    // sheen.
    Vec3 baseTint { 1.0f, 1.0f, 1.0f };
    Vec3 tipTint { 1.0f, 1.0f, 1.0f };
    // Shading knobs (uGrassShadeInfo/uGrassBladeInfo lanes).
    f32 rootAo { 1.0f };        // root-AO floor (1 = no root occlusion)
    f32 sheen { 0.5f };         // tip sheen strength (sun-facing spec)
    f32 bladeNormals { 0.0f };  // near blade-normal share (0 = blades
                                //   light exactly like the ground —
                                //   the flat-mass interior; 0.35 = the
                                //   historical rounded-blade shading)
    f32 brightMin { 1.0f };     // whole-blade brightness hash range
    f32 brightMax { 1.0f };
    f32 middleDarken { 0.0f };  // blade width-center darkening
    f32 backscatter { 0.0f };   // sun bleeding through the blade
};

// The SCATTER half is baked per chunk on the workers — the panel triggers
// a re-scatter (GrassSystem::regenerate) when one of these moves.
struct GrassScatterTuning {
    f32 spacing { 0.15f };          // meters between candidates (in-patch)
    f32 patchBroadScale { 21.0f };  // broad patch-mask noise period (m)
    f32 patchDetailScale { 6.0f };  // clump detail noise period (m)
    f32 patchThresholdLo { 0.47f }; // mask smoothstep window: below = bare
    f32 patchThresholdHi { 0.60f }; // ...above = heart of a patch
    f32 presenceLo { 0.08f };       // patch -> presence window (rim...)
    f32 presenceHi { 0.40f };       // ...to solid-volume interior
    f32 materialCutoff { 0.72f };   // min grass splat weight to grow
    // Splat tiling (tiles/meter) for the root-albedo bake — NOT a panel
    // knob: WorldRenderer syncs it from the view every frame (a change
    // regenerates, same policy as the other scatter knobs).
    f32 splatUvScale { 0.25f };
};

// Animated grass (blade model = the
// SimonDev Quick_Grass port, see grass.vert). A chunk ring around the
// camera scatters grass blades on worker threads — deterministic (seeded
// hash per chunk), so revisited meadows are identical — and draws each
// blade as REAL GEOMETRY: an instanced 6-segment strip, curved, tapered
// and wind-blown in the vertex shader, no alpha test at all (crisp
// silhouettes, no fizzle). Density follows the terrain material: blades
// grow only where the splat weights say grass. Same streaming skeleton
// as TerrainSystem (queue, budget, eviction, generation).
class GrassSystem {
public:
    static constexpr i32 kViewRadius = 3;  // chunks — fade hides the edge
    static constexpr i32 kEvictRadius = 4;
    static constexpr u32 kMaxUploadsPerFrame = 2;
    // Scatter jobs are budgeted like uploads (a border crossing used to
    // dump a whole ring edge of dense scatters on the workers at once —
    // part of the fast-travel stutter; see TerrainSystem).
    static constexpr u32 kMaxRequestsPerFrame = 2;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);

    // Streaming pump — main thread, once per frame (top of render).
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos);

    // Drops every chunk (terrain seed changed).
    void regenerate(rhi::Device& device);

    // Drops only these chunks (cx,cz keys) so update() re-scatters them onto
    // the current terrain — the sculpt path. Non-resident chunks are left to
    // finish streaming. Keys share the terrain chunk grid (keyOf).
    void invalidateChunks(rhi::Device& device, const vector<u64>& keys);

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Records instanced blade draws into the current render pass (opaque —
    // draw with the other opaques, before the sky). Grass receives shadows
    // (shadowBindGroup) but does not cast them. cameraPos drives the
    // metric density LOD: instances are sorted by the keep key grass.vert
    // clips against, so each chunk draws exactly the prefix the shader
    // keeps at its nearest distance (fewer, wider blades far away);
    // chunks are emitted front-to-back for early-z.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup, const Vec3& cameraPos,
              const Frustum* frustum = nullptr);

    u32 instanceTotal() const { return instances; }
    // CPU-side geometry counters for the frame's culled+thinned draw
    // (mid-pass GPU timestamps are meaningless on Metal).
    u32 indicesThisFrame() const { return frameIndices; }
    u32 bladesThisFrame() const { return frameBlades; }

    // The render panel's "Grass" category writes both; renderTuning is
    // live (FrameUbo + draw()'s prefix), scatterTuning applies on the
    // next regenerate()/re-scatter.
    GrassRenderTuning renderTuning;
    GrassScatterTuning scatterTuning;

    // One scattered blade. Layout mirrors the grass.vert instance attributes.
    struct Instance {
        Vec4 positionScale; // xyz = terrain point, w = height scale
        Vec4 params;        // x = yaw, y = flutter phase, z = tint jitter,
                            // w = lean amount
        // THE BotW look mechanism: blades shade with the GROUND normal
        // (the meadow lights as one continuous surface; blade geometry
        // only shows in silhouettes and wind) and inherit the GROUND
        // albedo at their root (one color source with the terrain splat).
        Vec4 groundNormal;  // xyz = terrain normal at the root,
                            // w = root ground albedo (packed sRGB bytes)
    };

private:
    struct Chunk {
        bool resident { false };
        rhi::UniqueBuffer instanceBuffer;
        u32 instanceCount { 0 };
        // Blade-root height range, for the frustum AABB.
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
    };

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // The shared ring mechanics (audit U3-1): map + generation-stamped
    // queue + budgeted request/evict live in ChunkStreamer.
    ChunkStreamer<Chunk, vector<Instance>> streamer;
    u32 instances { 0 };
    u32 frameIndices { 0 }; // reset in update(), summed by draw()
    u32 frameBlades { 0 };

    rhi::UniqueBuffer bladeVertexBuffer;
    rhi::UniqueBuffer bladeIndexBuffer;
    u32 bladeIndexCount { 0 };
    rhi::UniquePipeline pipeline;
    u64 shaderGeneration { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic:
// the same params, tuning and coords always produce the same tufts.
vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           const GrassScatterTuning& tuning,
                                           i32 cx, i32 cz);

} // namespace render
