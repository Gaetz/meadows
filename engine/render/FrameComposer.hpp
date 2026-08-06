#pragma once

#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"
#include "engine/render/landscape/SkySystem.hpp"
#include "engine/render/AtmosphereParams.hpp"

// The per-frame UBO assembly, extracted from LandscapeScene::render().
// Pure values in, FrameUniforms out — no World, no device — so the
// ~110-line composition is unit-testable headless and the
// WorldRenderer consumes it unchanged. The scene still owns what
// is genuinely frame STATE: the shadow-sun hysteresis, cascade fitting, GPU
// availability checks (they feed `shadowStrength` / `reflectionsActive`), and
// every updateBuffer.

namespace render {

// Everything render() feeds into the frame UBO, as plain values.
struct FrameComposerInputs {
    // View.
    Mat4 viewProj { 1.0f };
    Vec3 cameraPosition { 0.0f };
    u32 width { 1 };
    u32 height { 1 };
    f32 dt { 0.0f };
    f32 timeSeconds { 0.0f };

    // Sky + shadows, evaluated by the caller (SkySystem state, cascade math,
    // GPU-availability gates).
    render::SkySystem::SkyState sky {};
    render::ShadowMapper::Cascades cascades {};
    f32 shadowStrength { 0.0f };

    // Atmosphere + interior reshape.
    AtmosphereParams atmos {};
    bool interiorMode { false };
    Vec3 interiorAmbient { 0.0f };
    // H1 (docs/RENDERING.md): how much the interior ambient follows the
    // OUTSIDE (sun elevation x the weather's ambientIntensity) — the
    // Helios model. 0 = the old constant; 1 = fully coupled.
    f32 interiorDaylightWeight { 0.6f };
    // V4/H4: froxel fog active (reach + interior dust routing).
    bool froxelFog { false };
    f32 interiorDustDensity { 0.025f };

    // Terrain / tuning scalars.
    f32 seaLevel { 0.0f };
    f32 snowLine { 0.0f };
    f32 splatUvScale { 0.0f };
    f32 splatBlendDepth { 0.15f }; // height-blend band (0 = plain blend)
    f32 terrainTintStrength { 0.3f }; // macro tint (0 = off)
    f32 splatDetailFade { 24.0f }; // detail-normal fade end (m)
    f32 pomDistance { 12.0f }; // parallax occlusion reach (m, 0 = off)
    f32 splatVariety { 0.5f }; // anti-repetition second tap (0 = off)
    f32 pomShadowStrength { 0.6f }; // POM self-shadow (0 = off)
    f32 pomDepth { 0.03f }; // parallax relief depth (uv units)
    bool barkEnabled { false }; // tree bark textures resident
    f32 ssaoStrength { 0.85f }; // ssao.frag lanes
    f32 ssaoRadius { 0.7f };    // world radius (m)
    f32 ssdmAmplitude { 0.0f };    // ssdm warp amplitude (m, 0 = off)
    f32 shadowFarUvScale { 1.0f }; // far-cascade viewport scale (shadow.glsl)
    bool reflectionsActive { false };
    // Horizon-closure distance (m) for applyFog — the far-terrain
    // reach when it stands in, else the streaming ring (0 = off).
    f32 drawDistance { 0.0f };
    // The streaming ring itself (m) — the far mesh's sink bias.
    f32 nearRingDistance { 0.0f };
    // Real-tree fade-out distance — the far impostors fade IN against
    // it (uFogLayerInfo.w) so the handoff tracks the vegetation ring.
    f32 treeFadeEnd { 880.0f };
    // Seasons: autumn tint blend and leaf fall (0..1 each), applied per
    // leaf-mask atlas slot through `leafSeason` (rgb tint, a =
    // seasonality — VegetationSystem::leafSeason()).
    f32 seasonAutumn { 0.0f };
    f32 seasonLeafFall { 0.0f };
    array<Vec4, 8> leafSeason {};

    // Dev toggles (the render panel's A/B state).
    i32 debugBuffer { 0 };
    bool stylized { false };
    bool tonemap { true };
    f32 exposure { 1.0f };
    bool cascadeDebug { false };
    bool grading { false };
    f32 gradeVibrance { 0.0f };
    f32 gradeSplitTone { 0.0f };
    f32 gradeContrast { 1.0f };
    bool autoExposure { false };
    f32 autoExposureMin { 0.0f };
    f32 autoExposureMax { 0.0f };

    // Aux maps, already resolved by their owning systems.
    Vec4 waterMapInfo { 0.0f };
    Vec4 terrainLightInfo { 0.0f }; // xyz from the bake; w rewritten below
    bool terrainLightActive { false };

    // Submersion, wind, grass bend.
    f32 waterSurfaceY { -1.0e6f }; // effective surface above the camera
    f32 windTime { 0.0f };
    bool grassBend { false };  // Play mode with a live body
    Vec3 playerFeet { 0.0f };

