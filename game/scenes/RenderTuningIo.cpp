#include "game/scenes/RenderTuningIo.hpp"

#include <glm/glm.hpp>

#include "engine/render/WorldRenderer.hpp"
#include "game/scenes/LandscapeTuning.hpp"

namespace game {

namespace {

// ONE field list per Form <-> engine-params pair, walked in BOTH
// directions (apply and capture): a field added to a table is
// automatically read AND written — a field forgotten in one of two
// hand-written lists is how nine conifer fields silently stopped saving.
// The member pointers are compile-checked against both structs.
template <typename Params, typename Form, typename T>
struct FieldLane {
    T Params::* param;
    T Form::* form;
};

template <typename Params, typename Form, typename T, size_t N>
void applyLanes(Params& params, const Form& form,
                const FieldLane<Params, Form, T> (&lanes)[N]) {
    for (const auto& lane : lanes) {
        params.*lane.param = form.*lane.form;
    }
}

template <typename Params, typename Form, typename T, size_t N>
void captureLanes(const Params& params, Form& form,
                  const FieldLane<Params, Form, T> (&lanes)[N]) {
    for (const auto& lane : lanes) {
        form.*lane.form = params.*lane.param;
    }
}

constexpr FieldLane<render::LobeTreeParams, data::LobeTreeTuningForm, f32> kLobeLanesF32[] = {
    { &render::LobeTreeParams::trunkHeightMin, &data::LobeTreeTuningForm::trunkHeightMin },
    { &render::LobeTreeParams::trunkHeightMax, &data::LobeTreeTuningForm::trunkHeightMax },
    { &render::LobeTreeParams::trunkRadiusMin, &data::LobeTreeTuningForm::trunkRadiusMin },
    { &render::LobeTreeParams::trunkRadiusMax, &data::LobeTreeTuningForm::trunkRadiusMax },
    { &render::LobeTreeParams::trunkTaper, &data::LobeTreeTuningForm::trunkTaper },
    { &render::LobeTreeParams::lean, &data::LobeTreeTuningForm::lean },
    { &render::LobeTreeParams::branchLengthMin, &data::LobeTreeTuningForm::branchLengthMin },
    { &render::LobeTreeParams::branchLengthMax, &data::LobeTreeTuningForm::branchLengthMax },
    { &render::LobeTreeParams::crownLobeRadiusMin, &data::LobeTreeTuningForm::crownLobeRadiusMin },
    { &render::LobeTreeParams::crownLobeRadiusMax, &data::LobeTreeTuningForm::crownLobeRadiusMax },
    { &render::LobeTreeParams::branchLobeRadiusMin, &data::LobeTreeTuningForm::branchLobeRadiusMin },
    { &render::LobeTreeParams::branchLobeRadiusMax, &data::LobeTreeTuningForm::branchLobeRadiusMax },
    { &render::LobeTreeParams::lobeFlatten, &data::LobeTreeTuningForm::lobeFlatten },
    { &render::LobeTreeParams::normalSpherize, &data::LobeTreeTuningForm::normalSpherize },
};
constexpr FieldLane<render::LobeTreeParams, data::LobeTreeTuningForm, i32> kLobeLanesI32[] = {
    { &render::LobeTreeParams::branchCountMin, &data::LobeTreeTuningForm::branchCountMin },
    { &render::LobeTreeParams::branchCountMax, &data::LobeTreeTuningForm::branchCountMax },
};
constexpr FieldLane<render::ColonizedTreeParams, data::ColonizedTreeTuningForm, f32> kColonizedLanesF32[] = {
    { &render::ColonizedTreeParams::flareAmount, &data::ColonizedTreeTuningForm::flareAmount },
    { &render::ColonizedTreeParams::flareHeight, &data::ColonizedTreeTuningForm::flareHeight },
    { &render::ColonizedTreeParams::curvePreserve, &data::ColonizedTreeTuningForm::curvePreserve },
    { &render::ColonizedTreeParams::pathJitter, &data::ColonizedTreeTuningForm::pathJitter },
    { &render::ColonizedTreeParams::ringIrregularity, &data::ColonizedTreeTuningForm::ringIrregularity },
    { &render::ColonizedTreeParams::sideMinFraction, &data::ColonizedTreeTuningForm::sideMinFraction },
    { &render::ColonizedTreeParams::segment, &data::ColonizedTreeTuningForm::segment },
    { &render::ColonizedTreeParams::killDistance, &data::ColonizedTreeTuningForm::killDistance },
    { &render::ColonizedTreeParams::pipeExponent, &data::ColonizedTreeTuningForm::pipeExponent },
    { &render::ColonizedTreeParams::tropism, &data::ColonizedTreeTuningForm::tropism },
    { &render::ColonizedTreeParams::trunkBaseMin, &data::ColonizedTreeTuningForm::trunkBaseMin },
    { &render::ColonizedTreeParams::trunkBaseMax, &data::ColonizedTreeTuningForm::trunkBaseMax },
    { &render::ColonizedTreeParams::crownHeightMin, &data::ColonizedTreeTuningForm::crownHeightMin },
    { &render::ColonizedTreeParams::crownHeightMax, &data::ColonizedTreeTuningForm::crownHeightMax },
    { &render::ColonizedTreeParams::crownRadiusMin, &data::ColonizedTreeTuningForm::crownRadiusMin },
    { &render::ColonizedTreeParams::crownRadiusMax, &data::ColonizedTreeTuningForm::crownRadiusMax },
    { &render::ColonizedTreeParams::crownTaper, &data::ColonizedTreeTuningForm::crownTaper },
    { &render::ColonizedTreeParams::leaderBias, &data::ColonizedTreeTuningForm::leaderBias },
    { &render::ColonizedTreeParams::lateralFlatten, &data::ColonizedTreeTuningForm::lateralFlatten },
    { &render::ColonizedTreeParams::sprayFoliage, &data::ColonizedTreeTuningForm::sprayFoliage },
    { &render::ColonizedTreeParams::tipBallRadius, &data::ColonizedTreeTuningForm::tipBallRadius },
    { &render::ColonizedTreeParams::tipOrderFalloff, &data::ColonizedTreeTuningForm::tipOrderFalloff },
    { &render::ColonizedTreeParams::tipBallMin, &data::ColonizedTreeTuningForm::tipBallMin },
    { &render::ColonizedTreeParams::seasonality, &data::ColonizedTreeTuningForm::seasonality },
    { &render::ColonizedTreeParams::smoothK, &data::ColonizedTreeTuningForm::smoothK },
    { &render::ColonizedTreeParams::cardHalfSizeMin, &data::ColonizedTreeTuningForm::cardHalfSizeMin },
    { &render::ColonizedTreeParams::cardHalfSizeMax, &data::ColonizedTreeTuningForm::cardHalfSizeMax },
    { &render::ColonizedTreeParams::densityGradient, &data::ColonizedTreeTuningForm::densityGradient },
    { &render::ColonizedTreeParams::foliageDensity, &data::ColonizedTreeTuningForm::foliageDensity },
    { &render::ColonizedTreeParams::leafSizeMin, &data::ColonizedTreeTuningForm::leafSizeMin },
    { &render::ColonizedTreeParams::leafSizeMax, &data::ColonizedTreeTuningForm::leafSizeMax },
    { &render::ColonizedTreeParams::leafSolidStart, &data::ColonizedTreeTuningForm::leafSolidStart },
    { &render::ColonizedTreeParams::leafSolidEnd, &data::ColonizedTreeTuningForm::leafSolidEnd },
    { &render::ColonizedTreeParams::barkTileScale, &data::ColonizedTreeTuningForm::barkTileScale },
    { &render::ColonizedTreeParams::barkHexCell, &data::ColonizedTreeTuningForm::barkHexCell },
    { &render::ColonizedTreeParams::barkHexSharpness, &data::ColonizedTreeTuningForm::barkHexSharpness },
};
constexpr FieldLane<render::ColonizedTreeParams, data::ColonizedTreeTuningForm, i32> kColonizedLanesI32[] = {
    { &render::ColonizedTreeParams::tubeSides, &data::ColonizedTreeTuningForm::tubeSides },
    { &render::ColonizedTreeParams::flareLobes, &data::ColonizedTreeTuningForm::flareLobes },
    { &render::ColonizedTreeParams::curveSubdiv, &data::ColonizedTreeTuningForm::curveSubdiv },
    { &render::ColonizedTreeParams::attractorCount, &data::ColonizedTreeTuningForm::attractorCount },
    { &render::ColonizedTreeParams::leafStyle, &data::ColonizedTreeTuningForm::leafStyle },
    { &render::ColonizedTreeParams::leafShape, &data::ColonizedTreeTuningForm::leafShape },
    { &render::ColonizedTreeParams::leafCount, &data::ColonizedTreeTuningForm::leafCount },
};
constexpr FieldLane<render::ColonizedTreeParams, data::ColonizedTreeTuningForm, Vec3> kColonizedLanesVec3[] = {
    { &render::ColonizedTreeParams::autumnTint, &data::ColonizedTreeTuningForm::autumnTint },
    { &render::ColonizedTreeParams::barkTint, &data::ColonizedTreeTuningForm::barkTint },
};

constexpr FieldLane<render::RcTuning, data::RcTuningForm, f32> kRcLanesF32[] = {
    { &render::RcTuning::fineVoxel, &data::RcTuningForm::fineVoxel },
    { &render::RcTuning::coarseVoxel, &data::RcTuningForm::coarseVoxel },
    { &render::RcTuning::intensity, &data::RcTuningForm::intensity },
    { &render::RcTuning::skyFactor, &data::RcTuningForm::skyFactor },
    { &render::RcTuning::emitterBoost, &data::RcTuningForm::emitterBoost },
    { &render::RcTuning::lightSplatBounce,
      &data::RcTuningForm::lightSplatBounce },
    { &render::RcTuning::bounceFeedback,
      &data::RcTuningForm::bounceFeedback },
    { &render::RcTuning::interval0, &data::RcTuningForm::interval0 },
    { &render::RcTuning::edgeFade, &data::RcTuningForm::edgeFade },
    { &render::RcTuning::bandCount, &data::RcTuningForm::bandCount },
    { &render::RcTuning::bandAa, &data::RcTuningForm::bandAa },
    { &render::RcTuning::giFloor, &data::RcTuningForm::giFloor },
};
constexpr FieldLane<render::RcTuning, data::RcTuningForm, i32> kRcLanesI32[] = {
    { &render::RcTuning::resolution, &data::RcTuningForm::resolution },
    { &render::RcTuning::cascadeCount, &data::RcTuningForm::cascadeCount },
    { &render::RcTuning::updateInterval,
      &data::RcTuningForm::updateInterval },
};
constexpr FieldLane<render::RcTuning, data::RcTuningForm, bool> kRcLanesBool[] = {
    { &render::RcTuning::pipelined, &data::RcTuningForm::pipelined },
    { &render::RcTuning::asyncCompute, &data::RcTuningForm::asyncCompute },
    { &render::RcTuning::rcOnlyLights, &data::RcTuningForm::rcOnlyLights },
    { &render::RcTuning::intervalExtension,
      &data::RcTuningForm::intervalExtension },
};


using RenderTuning = render::WorldRenderer::RenderTuning;

// World-renderer knobs with a 1:1 Form field. Clamped fields, Vec4
// packing and subsystem scalars stay hand-written in apply/capture.
constexpr FieldLane<RenderTuning, data::LandscapeTuningForm, f32> kWorldLanesF32[] = {
    { &RenderTuning::seasonAutumn, &data::LandscapeTuningForm::seasonAutumn },
    { &RenderTuning::seasonLeafFall, &data::LandscapeTuningForm::seasonLeafFall },
    { &RenderTuning::exposure, &data::LandscapeTuningForm::exposure },
    { &RenderTuning::gradeVibrance, &data::LandscapeTuningForm::gradeVibrance },
    { &RenderTuning::gradeSplitTone, &data::LandscapeTuningForm::gradeSplitTone },
    { &RenderTuning::gradeContrast, &data::LandscapeTuningForm::gradeContrast },
    { &RenderTuning::autoExposureMin, &data::LandscapeTuningForm::autoExposureMin },
    { &RenderTuning::autoExposureMax, &data::LandscapeTuningForm::autoExposureMax },
    { &RenderTuning::interiorDaylightWeight, &data::LandscapeTuningForm::interiorDaylightWeight },
    { &RenderTuning::interiorDustDensity, &data::LandscapeTuningForm::interiorDustDensity },
    { &RenderTuning::mistReach, &data::LandscapeTuningForm::mistReach },
    { &RenderTuning::mistCoverageSoftness, &data::LandscapeTuningForm::mistCoverageSoftness },
    { &RenderTuning::mistSunBoost, &data::LandscapeTuningForm::mistSunBoost },
    { &RenderTuning::mistDetailDropout, &data::LandscapeTuningForm::mistDetailDropout },
    { &RenderTuning::mistPuffiness, &data::LandscapeTuningForm::mistPuffiness },
    { &RenderTuning::skyCloudLiningLobe, &data::LandscapeTuningForm::skyCloudLiningLobe },
    { &RenderTuning::skyCloudPowder, &data::LandscapeTuningForm::skyCloudPowder },
    { &RenderTuning::skyCloudPuffiness, &data::LandscapeTuningForm::skyCloudPuffiness },
    { &RenderTuning::skyCloudRimGain, &data::LandscapeTuningForm::skyCloudRimGain },
    { &RenderTuning::skyCloudRimLobe, &data::LandscapeTuningForm::skyCloudRimLobe },
    { &RenderTuning::skyCloudBaseDark, &data::LandscapeTuningForm::skyCloudBaseDark },
};
constexpr FieldLane<RenderTuning, data::LandscapeTuningForm, bool> kWorldLanesBool[] = {
    { &RenderTuning::farTerrain, &data::LandscapeTuningForm::farTerrain },
    { &RenderTuning::clusteredLights, &data::LandscapeTuningForm::clusteredLights },
    { &RenderTuning::mist, &data::LandscapeTuningForm::mistEnabled },
    { &RenderTuning::mistNoiseTex, &data::LandscapeTuningForm::mistNoiseTexture },
    { &RenderTuning::skyClouds, &data::LandscapeTuningForm::skyCloudsVolumetric },
};
constexpr FieldLane<render::PostFx, data::LandscapeTuningForm, f32> kPostFxLanesF32[] = {
    { &render::PostFx::froxelTemporalBlend, &data::LandscapeTuningForm::froxelTemporalBlend },
    { &render::PostFx::froxelDustNoise, &data::LandscapeTuningForm::froxelDustNoise },
    { &render::PostFx::mistTemporalBlend, &data::LandscapeTuningForm::mistTemporalBlend },
};
constexpr FieldLane<render::PostFx, data::LandscapeTuningForm, bool> kPostFxLanesBool[] = {
    { &render::PostFx::froxelFog, &data::LandscapeTuningForm::froxelFog },
};
constexpr FieldLane<render::GrassRenderTuning, data::LandscapeTuningForm, f32> kGrassRenderLanesF32[] = {
    { &render::GrassRenderTuning::bladeHeight, &data::LandscapeTuningForm::grassBladeHeight },
    { &render::GrassRenderTuning::bladeHalfWidth, &data::LandscapeTuningForm::grassBladeHalfWidth },
    { &render::GrassRenderTuning::detailNear, &data::LandscapeTuningForm::grassDetailNear },
    { &render::GrassRenderTuning::detailFar, &data::LandscapeTuningForm::grassDetailFar },
    { &render::GrassRenderTuning::thinStart, &data::LandscapeTuningForm::grassThinStart },
    { &render::GrassRenderTuning::thinEnd, &data::LandscapeTuningForm::grassThinEnd },
    { &render::GrassRenderTuning::farDensity, &data::LandscapeTuningForm::grassFarDensity },
    { &render::GrassRenderTuning::widthCompensation, &data::LandscapeTuningForm::grassWidthCompensation },
    { &render::GrassRenderTuning::fadeStart, &data::LandscapeTuningForm::grassFadeStart },
    { &render::GrassRenderTuning::fadeEnd, &data::LandscapeTuningForm::grassFadeEnd },
    { &render::GrassRenderTuning::rootAo, &data::LandscapeTuningForm::grassRootAo },
    { &render::GrassRenderTuning::sheen, &data::LandscapeTuningForm::grassSheen },
    { &render::GrassRenderTuning::bladeNormals, &data::LandscapeTuningForm::grassBladeNormals },
    { &render::GrassRenderTuning::brightMin, &data::LandscapeTuningForm::grassBrightMin },
    { &render::GrassRenderTuning::brightMax, &data::LandscapeTuningForm::grassBrightMax },
    { &render::GrassRenderTuning::middleDarken, &data::LandscapeTuningForm::grassMiddleDarken },
    { &render::GrassRenderTuning::backscatter, &data::LandscapeTuningForm::grassBackscatter },
};
constexpr FieldLane<render::GrassRenderTuning, data::LandscapeTuningForm, Vec3> kGrassRenderLanesVec3[] = {
    { &render::GrassRenderTuning::baseTint, &data::LandscapeTuningForm::grassBaseTint },
    { &render::GrassRenderTuning::tipTint, &data::LandscapeTuningForm::grassTipTint },
};
constexpr FieldLane<render::GrassScatterTuning, data::LandscapeTuningForm, f32> kGrassScatterLanesF32[] = {
    { &render::GrassScatterTuning::spacing, &data::LandscapeTuningForm::grassSpacing },
    { &render::GrassScatterTuning::patchBroadScale, &data::LandscapeTuningForm::grassPatchBroadScale },
    { &render::GrassScatterTuning::patchDetailScale, &data::LandscapeTuningForm::grassPatchDetailScale },
    { &render::GrassScatterTuning::patchThresholdLo, &data::LandscapeTuningForm::grassPatchThresholdLo },
    { &render::GrassScatterTuning::patchThresholdHi, &data::LandscapeTuningForm::grassPatchThresholdHi },
    { &render::GrassScatterTuning::presenceLo, &data::LandscapeTuningForm::grassPresenceLo },
    { &render::GrassScatterTuning::presenceHi, &data::LandscapeTuningForm::grassPresenceHi },
    { &render::GrassScatterTuning::materialCutoff, &data::LandscapeTuningForm::grassMaterialCutoff },
};

} // namespace

void RenderTuningIo::applyTuning(
    render::WorldRenderer& r, const data::LandscapeTuningForm& tuning,
    const sptr<const render::HeightPatches>& patches,
    const sptr<const render::TerrainBase>& base, f32 activeSnowLine) {
    // Terrain shape + startup values for every live-adjustable knob the
    // render panel owns (§5: the TOML sets where it all starts; the scene
    // keeps the atmosphere half in `atmos`). Terrain shape is apply-only
    // (never captured back — the world's identity is not a slider).
    r.terrain.params.seed = tuning.terrainSeed;
    r.terrain.params.patches = patches;
    r.terrain.params.base = base;
    r.terrain.params.hillWavelength = tuning.hillWavelength;
    r.terrain.params.hillAmplitude = tuning.hillAmplitude;
    r.terrain.params.octaves = tuning.terrainOctaves;
    r.terrain.params.lacunarity = tuning.terrainLacunarity;
    r.terrain.params.gain = tuning.terrainGain;
    r.terrain.params.mountainWavelength = tuning.mountainWavelength;
    r.terrain.params.mountainAmplitude = tuning.mountainAmplitude;
    r.terrain.params.mountainMaskLow = tuning.mountainMaskLow;
    r.terrain.params.mountainMaskHigh = tuning.mountainMaskHigh;
    r.terrain.params.seaLevel = tuning.seaLevel;
    r.terrain.params.snowLine = activeSnowLine;
    r.terrain.params.treeLineFactor = tuning.treeLineFactor;
    r.terrain.viewRadius =
        glm::clamp(tuning.terrainViewRadius, 8,
                   render::TerrainSystem::kMaxViewRadius);

    // The 1:1 knobs ride the lane tables (captureTuning walks the SAME
    // tables back). (tuning.ssaoStrength is unused — screen AO removed.)
    applyLanes(r.tuning, tuning, kWorldLanesF32);
    applyLanes(r.tuning, tuning, kWorldLanesBool);
    applyLanes(r.postFx, tuning, kPostFxLanesF32);
    applyLanes(r.postFx, tuning, kPostFxLanesBool);
    applyLanes(r.grass.renderTuning, tuning, kGrassRenderLanesF32);
    applyLanes(r.grass.renderTuning, tuning, kGrassRenderLanesVec3);
    // Startup-only mapping: the scatter bake reads these on first
    // request, no regenerate needed (nothing is resident yet).
    applyLanes(r.grass.scatterTuning, tuning, kGrassScatterLanesF32);

    // Vec4 packing (the Form spells the lanes as scalars) and clamps.
    r.tuning.stylizedDiffuse = { tuning.stylizedDiffuseEdge0Start,
                                 tuning.stylizedDiffuseEdge0End,
                                 tuning.stylizedDiffuseEdge1Start,
                                 tuning.stylizedDiffuseEdge1End };
    r.tuning.stylizedShadow = { tuning.stylizedShadowStart,
                                tuning.stylizedShadowEnd,
                                tuning.stylizedShadowFloor,
                                tuning.stylizedHalfTone };
    r.tuning.stylizedSpec = { tuning.stylizedSpecStrength,
                              tuning.stylizedSpecThreshold,
                              tuning.stylizedSpecExponent, 0.0f };
    r.tuning.mistShape = { tuning.mistCoverageScale, tuning.mistErosionScale,
                           tuning.mistErosionStrength, tuning.mistLift };
    r.tuning.mistLight = { tuning.mistSunLobe, tuning.mistBackscatter,
                           tuning.mistAmbientGain, tuning.mistShadowFloor };
    r.tuning.skyCloudShape = { tuning.skyCloudThickness,
                               tuning.skyCloudDensity, tuning.skyCloudErosion,
                               tuning.skyCloudThicknessSpread };
    r.tuning.skyCloudLight = { tuning.skyCloudSunGain, tuning.skyCloudSunLobe,
                               tuning.skyCloudAmbientGain,
                               tuning.skyCloudLiningGain };
    r.tuning.shadowResolution =
        glm::clamp(tuning.shadowResolution, 1024, 4096);
    r.tuning.reflectionScale =
        glm::clamp(tuning.reflectionScale, 0.25f, 0.5f);
    r.tuning.mistSteps = glm::clamp(tuning.mistSteps, 4, 64);
    // Vegetation draw budget (clamped — the streamer ring
    // and the Hi-Z candidate cap size the safe range).
    r.vegetation.viewRadius = glm::clamp(tuning.vegViewRadius, 4, 24);
    r.vegetation.highDetailRadius =
        glm::clamp(tuning.vegHighDetailRadius, 0, 8);
    r.vegetation.lowDetailRadius =
        glm::clamp(tuning.vegLowDetailRadius, 2, 12);
}

void RenderTuningIo::applyTreeTuning(
    render::WorldRenderer& r, const data::LobeTreeTuningForm& lobes,
    const data::ColonizedTreeTuningForm& colonized) {
    r.vegetation.lobeTreeParams = toLobeParams(lobes);
    r.vegetation.colonizedTreeParams = toColonizedParams(colonized);
}

render::LobeTreeParams RenderTuningIo::toLobeParams(
    const data::LobeTreeTuningForm& lobes) {
    // Form -> flat engine params through the lane tables (§4: engine
    // never sees data/). The Trees panel then edits the params live.
    render::LobeTreeParams l;
    applyLanes(l, lobes, kLobeLanesF32);
    applyLanes(l, lobes, kLobeLanesI32);
    return l;
}

render::ColonizedTreeParams RenderTuningIo::toColonizedParams(
    const data::ColonizedTreeTuningForm& colonized) {
    render::ColonizedTreeParams c;
    applyLanes(c, colonized, kColonizedLanesF32);
    applyLanes(c, colonized, kColonizedLanesI32);
    applyLanes(c, colonized, kColonizedLanesVec3);
    return c;
}

void RenderTuningIo::applyRcTuning(render::WorldRenderer& r,
                                   const data::RcTuningForm& rc) {
    render::RcTuning& t = r.radianceCascades.tuning;
    applyLanes(t, rc, kRcLanesF32);
    applyLanes(t, rc, kRcLanesI32);
    applyLanes(t, rc, kRcLanesBool);
    // The one non-lane field: the Form spells the technique as an i32.
    t.technique = rc.technique == 1 ? render::GiTechnique::RadianceCascades
                                    : render::GiTechnique::Classic;
}

void RenderTuningIo::captureTuning(const render::WorldRenderer& r,
                                   data::LandscapeTuningForm& out) {
    captureLanes(r.tuning, out, kWorldLanesF32);
    captureLanes(r.tuning, out, kWorldLanesBool);
    captureLanes(r.postFx, out, kPostFxLanesF32);
    captureLanes(r.postFx, out, kPostFxLanesBool);
    captureLanes(r.grass.renderTuning, out, kGrassRenderLanesF32);
    captureLanes(r.grass.renderTuning, out, kGrassRenderLanesVec3);
    captureLanes(r.grass.scatterTuning, out, kGrassScatterLanesF32);

    // Vec4 unpacking + the clamped/subsystem scalars (mirror of
    // applyTuning's hand-written half; terrain SHAPE stays apply-only).
    out.stylizedDiffuseEdge0Start = r.tuning.stylizedDiffuse.x;
    out.stylizedDiffuseEdge0End = r.tuning.stylizedDiffuse.y;
    out.stylizedDiffuseEdge1Start = r.tuning.stylizedDiffuse.z;
    out.stylizedDiffuseEdge1End = r.tuning.stylizedDiffuse.w;
    out.stylizedShadowStart = r.tuning.stylizedShadow.x;
    out.stylizedShadowEnd = r.tuning.stylizedShadow.y;
    out.stylizedShadowFloor = r.tuning.stylizedShadow.z;
    out.stylizedHalfTone = r.tuning.stylizedShadow.w;
    out.stylizedSpecStrength = r.tuning.stylizedSpec.x;
    out.stylizedSpecThreshold = r.tuning.stylizedSpec.y;
    out.stylizedSpecExponent = r.tuning.stylizedSpec.z;
    out.mistCoverageScale = r.tuning.mistShape.x;
    out.mistErosionScale = r.tuning.mistShape.y;
    out.mistErosionStrength = r.tuning.mistShape.z;
    out.mistLift = r.tuning.mistShape.w;
    out.mistSunLobe = r.tuning.mistLight.x;
    out.mistBackscatter = r.tuning.mistLight.y;
    out.mistAmbientGain = r.tuning.mistLight.z;
    out.mistShadowFloor = r.tuning.mistLight.w;
    out.skyCloudThickness = r.tuning.skyCloudShape.x;
    out.skyCloudDensity = r.tuning.skyCloudShape.y;
    out.skyCloudErosion = r.tuning.skyCloudShape.z;
    out.skyCloudThicknessSpread = r.tuning.skyCloudShape.w;
    out.skyCloudSunGain = r.tuning.skyCloudLight.x;
    out.skyCloudSunLobe = r.tuning.skyCloudLight.y;
    out.skyCloudAmbientGain = r.tuning.skyCloudLight.z;
    out.skyCloudLiningGain = r.tuning.skyCloudLight.w;
    out.shadowResolution = r.tuning.shadowResolution;
    out.reflectionScale = r.tuning.reflectionScale;
    out.mistSteps = r.tuning.mistSteps;
    out.terrainViewRadius = r.terrain.viewRadius;
    out.vegViewRadius = r.vegetation.viewRadius;
    out.vegHighDetailRadius = r.vegetation.highDetailRadius;
    out.vegLowDetailRadius = r.vegetation.lowDetailRadius;
}

void RenderTuningIo::captureRcTuning(const render::WorldRenderer& r,
                                     data::RcTuningForm& out) {
    const render::RcTuning& t = r.radianceCascades.tuning;
    captureLanes(t, out, kRcLanesF32);
    captureLanes(t, out, kRcLanesI32);
    captureLanes(t, out, kRcLanesBool);
    out.technique =
        t.technique == render::GiTechnique::RadianceCascades ? 1 : 0;
}

void RenderTuningIo::captureTreeTuning(
    const render::WorldRenderer& r, data::LobeTreeTuningForm& lobes,
    data::ColonizedTreeTuningForm& colonized) {
    const render::LobeTreeParams& l = r.vegetation.lobeTreeParams;
    captureLanes(l, lobes, kLobeLanesF32);
    captureLanes(l, lobes, kLobeLanesI32);
    const render::ColonizedTreeParams& c = r.vegetation.colonizedTreeParams;
    captureLanes(c, colonized, kColonizedLanesF32);
    captureLanes(c, colonized, kColonizedLanesI32);
    captureLanes(c, colonized, kColonizedLanesVec3);
}

} // namespace game
