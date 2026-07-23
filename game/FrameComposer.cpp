#include "game/FrameComposer.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace game {

ComposedFrame composeFrameUniforms(const FrameComposerInputs& in) {
    // Sun position on screen for the god rays; shafts fade as the sun
    // leaves the frame or dips below the horizon.
    Vec2 sunUv { 0.5f, 0.5f };
    f32 shaftFade = 0.0f;
    {
        const Vec4 clip =
            in.viewProj *
            Vec4 { in.cameraPosition + in.sky.sunDirection * 1000.0f, 1.0f };
        if (clip.w > 0.0f) {
            const Vec2 ndc { clip.x / clip.w, clip.y / clip.w };
            sunUv = ndc * 0.5f + Vec2 { 0.5f };
            const f32 edge = glm::max(std::abs(ndc.x), std::abs(ndc.y));
            shaftFade = (1.0f - glm::smoothstep(0.85f, 1.35f, edge)) *
                        glm::smoothstep(-0.02f, 0.05f, in.sky.sunDirection.y);
        }
    }

    constexpr f32 kCloudCell =
        render::SkySystem::kCloudMapSpan / render::SkySystem::kCloudMapSize;
    const render::FrameUniforms base {
        .viewProj = in.viewProj,
        .invViewProj = glm::inverse(in.viewProj),
        .cameraPos = { in.cameraPosition, 1.0f },
        // (time.y is a retired slot kept neutral: UBO layout is
        // append-only.)
        .time = { in.timeSeconds, 0.0f, in.atmos.volumetric,
                  static_cast<f32>(in.debugBuffer) },
        .sunDirection = { in.sky.sunDirection, 0.0f },
        .sunColor = { in.sky.sunColor, in.sky.sunDiscIntensity },
        .sunGlowColor = { in.sky.glowColor, 0.0f },
        .ambientColor = { in.sky.ambientColor, in.stylized ? 1.0f : 0.0f },
        .zenithColor = { in.sky.zenithColor, 0.0f },
        .horizonColor = { in.sky.horizonColor, 0.0f },
        .horizonFarColor = { in.sky.horizonFarColor, 0.0f },
        .terrainInfo = { in.seaLevel, in.snowLine, in.splatUvScale,
                         in.reflectionsActive ? 1.0f : 0.0f },
        .postInfo = { in.tonemap ? 1.0f : 0.0f, in.exposure,
                      in.cascadeDebug ? 1.0f : 0.0f,
                      in.atmos.bloomIntensity },
        .fogInfo = { in.atmos.fogDensity, in.atmos.fogHeightFalloff,
                     in.atmos.fogLowBoost, in.atmos.fogStart },
        .sunViewProj = in.cascades.viewProj,
        // .w = interior flag: mesh/skinned/locallights switch to the
        // hemispheric ambient + wrap/bounce indoors; 0 keeps the exterior
        // byte-identical.
        .cascadeSplits = { in.cascades.splitFar[0], in.cascades.splitFar[1],
                           in.cascades.splitFar[2],
                           in.interiorMode ? 1.0f : 0.0f },
        .shadowInfo = { in.cascades.texelWorld[0], in.cascades.texelWorld[1],
                        in.cascades.texelWorld[2], in.shadowStrength },
        .screenInfo = { static_cast<f32>(in.width),
                        static_cast<f32>(in.height),
                        1.0f / static_cast<f32>(in.width),
                        1.0f / static_cast<f32>(in.height) },
        .cloudInfo = { in.atmos.cloudCoverage, in.atmos.cloudHeight,
                       in.atmos.cloudScale, in.atmos.cloudShadow },
        .sunScreen = { sunUv.x, sunUv.y, shaftFade,
                       in.atmos.godRayIntensity },
        .cloudMapInfo = { std::floor(in.cameraPosition.x / kCloudCell) *
                              kCloudCell,
                          std::floor(in.cameraPosition.z / kCloudCell) *
                              kCloudCell,
                          1.0f / render::SkySystem::kCloudMapSpan, 0.0f },
        .waterMapInfo = in.waterMapInfo,
        .windInfo = { in.windTime, in.atmos.windStrength, in.atmos.waveChop,
                      0.0f },
        // In BASE so the planar-reflection pass gets the same meadow
        // tuning as the main view.
        .grassShapeInfo = in.grassShapeInfo,
        .grassLodInfo = in.grassLodInfo,
        .grassBaseColor = in.grassBaseColor,
        .grassTipColor = in.grassTipColor,
        // BASE too: the reflection pass draws the tree cards.
        .leafLodInfo = in.leafLodInfo,
        // BASE too: the fog applies in the reflection as well.
        .fogSunInfo = { in.atmos.fogSunScatter, in.atmos.fogSunPhase, 0.0f,
                        0.0f },
        // BASE too: the reflection shades with the same ramp.
        .stylizedDiffuseInfo = in.stylizedDiffuseInfo,
        .stylizedShadowInfo = in.stylizedShadowInfo,
    };

    render::FrameUniforms resolved = base;
    if (in.interiorMode) {
        // Interior mode: no sun, no sky glow, dim constant ambient, no
        // fog, no god rays/volumetric — local lights carry the room.
        resolved.sunColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        resolved.sunGlowColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        resolved.ambientColor = { in.interiorAmbient, base.ambientColor.w };
        resolved.fogInfo = { 0.0f, 0.02f, 0.0f, 100000.0f };
        resolved.sunScreen = { 0.5f, 0.5f, 0.0f, 0.0f };
        resolved.time.z = 0.0f; // volumetric shafts off
    }
    // Grade parameters on free .w slots — AFTER the
    // interior override (which zeroes sunGlowColor), so the grade applies
    // in both modes. Neutral values when the A/B toggle is off.
    resolved.sunGlowColor.w = in.grading ? in.gradeVibrance : 0.0f;
    resolved.zenithColor.w = in.grading ? in.gradeSplitTone : 0.0f;
    resolved.horizonColor.w = in.grading ? in.gradeContrast : 1.0f;
    // The effective water surface above the camera (sea /
    // volume top / dry) — the tonemap submersion input.
    resolved.submersionInfo.x = in.waterSurfaceY;
    // The terrain light map info (w = strength, 0 until the first
    // bake lands or when toggled off / indoors).
    resolved.terrainLightInfo = in.terrainLightInfo;
    resolved.terrainLightInfo.w = in.terrainLightActive ? 1.0f : 0.0f;
    // The GI switch rides RESOLVED only (base = the
    // reflection pass stays Classic — no cascade sampler needed there).
    resolved.giInfo = in.giInfo;
    resolved.giGridInfo = in.giGridInfo;
    resolved.giBandInfo = in.giBandInfo;
    // The player's feet part the grass (off in Fly).
    resolved.grassBendInfo =
        in.grassBend ? Vec4 { in.playerFeet.x, in.playerFeet.z,
                              in.playerFeet.y, 0.85f }
                     : Vec4 { 0.0f };
    // The crossfaded storm front + rain intensity, and the top-down
    // rain-occlusion matrix (ortho, 40 m around the camera). .x carries
    // the raw storm front; rain, wetness and the occlusion matrix all
    // ride .y.
    resolved.stormInfo.x = in.atmos.stormFront;
    resolved.stormInfo.y = in.interiorMode ? 0.0f : in.atmos.rainIntensity;
    if (resolved.stormInfo.y > 0.003f) {
        const Vec3 eye = in.cameraPosition;
        const Mat4 rainView = glm::lookAt(eye + Vec3 { 0.0f, 60.0f, 0.0f },
                                          eye, Vec3 { 0.0f, 0.0f, 1.0f });
        const Mat4 rainProj =
            glm::ortho(-40.0f, 40.0f, -40.0f, 40.0f, 0.0f, 140.0f);
        resolved.rainOcclusionViewProj = rainProj * rainView;
    }
    // Auto-exposure parameters on free .w slots (adapt.frag
    // + the tonemap tap flag).
    resolved.sunDirection.w = in.dt;
    resolved.horizonFarColor.w = in.autoExposureMin;
    resolved.cloudMapInfo.w = in.autoExposureMax;
    resolved.windInfo.w = in.autoExposure ? 1.0f : 0.0f;

    return { base, resolved };
}

} // namespace game
