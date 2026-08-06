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
    r.renderScaleUi = glm::clamp(tuning.renderScale, 0.5f, 1.0f);
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

void RenderTuningIo::applyTreeTuning(
    render::WorldRenderer& r, const data::LobeTreeTuningForm& lobes,
    const data::ColonizedTreeTuningForm& colonized) {
    r.vegetation.lobeTreeParams = toLobeParams(lobes);
    r.vegetation.colonizedTreeParams = toColonizedParams(colonized);
}

render::LobeTreeParams RenderTuningIo::toLobeParams(
    const data::LobeTreeTuningForm& lobes) {
    // Field-for-field Form -> flat engine params (§4: engine never sees
    // data/). The Trees panel then edits the params live.
    render::LobeTreeParams l;
    l.trunkHeightMin = lobes.trunkHeightMin;
    l.trunkHeightMax = lobes.trunkHeightMax;
    l.trunkRadiusMin = lobes.trunkRadiusMin;
    l.trunkRadiusMax = lobes.trunkRadiusMax;
    l.trunkTaper = lobes.trunkTaper;
    l.lean = lobes.lean;
    l.branchCountMin = lobes.branchCountMin;
    l.branchCountMax = lobes.branchCountMax;
    l.branchLengthMin = lobes.branchLengthMin;
    l.branchLengthMax = lobes.branchLengthMax;
    l.crownLobeRadiusMin = lobes.crownLobeRadiusMin;
    l.crownLobeRadiusMax = lobes.crownLobeRadiusMax;
    l.branchLobeRadiusMin = lobes.branchLobeRadiusMin;
    l.branchLobeRadiusMax = lobes.branchLobeRadiusMax;
    l.lobeFlatten = lobes.lobeFlatten;
    l.normalSpherize = lobes.normalSpherize;
    return l;
}

render::ColonizedTreeParams RenderTuningIo::toColonizedParams(
    const data::ColonizedTreeTuningForm& colonized) {
    render::ColonizedTreeParams c;
    c.tubeSides = colonized.tubeSides;
    c.curvePreserve = colonized.curvePreserve;
    c.curveSubdiv = colonized.curveSubdiv;
    c.pathJitter = colonized.pathJitter;
    c.ringIrregularity = colonized.ringIrregularity;
    c.sideMinFraction = colonized.sideMinFraction;
    c.segment = colonized.segment;
    c.killDistance = colonized.killDistance;
    c.attractorCount = colonized.attractorCount;
    c.pipeExponent = colonized.pipeExponent;
    c.tropism = colonized.tropism;
    c.trunkBaseMin = colonized.trunkBaseMin;
    c.trunkBaseMax = colonized.trunkBaseMax;
    c.crownHeightMin = colonized.crownHeightMin;
    c.crownHeightMax = colonized.crownHeightMax;
    c.crownRadiusMin = colonized.crownRadiusMin;
    c.crownRadiusMax = colonized.crownRadiusMax;
    c.crownTaper = colonized.crownTaper;
    c.leaderBias = colonized.leaderBias;
    c.lateralFlatten = colonized.lateralFlatten;
    c.sprayFoliage = colonized.sprayFoliage;
    c.tipBallRadius = colonized.tipBallRadius;
    c.tipOrderFalloff = colonized.tipOrderFalloff;
    c.tipBallMin = colonized.tipBallMin;
    c.leafStyle = colonized.leafStyle;
    c.leafShape = colonized.leafShape;
    c.autumnTint = colonized.autumnTint;
    c.seasonality = colonized.seasonality;
    c.smoothK = colonized.smoothK;
    c.cardHalfSizeMin = colonized.cardHalfSizeMin;
    c.cardHalfSizeMax = colonized.cardHalfSizeMax;
    c.densityGradient = colonized.densityGradient;
    c.foliageDensity = colonized.foliageDensity;
    c.leafCount = colonized.leafCount;
    c.leafSizeMin = colonized.leafSizeMin;
    c.leafSizeMax = colonized.leafSizeMax;
    c.leafSolidStart = colonized.leafSolidStart;
    c.leafSolidEnd = colonized.leafSolidEnd;
    return c;
}

void RenderTuningIo::applyRcTuning(render::WorldRenderer& r,
                                   const data::RcTuningForm& rc) {
    render::RcTuning& t = r.radianceCascades.tuning;
    t.resolution = rc.resolution;
    t.fineVoxel = rc.fineVoxel;
    t.coarseVoxel = rc.coarseVoxel;
    t.cascadeCount = rc.cascadeCount;
    t.updateInterval = rc.updateInterval;
    t.technique = rc.technique == 1 ? render::GiTechnique::RadianceCascades
                                    : render::GiTechnique::Classic;
    t.intensity = rc.intensity;
    t.skyFactor = rc.skyFactor;
    t.emitterBoost = rc.emitterBoost;
    t.lightSplatBounce = rc.lightSplatBounce;
    t.pipelined = rc.pipelined;
    t.asyncCompute = rc.asyncCompute;
    t.bounceFeedback = rc.bounceFeedback;
    t.rcOnlyLights = rc.rcOnlyLights;
    t.interval0 = rc.interval0;
    t.edgeFade = rc.edgeFade;
    t.bandCount = rc.bandCount;
    t.bandAa = rc.bandAa;
    t.giFloor = rc.giFloor;
    t.intervalExtension = rc.intervalExtension;
}

