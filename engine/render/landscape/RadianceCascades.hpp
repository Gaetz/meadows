#pragma once

#include <memory>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/GpuProbe.hpp"
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

// Which indirect-lighting technique the surface shaders use (chantier RC,
// docs/RADIANCE-CASCADES.md). Classic = the current flat sky ambient ×
// terrain light map; RadianceCascades = the GI volume (gi.glsl, G6) with
// Classic as its far-field fallback. Runtime-switchable from the render
// panel — the parallel-technique seam the dev asked for (modding later).
enum class GiTechnique : u32 {
    Classic,
    RadianceCascades,
};

// Every cost-affecting parameter lives HERE and in the UI (dev workflow:
// quality first, HE does the perf descent with these knobs — never
// constants). Structural fields recreate the volumes when applied values
// change (the reflectionScale pattern).
struct RcTuning {
    // Structural (recreate):
    i32 resolution { 64 };    // voxels/probes per axis, clips + cascade 0
    f32 fineVoxel { 0.5f };   // meters — interiors / near detail
    f32 coarseVoxel { 2.0f }; // meters — mid-field (span = res × voxel)
    i32 cascadeCount { 5 };   // levels (clamped so the top keeps ≥2 probes)
    // Live:
    GiTechnique technique { GiTechnique::Classic }; // apply switch (G6)
    f32 intensity { 0.7f };   // indirect strength at apply (dev pick
                              // 2026-07-11, post fixed-step ramp)
    f32 skyFactor { 0.6f };   // sky ambient folded into injected surfaces
                              // (dev pick 2026-07-11, post fixed-step
                              // ramp — bounce 0.5 returns sky too)
    f32 interval0 { 1.0f };   // cascade-0 interval length (m); reach =
                              // interval0 × (2^count − 1)
    f32 edgeFade { 8.0f };    // meters of blend back to Classic at the
                              // grid border (G6)
    // FIXED log-step stylized ramp (dev decision 2026-07-11, replacing
    // the adaptive chain — bands must be PREDICTABLE): the GI luminance
    // snaps to absolute exposure steps, day/night/torch alike.
    f32 bandStops { 0.85f };  // stops per band (bigger = fewer, bolder)
    f32 bandAa { 0.3f };      // anti-aliasing width across a band edge
                              // (dev pick 2026-07-11)
    // G7c — interval extension (§2.3.3 du papier): levels whose full
    // march is >= 8 steps raymarch a QUARTER of their interval and
    // double it twice by shift+merge (x4 reach per marched step).
    // STRUCTURAL (recreates the scratch ping-pong textures); an
    // approximation — A/B it on the F6 rcBuild line.
    bool intervalExtension { false };
    f32 emitterBoost { 1.0f };  // radiance of the light-source blobs in
                                // the field (0 = surfaces-only, the old
                                // behavior)
    // G7a — multi-bounce: surfaces also receive LAST frame's merged GI
    // (one extra bounce per frame, converges geometrically; adds ~a
    // frame of light latency per bounce). 0 = single bounce.
    f32 bounceFeedback { 0.5f };
    // G7b — the penumbra experiment: local lights removed from the
    // DIRECT path (locallights.glsl sees none), living only in the GI —
    // their shadows/penumbras come from the cascades, voxel-resolution.
    bool rcOnlyLights { false };
    i32 updateInterval { 1 }; // inject every N frames (1 = every frame)
    i32 debugView { 0 };      // 0 off, 1 fine clip, 2 coarse clip,
                              // 3 merged cascade-0 irradiance
};

// One world-space occluder/emitter BOX for the injection (G3): props,
// kit modules, NPCs — built by the renderer from the snapshot (the
// engine never sees the game's caches). v1 boxes occlude as their AABB
// (assumed stylized; real triangles are a later brick).
struct RcBox {
    Vec4 boundsMin;      // xyz; w = opacity (1 = solid, <1 = the interval
                         //     march filters through — tree canopies)
    Vec4 boundsMax;      // xyz; w unused
    Vec4 albedoEmissive; // rgb = albedo proxy, a = emissive multiplier
};

