#include "game/scenes/RenderTuningIo.hpp"

#include <glm/glm.hpp>

#include "engine/render/WorldRenderer.hpp"
#include "game/scenes/LandscapeTuning.hpp"

namespace game {

void RenderTuningIo::applyTuning(
    render::WorldRenderer& r, const data::LandscapeTuningForm& tuning,
    const sptr<const render::HeightPatches>& patches) {
    // Terrain shape + startup values for every live-adjustable knob the
    // render panel owns (§5: the TOML sets where it all starts; the scene
    // keeps the atmosphere half in `atmos`).
    r.terrain.params.seed = tuning.terrainSeed;
    r.terrain.params.patches = patches;
    r.terrain.params.hillWavelength = tuning.hillWavelength;
    r.terrain.params.hillAmplitude = tuning.hillAmplitude;
    r.terrain.params.mountainWavelength = tuning.mountainWavelength;
    r.terrain.params.mountainAmplitude = tuning.mountainAmplitude;
    r.terrain.params.seaLevel = tuning.seaLevel;
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
    r.shadowResolutionUi = glm::clamp(tuning.shadowResolution, 1024, 4096);
    r.interiorDaylightWeightUi = tuning.interiorDaylightWeight;
    r.clusteredLightsUi = tuning.clusteredLights;
    r.postFx.froxelFog = tuning.froxelFog;
    r.postFx.froxelTemporalBlend = tuning.froxelTemporalBlend;
    r.postFx.froxelDustNoise = tuning.froxelDustNoise;
    r.interiorDustDensityUi = tuning.interiorDustDensity;
    // Vegetation draw budget (clamped — the streamer ring
    // and the Hi-Z candidate cap size the safe range).
    r.vegetation.viewRadius = glm::clamp(tuning.vegViewRadius, 4, 15);
    r.vegetation.highDetailRadius =
        glm::clamp(tuning.vegHighDetailRadius, 0, 8);
    r.vegetation.lowDetailRadius =
        glm::clamp(tuning.vegLowDetailRadius, 2, 12);
}

void RenderTuningIo::applyTreeTuning(
    render::WorldRenderer& r, const data::LobeTreeTuningForm& lobes,
    const data::ColonizedTreeTuningForm& colonized) {
    // Field-for-field Form -> flat engine params (§4: engine never sees
    // data/). The Trees panel then edits the params live.
    render::LobeTreeParams& l = r.vegetation.lobeTreeParams;
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
    render::ColonizedTreeParams& c = r.vegetation.colonizedTreeParams;
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
    c.tipBallRadius = colonized.tipBallRadius;
    c.tipOrderFalloff = colonized.tipOrderFalloff;
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
    out.shadowResolution = r.shadowResolutionUi;
    out.interiorDaylightWeight = r.interiorDaylightWeightUi;
    out.clusteredLights = r.clusteredLightsUi;
    out.froxelFog = r.postFx.froxelFog;
    out.froxelTemporalBlend = r.postFx.froxelTemporalBlend;
    out.froxelDustNoise = r.postFx.froxelDustNoise;
    out.interiorDustDensity = r.interiorDustDensityUi;
    out.vegViewRadius = r.vegetation.viewRadius;
    out.vegHighDetailRadius = r.vegetation.highDetailRadius;
    out.vegLowDetailRadius = r.vegetation.lowDetailRadius;
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