void RenderTuningIo::captureTuning(const render::WorldRenderer& r,
                                   data::LandscapeTuningForm& out) {
    out.renderScale = r.renderScaleUi;
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
    out.resolution = t.resolution;
    out.fineVoxel = t.fineVoxel;
    out.coarseVoxel = t.coarseVoxel;
    out.cascadeCount = t.cascadeCount;
    out.updateInterval = t.updateInterval;
    out.technique =
        t.technique == render::GiTechnique::RadianceCascades ? 1 : 0;
    out.intensity = t.intensity;
    out.skyFactor = t.skyFactor;
    out.emitterBoost = t.emitterBoost;
    out.lightSplatBounce = t.lightSplatBounce;
    out.pipelined = t.pipelined;
    out.asyncCompute = t.asyncCompute;
    out.bounceFeedback = t.bounceFeedback;
    out.rcOnlyLights = t.rcOnlyLights;
    out.interval0 = t.interval0;
    out.edgeFade = t.edgeFade;
    out.bandCount = t.bandCount;
    out.bandAa = t.bandAa;
    out.giFloor = t.giFloor;
    out.intervalExtension = t.intervalExtension;
}

void RenderTuningIo::captureTreeTuning(
    const render::WorldRenderer& r, data::LobeTreeTuningForm& lobes,
    data::ColonizedTreeTuningForm& colonized) {
    const render::LobeTreeParams& l = r.vegetation.lobeTreeParams;
    lobes.trunkHeightMin = l.trunkHeightMin;
    lobes.trunkHeightMax = l.trunkHeightMax;
    lobes.trunkRadiusMin = l.trunkRadiusMin;
    lobes.trunkRadiusMax = l.trunkRadiusMax;
    lobes.trunkTaper = l.trunkTaper;
    lobes.lean = l.lean;
    lobes.branchCountMin = l.branchCountMin;
    lobes.branchCountMax = l.branchCountMax;
    lobes.branchLengthMin = l.branchLengthMin;
    lobes.branchLengthMax = l.branchLengthMax;
    lobes.crownLobeRadiusMin = l.crownLobeRadiusMin;
    lobes.crownLobeRadiusMax = l.crownLobeRadiusMax;
    lobes.branchLobeRadiusMin = l.branchLobeRadiusMin;
    lobes.branchLobeRadiusMax = l.branchLobeRadiusMax;
    lobes.lobeFlatten = l.lobeFlatten;
    lobes.normalSpherize = l.normalSpherize;
    const render::ColonizedTreeParams& c = r.vegetation.colonizedTreeParams;
    colonized.tubeSides = c.tubeSides;
    colonized.curvePreserve = c.curvePreserve;
    colonized.curveSubdiv = c.curveSubdiv;
    colonized.pathJitter = c.pathJitter;
    colonized.ringIrregularity = c.ringIrregularity;
    colonized.sideMinFraction = c.sideMinFraction;
    colonized.segment = c.segment;
    colonized.killDistance = c.killDistance;
    colonized.attractorCount = c.attractorCount;
    colonized.pipeExponent = c.pipeExponent;
    colonized.tropism = c.tropism;
    colonized.trunkBaseMin = c.trunkBaseMin;
    colonized.trunkBaseMax = c.trunkBaseMax;
    colonized.crownHeightMin = c.crownHeightMin;
    colonized.crownHeightMax = c.crownHeightMax;
    colonized.crownRadiusMin = c.crownRadiusMin;
    colonized.crownRadiusMax = c.crownRadiusMax;
    colonized.tipBallRadius = c.tipBallRadius;
    colonized.tipOrderFalloff = c.tipOrderFalloff;
    colonized.smoothK = c.smoothK;
    colonized.cardHalfSizeMin = c.cardHalfSizeMin;
    colonized.cardHalfSizeMax = c.cardHalfSizeMax;
    colonized.densityGradient = c.densityGradient;
    colonized.foliageDensity = c.foliageDensity;
    colonized.leafCount = c.leafCount;
    colonized.leafSizeMin = c.leafSizeMin;
    colonized.leafSizeMax = c.leafSizeMax;
    colonized.leafSolidStart = c.leafSolidStart;
    colonized.leafSolidEnd = c.leafSolidEnd;
}

} // namespace game