// A local light splatted into the volume (G3): torches, lamps, spells.
struct RcLight {
    Vec4 positionRadius; // xyz = world position, w = radius (m)
    Vec4 color;          // rgb premultiplied by intensity; w unused
};

// Radiance cascades GI — G2 scope: the camera-centered voxel CLIPMAP
// (2 levels) fed by an analytic terrain height/albedo tile (worker-baked,
// the TerrainLightMap pattern — patch/sculpt-aware since terrain::height
// runs on the CPU) + per-voxel sun via the CSM, re-injected in compute
// every frame (single-shot: fully dynamic, zero invalidation). Cascades
// (G4), merge (G5) and the gi.glsl apply (G6) build on these volumes.
class RadianceCascades {
public:
    static constexpr u32 kTileSize = 256; // height/albedo bake tile (texels)
    static constexpr u32 kMaxBoxes = 256; // per-frame injected AABBs (G3)
    static constexpr u32 kMaxLights = 16; // matches the scene's lights UBO

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobs);
    void destroy(rhi::Device& device);
    void refreshPipelines(rhi::Device& device, ShaderLibrary& shaders);

    // Call BEFORE composing the frame UBO: decides whether this frame
    // re-injects (updateInterval) and, if so, snaps the clip origins the
    // frame will use — giGridInfo() then matches the volume content the
    // apply samples (dev bug 2026-07-11: composing with LAST frame's
    // origin displaced light opposite to the camera motion for a frame).
    void prepare(const Vec3& cameraPos);

    // Main thread, once per frame, OUTSIDE any render pass (compute):
    // pumps the tile bake, recreates volumes on knob changes, uploads the
    // RC UBO and dispatches the injection. Requires the frame UBO already
    // uploaded and the CSM rendered (the inject samples uShadowMap).
    // `terrainLightGroup` may be null (far-sun falls back to the CSM only).
    // `bakeTerrain` = false (interiors) keeps the "no terrain" placeholder
    // tile — the kit boxes and lights carry the room.
    void update(rhi::Device& device, rhi::CommandBuffer& cmd,
                const TerrainParams& params, const Vec3& cameraPos,
                const vector<RcBox>& boxes, const vector<RcLight>& lights,
                bool bakeTerrain, rhi::BindGroupHandle frameBindGroup,
                rhi::BindGroupHandle shadowBindGroup,
                rhi::BindGroupHandle terrainLightGroup,
                rhi::Device* probeDevice = nullptr,
                GpuProbe* probe = nullptr);

    // Fullscreen raymarch of the selected clip volume (tuning.debugView),
    // recorded INSIDE the composite pass, after the tonemap draw.
    void drawDebug(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle frameBindGroup);

    bool ready() const { return tileUploaded && clipFine.id() != 0; }

    // --- G6 apply: what the surface shaders consume ----------------------
    // The merged cascade-0 sampler (binding 11), bound once per main pass.
    rhi::BindGroupHandle applyGroup() const { return applyGroup_; }
    // uGiInfo — technique forced to Classic until the volumes are live.
    Vec4 giInfo() const {
        const bool active = tuning.technique == GiTechnique::RadianceCascades
                            && ready() && !levels.empty();
        return { active ? 1.0f : 0.0f, tuning.intensity, tuning.edgeFade,
                 static_cast<f32>(appliedResolution) };
    }
    // uGiGridInfo — the cascade-0 grid this frame (origin snaps in update).
    Vec4 giGridInfo() const { return { lastFineOrigin, appliedFineVoxel }; }

    RcTuning tuning;