    // The meadow grass tuning, pre-packed by the renderer from
    // GrassRenderTuning (see FrameUniforms for the lane meanings).
    Vec4 grassShapeInfo { 0.95f, 0.045f, 12.5f, 25.0f };
    Vec4 grassLodInfo { 10.0f, 70.0f, 0.20f, 1.7f };
    Vec4 grassBaseTint { 1.0f, 1.0f, 1.0f, 140.0f };
    Vec4 grassTipTint { 1.0f, 1.0f, 1.0f, 190.0f };
    Vec4 grassShadeInfo { 1.0f, 0.5f, 0.0f, 0.0f };
    Vec4 grassBladeInfo { 1.0f, 1.0f, 0.0f, 0.0f };
    // Foliage-card leaf cutout -> solid mip window (Tree builder).
    Vec4 leafLodInfo { 4.0f, 7.0f, 0.0f, 0.0f };
    // Stylized ramp lanes (see FrameUniforms).
    Vec4 stylizedDiffuseInfo { 0.02f, 0.09f, 0.32f, 0.40f };
    Vec4 stylizedShadowInfo { 0.45f, 0.55f, 0.0f, 0.6f };
    Vec4 stylizedSpecInfo { 0.35f, 0.35f, 24.0f, 0.0f };

    // The GI switch, RESOLVED only — the reflection pass
    // (which copies `base`) keeps the Classic ambient, so it never needs
    // the cascade sampler bound. Defaults = Classic (x = 0).
    Vec4 giInfo {};
    Vec4 giGridInfo {};
    Vec4 giBandInfo { 0.0f, 0.3f, 0.7f, 0.0f }; // band count + AA + floor

    // Ground mist (mist.frag), RESOLVED only — no mist in the reflection
    // pass or indoors. Density/coverage ride `atmos` (weather-crossfaded);
    // these are the renderer-tuning knobs. atmos.mistDensity == 0 keeps
    // the pass off and its target neutral.
    bool mistActive { false };
    f32 mistCoverageSoftness { 0.6f };
    f32 mistReach { 1200.0f };
    Vec4 mistShapeInfo { 0.0035f, 0.02f, 0.2f, 49.0f };
    Vec4 mistMapInfo {};    // from MistMap::info()
    // x = NoiseVolume active, y = steps, z = dropout (m), w = sun gain.
    Vec4 mistDetailInfo { 0.0f, 16.0f, 400.0f, 10.0f };
    // x = forward HG lobe g, y = backscatter weight, z = ambient gain,
    // w = ambient floor in shadow.
    Vec4 mistLightInfo { 0.95f, 0.8f, 1.25f, 0.6f };
    // Volumetric sky clouds (RESOLVED only — the reflection pass keeps
    // the 2D dome layer): x = active, y = thickness, z = sigma,
    // w = erosion strength; light = gain/g/ambient.
    Vec4 cloudVolInfo { 0.0f, 440.0f, 0.065f, 0.31f };
    Vec4 cloudVolLightInfo { 19.9f, 0.3f, 0.9f, 30.2f };
    Vec4 cloudVolShapeInfo { 3.4f, 0.8f, 1.0f, 0.5f };
    Vec4 cloudVolRimInfo { 25.0f, 0.75f, 7.4f, 0.0f };
    Vec4 mistPuffInfo { 0.5f, 0.0f, 0.0f, 0.0f };
    // x = water debug view mode (0 off).
    Vec4 waterDebugInfo { 0.0f, 0.0f, 0.0f, 0.0f };
    // Water-info map: xy = center, z = 1/span, w = valid.
    Vec4 waterInfoMapInfo { 0.0f, 0.0f, 0.0f, 0.0f };
    // Region shading maps (TerrainShadeMap::info()).
    Vec4 terrainShadeMapInfo { 0.0f, 0.0f, 0.0f, 0.0f };
};

// The volumetric fog's reach (froxel far AND cluster grid far — the two
// grids share their z slices by construction, docs/RENDERING.md §5 B5, so
// this is the ONE place the formula lives). Exterior: scales with the
// weather's fog start so a far-fog look (high start) pushes the froxel
// band — and its cloud-shadow ray curtains — into the distance instead of
// spending ~90% of the slices on clear air.
inline f32 volumetricReach(bool interiorMode, f32 fogStart) {
    return interiorMode ? 48.0f
                        : glm::clamp(fogStart * 3.0f, 800.0f, 2400.0f);
}

// The two variants one frame needs. `base` is the raw exterior composition —
// the planar-reflection pass copies it (with its own view matrices) WITHOUT
// the interior/grade/storm patches, exactly as render() always did.
// `resolved` is what the frame UBO uploads (interior override, grading,
// submersion, terrain light, grass bend, storm/rain, auto-exposure applied).
struct ComposedFrame {
    render::FrameUniforms base;
    render::FrameUniforms resolved;
};

ComposedFrame composeFrameUniforms(const FrameComposerInputs& in);

} // namespace render
