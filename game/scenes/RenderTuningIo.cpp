#include "game/scenes/RenderTuningIo.hpp"

#include <glm/glm.hpp>

#include "engine/render/WorldRenderer.hpp"
#include "game/scenes/LandscapeTuning.hpp"

namespace game {

void RenderTuningIo::applyTuning(
    render::WorldRenderer& r, const data::LandscapeTuningForm& tuning,
    const sptr<const render::HeightPatches>& patches,
    const sptr<const render::TerrainBase>& base, f32 activeSnowLine) {
    // Terrain shape + startup values for every live-adjustable knob the
    // render panel owns (§5: the TOML sets where it all starts; the scene
    // keeps the atmosphere half in `atmos`).
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
    r.seasonAutumnUi = tuning.seasonAutumn;
    r.seasonLeafFallUi = tuning.seasonLeafFall;
    r.terrain.viewRadius =
        glm::clamp(tuning.terrainViewRadius, 8,
                   render::TerrainSystem::kMaxViewRadius);
    r.farTerrainUi = tuning.farTerrain;
    r.exposureUi = tuning.exposure;
    // (tuning.ssaoStrength is unused — screen-space AO removed.)
    r.gradeVibranceUi = tuning.gradeVibrance;
    r.gradeSplitToneUi = tuning.gradeSplitTone;
    r.gradeContrastUi = tuning.gradeContrast;
    r.autoExposureMinUi = tuning.autoExposureMin;
    r.autoExposureMaxUi = tuning.autoExposureMax;
    r.stylizedDiffuseUi = { tuning.stylizedDiffuseEdge0Start,
                            tuning.stylizedDiffuseEdge0End,
                            tuning.stylizedDiffuseEdge1Start,
                            tuning.stylizedDiffuseEdge1End };
    r.stylizedShadowUi = { tuning.stylizedShadowStart,
                           tuning.stylizedShadowEnd,
                           tuning.stylizedShadowFloor,
                           tuning.stylizedHalfTone };
    r.stylizedSpecUi = { tuning.stylizedSpecStrength,
                         tuning.stylizedSpecThreshold,
                         tuning.stylizedSpecExponent, 0.0f };
    r.shadowResolutionUi = glm::clamp(tuning.shadowResolution, 1024, 4096);
    r.reflectionScaleUi = glm::clamp(tuning.reflectionScale, 0.25f, 0.5f);
    r.interiorDaylightWeightUi = tuning.interiorDaylightWeight;
    r.clusteredLightsUi = tuning.clusteredLights;
    r.postFx.froxelFog = tuning.froxelFog;
    r.postFx.froxelTemporalBlend = tuning.froxelTemporalBlend;
    r.postFx.froxelDustNoise = tuning.froxelDustNoise;
    r.interiorDustDensityUi = tuning.interiorDustDensity;
    r.mistUi = tuning.mistEnabled;
    r.mistReachUi = tuning.mistReach;
    r.mistCoverageSoftnessUi = tuning.mistCoverageSoftness;
    r.mistShapeUi = { tuning.mistCoverageScale, tuning.mistErosionScale,
                      tuning.mistErosionStrength, tuning.mistLift };
    r.mistSunBoostUi = tuning.mistSunBoost;
    r.mistLightUi = { tuning.mistSunLobe, tuning.mistBackscatter,
                      tuning.mistAmbientGain, tuning.mistShadowFloor };
    r.mistNoiseTexUi = tuning.mistNoiseTexture;
    r.mistDetailDropoutUi = tuning.mistDetailDropout;
    r.mistStepsUi = glm::clamp(tuning.mistSteps, 4, 64);
    r.postFx.mistTemporalBlend = tuning.mistTemporalBlend;
    r.mistPuffinessUi = tuning.mistPuffiness;
    r.skyCloudsUi = tuning.skyCloudsVolumetric;
    r.skyCloudShapeUi = { tuning.skyCloudThickness, tuning.skyCloudDensity,
                          tuning.skyCloudErosion,
                          tuning.skyCloudThicknessSpread };
    r.skyCloudLightUi = { tuning.skyCloudSunGain, tuning.skyCloudSunLobe,
                          tuning.skyCloudAmbientGain,
                          tuning.skyCloudLiningGain };
    r.skyCloudLiningLobeUi = tuning.skyCloudLiningLobe;
    r.skyCloudPowderUi = tuning.skyCloudPowder;
    r.skyCloudPuffinessUi = tuning.skyCloudPuffiness;
    r.skyCloudRimGainUi = tuning.skyCloudRimGain;
    r.skyCloudRimLobeUi = tuning.skyCloudRimLobe;
    r.skyCloudBaseDarkUi = tuning.skyCloudBaseDark;
    // Vegetation draw budget (clamped — the streamer ring
    // and the Hi-Z candidate cap size the safe range).
    r.vegetation.viewRadius = glm::clamp(tuning.vegViewRadius, 4, 24);
    r.vegetation.highDetailRadius =
        glm::clamp(tuning.vegHighDetailRadius, 0, 8);
    r.vegetation.lowDetailRadius =
        glm::clamp(tuning.vegLowDetailRadius, 2, 12);
    render::GrassRenderTuning& gr = r.grass.renderTuning;
    gr.bladeHeight = tuning.grassBladeHeight;
    gr.bladeHalfWidth = tuning.grassBladeHalfWidth;
    gr.detailNear = tuning.grassDetailNear;
    gr.detailFar = tuning.grassDetailFar;
    gr.thinStart = tuning.grassThinStart;
    gr.thinEnd = tuning.grassThinEnd;
    gr.farDensity = tuning.grassFarDensity;
    gr.widthCompensation = tuning.grassWidthCompensation;
    gr.fadeStart = tuning.grassFadeStart;
    gr.fadeEnd = tuning.grassFadeEnd;
    gr.baseTint = tuning.grassBaseTint;
    gr.tipTint = tuning.grassTipTint;
    gr.rootAo = tuning.grassRootAo;
    gr.sheen = tuning.grassSheen;
    gr.bladeNormals = tuning.grassBladeNormals;
    gr.brightMin = tuning.grassBrightMin;
    gr.brightMax = tuning.grassBrightMax;
    gr.middleDarken = tuning.grassMiddleDarken;
    gr.backscatter = tuning.grassBackscatter;
    // Startup-only mapping: the scatter bake reads these on first
    // request, no regenerate needed (nothing is resident yet).
    render::GrassScatterTuning& gs = r.grass.scatterTuning;
    gs.spacing = tuning.grassSpacing;
    gs.patchBroadScale = tuning.grassPatchBroadScale;
    gs.patchDetailScale = tuning.grassPatchDetailScale;
    gs.patchThresholdLo = tuning.grassPatchThresholdLo;
    gs.patchThresholdHi = tuning.grassPatchThresholdHi;
    gs.presenceLo = tuning.grassPresenceLo;
    gs.presenceHi = tuning.grassPresenceHi;
    gs.materialCutoff = tuning.grassMaterialCutoff;
}

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

} // namespace

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
    out.exposure = r.exposureUi;
    out.gradeVibrance = r.gradeVibranceUi;
    out.gradeSplitTone = r.gradeSplitToneUi;
    out.gradeContrast = r.gradeContrastUi;
    out.autoExposureMin = r.autoExposureMinUi;
    out.autoExposureMax = r.autoExposureMaxUi;
    out.stylizedDiffuseEdge0Start = r.stylizedDiffuseUi.x;
    out.stylizedDiffuseEdge0End = r.stylizedDiffuseUi.y;
    out.stylizedDiffuseEdge1Start = r.stylizedDiffuseUi.z;
    out.stylizedDiffuseEdge1End = r.stylizedDiffuseUi.w;
    out.stylizedHalfTone = r.stylizedShadowUi.w;
    out.stylizedShadowStart = r.stylizedShadowUi.x;
    out.stylizedShadowEnd = r.stylizedShadowUi.y;
    out.stylizedShadowFloor = r.stylizedShadowUi.z;
    out.stylizedSpecStrength = r.stylizedSpecUi.x;
    out.stylizedSpecThreshold = r.stylizedSpecUi.y;
    out.stylizedSpecExponent = r.stylizedSpecUi.z;
    out.shadowResolution = r.shadowResolutionUi;
    out.reflectionScale = r.reflectionScaleUi;
    out.interiorDaylightWeight = r.interiorDaylightWeightUi;
    out.clusteredLights = r.clusteredLightsUi;
    out.froxelFog = r.postFx.froxelFog;
    out.froxelTemporalBlend = r.postFx.froxelTemporalBlend;
    out.froxelDustNoise = r.postFx.froxelDustNoise;
    out.interiorDustDensity = r.interiorDustDensityUi;
    out.mistEnabled = r.mistUi;
    out.mistReach = r.mistReachUi;
    out.mistCoverageSoftness = r.mistCoverageSoftnessUi;
    out.mistCoverageScale = r.mistShapeUi.x;
    out.mistErosionScale = r.mistShapeUi.y;
    out.mistErosionStrength = r.mistShapeUi.z;
    out.mistLift = r.mistShapeUi.w;
    out.mistSunBoost = r.mistSunBoostUi;
    out.mistSunLobe = r.mistLightUi.x;
    out.mistBackscatter = r.mistLightUi.y;
    out.mistAmbientGain = r.mistLightUi.z;
    out.mistShadowFloor = r.mistLightUi.w;
    out.mistNoiseTexture = r.mistNoiseTexUi;
    out.mistDetailDropout = r.mistDetailDropoutUi;
    out.mistSteps = r.mistStepsUi;
    out.mistTemporalBlend = r.postFx.mistTemporalBlend;
    out.mistPuffiness = r.mistPuffinessUi;
    out.skyCloudsVolumetric = r.skyCloudsUi;
    out.skyCloudThickness = r.skyCloudShapeUi.x;
    out.skyCloudDensity = r.skyCloudShapeUi.y;
    out.skyCloudErosion = r.skyCloudShapeUi.z;
    out.skyCloudSunGain = r.skyCloudLightUi.x;
    out.skyCloudSunLobe = r.skyCloudLightUi.y;
    out.skyCloudAmbientGain = r.skyCloudLightUi.z;
    out.skyCloudLiningGain = r.skyCloudLightUi.w;
    out.skyCloudLiningLobe = r.skyCloudLiningLobeUi;
    out.skyCloudPowder = r.skyCloudPowderUi;
    out.skyCloudThicknessSpread = r.skyCloudShapeUi.w;
    out.skyCloudPuffiness = r.skyCloudPuffinessUi;
    out.skyCloudRimGain = r.skyCloudRimGainUi;
    out.skyCloudRimLobe = r.skyCloudRimLobeUi;
    out.skyCloudBaseDark = r.skyCloudBaseDarkUi;
    out.terrainViewRadius = r.terrain.viewRadius;
    out.seasonAutumn = r.seasonAutumnUi;
    out.seasonLeafFall = r.seasonLeafFallUi;
    out.farTerrain = r.farTerrainUi;
    out.vegViewRadius = r.vegetation.viewRadius;
    out.vegHighDetailRadius = r.vegetation.highDetailRadius;
    out.vegLowDetailRadius = r.vegetation.lowDetailRadius;
    const render::GrassRenderTuning& gr = r.grass.renderTuning;
    out.grassBladeHeight = gr.bladeHeight;
    out.grassBladeHalfWidth = gr.bladeHalfWidth;
    out.grassDetailNear = gr.detailNear;
    out.grassDetailFar = gr.detailFar;
    out.grassThinStart = gr.thinStart;
    out.grassThinEnd = gr.thinEnd;
    out.grassFarDensity = gr.farDensity;
    out.grassWidthCompensation = gr.widthCompensation;
    out.grassFadeStart = gr.fadeStart;
    out.grassFadeEnd = gr.fadeEnd;
    out.grassBaseTint = gr.baseTint;
    out.grassTipTint = gr.tipTint;
    out.grassRootAo = gr.rootAo;
    out.grassSheen = gr.sheen;
    out.grassBladeNormals = gr.bladeNormals;
    out.grassBrightMin = gr.brightMin;
    out.grassBrightMax = gr.brightMax;
    out.grassMiddleDarken = gr.middleDarken;
    out.grassBackscatter = gr.backscatter;
    const render::GrassScatterTuning& gs = r.grass.scatterTuning;
    out.grassSpacing = gs.spacing;
    out.grassPatchBroadScale = gs.patchBroadScale;
    out.grassPatchDetailScale = gs.patchDetailScale;
    out.grassPatchThresholdLo = gs.patchThresholdLo;
    out.grassPatchThresholdHi = gs.patchThresholdHi;
    out.grassPresenceLo = gs.presenceLo;
    out.grassPresenceHi = gs.presenceHi;
    out.grassMaterialCutoff = gs.materialCutoff;
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