private:
    void createVolumes(rhi::Device& device);
    void makePlaceholderTile(rhi::Device& device); // "no terrain" (interiors)
    void pumpTileBake(rhi::Device& device, const TerrainParams& params,
                      const Vec3& cameraPos);

    struct BakedTile {
        vector<f32> height; // kTileSize², meters
        vector<u8> albedo;  // kTileSize², RGBA8 (a unused)
        Vec2 center {};
        u64 gen { 0 };
    };

    core::JobSystem* jobs { nullptr };
    std::shared_ptr<core::ConcurrentQueue<BakedTile>> baked;
    rhi::UniqueTexture heightTex;  // R32F — GPU-side terrain height
    rhi::UniqueTexture albedoTex;  // RGBA8 — material proxy colors
    rhi::UniqueSampler tileSampler;
    rhi::UniqueSampler volumeSampler;
    Vec2 tileCenter {};
    f32 tileSpan { 0.0f };
    bool tileInFlight { false };
    bool tileUploaded { false };
    bool tileIsPlaceholder { true }; // "no terrain" tile (interiors/boot)
    u64 tileGeneration { 0 };

    rhi::UniqueTexture clipFine;   // res³ RGBA16F: rgb radiance, a occupancy
    rhi::UniqueTexture clipCoarse;
    rhi::UniqueBuffer rcUbo;
    rhi::UniqueBuffer boxBuffer;   // G3: SSBO of RcBox, kMaxBoxes
    rhi::UniqueBindGroup injectGroup;
    rhi::UniqueBindGroup debugGroup;
    rhi::UniquePipeline injectPipeline;
    u64 injectGeneration { 0 };
    rhi::UniquePipeline debugPipeline;
    u64 debugGeneration { 0 };
    rhi::UniquePipeline buildPipeline;  // G4
    u64 buildGeneration { 0 };
    rhi::UniquePipeline mergePipeline;  // G5
    u64 mergeGeneration { 0 };
    rhi::UniquePipeline extendPipeline; // G7c
    u64 extendGeneration { 0 };

    // G4/G5: one texture + one build group per cascade; merge groups pair
    // level i (image) with level i+1 (sampled src). Level layouts per
    // docs/RADIANCE-CASCADES.md §2.2 (c0 dir-major, c1+ dir-tiled).
    struct CascadeLevel {
        rhi::UniqueTexture texture;
        rhi::UniqueBindGroup buildGroup;
        rhi::UniqueBindGroup mergeGroup;
        u32 probes { 0 };  // per axis
        u32 dirsW { 0 };   // octahedral direction grid
        u32 dirsH { 0 };
        u32 width { 0 };   // texture dims
        u32 height { 0 };
        u32 depth { 0 };
        // G7c: extension ping-pong (texture -> scratch -> texture).
        bool extended { false }; // full march >= 8 steps at these knobs
        rhi::UniqueTexture scratch;
        rhi::UniqueBindGroup extendToScratch;
        rhi::UniqueBindGroup extendToTexture;
    };
    vector<CascadeLevel> levels;
    i32 appliedCascadeCount { 0 };
    bool appliedExtension { false };
    rhi::UniqueBindGroup applyGroup_; // G6: cascade 0 at binding 11
    Vec3 lastFineOrigin { 0.0f };     // this frame's grid origin (prepare)
    Vec3 lastCoarseOrigin { 0.0f };
    bool injectThisFrame { true };    // prepare()'s updateInterval verdict
    // G7a: where LAST inject's merged cascade 0 lives (bounce feedback);
    // false until one full inject+merge exists (fresh textures = garbage).
    Vec3 prevFineOrigin { 0.0f };
    f32 prevFineSpacing { 0.0f };
    bool havePrev { false };

    // Applied structural knobs (recreate when the tuning diverges).
    i32 appliedResolution { 0 };
    f32 appliedFineVoxel { 0.0f };
    f32 appliedCoarseVoxel { 0.0f };
    u32 frameCounter { 0 };
};

} // namespace render
