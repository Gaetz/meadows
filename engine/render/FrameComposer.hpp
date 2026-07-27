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
    bool reflectionsActive { false };

    // Dev toggles (the render panel's A/B state).
    i32 debugBuffer { 0 };
    bool stylized { true };
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

    // The GI switch, RESOLVED only — the reflection pass
    // (which copies `base`) keeps the Classic ambient, so it never needs
    // the cascade sampler bound. Defaults = Classic (x = 0).
    Vec4 giInfo {};
    Vec4 giGridInfo {};
    Vec4 giBandInfo { 0.0f, 0.3f, 0.7f, 0.0f }; // band count + AA + floor
};

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
