#include "game/scenes/RenderTuningIo.hpp"

#include <glm/glm.hpp>

#include "engine/core/Assert.hpp"
#include "engine/reflect/FieldLane.hpp"
#include "engine/render/AtmosphereParams.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "game/scenes/LandscapeTuning.hpp"

namespace game {

namespace {

using namespace tuning_lanes;

// The slider range reflected on a Form field (REFLECT_FIELD_RANGE), as
// the clamp applied to loaded data: the panel and the seam read the one
// definition. A field without a range is a programming error.
template <typename Form, typename T>
const reflect::FieldRange& rangeOf(T Form::* field) {
    const reflect::FieldInfo* info =
        Form::staticTypeInfo().findFieldAt(reflect::memberOffset(field));
    ENGINE_ASSERT_MSG(info && info->range, "{}: field without a range",
                      Form::staticTypeInfo().name);
    static constexpr reflect::FieldRange kUnbounded { -3.4e38f, 3.4e38f };
    return info && info->range ? *info->range : kUnbounded;
}

f32 clampToRange(f32 value, const reflect::FieldRange& range) {
    return glm::clamp(value, range.lo, range.hi);
}
i32 clampToRange(i32 value, const reflect::FieldRange& range) {
    return glm::clamp(value, static_cast<i32>(range.lo),
                      static_cast<i32>(range.hi));
}

} // namespace

void RenderTuningIo::applyTuning(
    render::WorldRenderer& r, render::TerrainParams& terrain,
    const data::LandscapeTuningForm& tuning,
    const sptr<const render::HeightPatches>& patches,
    const sptr<const render::TerrainBase>& base, f32 activeSnowLine) {
    // Terrain shape + startup values for every live-adjustable knob the
    // render panel owns (§5: the TOML sets where it all starts; the scene
    // keeps the atmosphere half in `atmos`). Terrain shape is apply-only
    // (never captured back — the world's identity is not a slider).
    terrain.seed = tuning.terrainSeed;
    terrain.patches = patches;
    terrain.base = base;
    terrain.hillWavelength = tuning.hillWavelength;
    terrain.hillAmplitude = tuning.hillAmplitude;
    terrain.octaves = tuning.terrainOctaves;
    terrain.lacunarity = tuning.terrainLacunarity;
    terrain.gain = tuning.terrainGain;
    terrain.mountainWavelength = tuning.mountainWavelength;
    terrain.mountainAmplitude = tuning.mountainAmplitude;
    terrain.mountainMaskLow = tuning.mountainMaskLow;
    terrain.mountainMaskHigh = tuning.mountainMaskHigh;
    terrain.seaLevel = tuning.seaLevel;
    terrain.snowLine = activeSnowLine;
    terrain.treeLineFactor = tuning.treeLineFactor;
    // The view-radius cap is the engine's own constant, not Form metadata.
    r.terrainSystem().viewRadius =
        glm::clamp(tuning.terrainViewRadius, 8,
                   render::TerrainSystem::kMaxViewRadius);

    // The 1:1 knobs ride the lane tables (captureTuning walks the SAME
    // tables back). (tuning.ssaoStrength is unused — screen AO removed.)
    reflect::applyLanes(r.tuning, tuning, kTerrainFar, kSeasons,
                        kClusteredLights, kInteriorDaylight, kInteriorDust,
                        kFogTail, kSkyClouds, kSkyCloudLook, kMist,
                        kMistCover, kMistSun, kMistNoise, kMistDetail,
                        kExposure, kAutoExposure, kGrade);
    reflect::applyLanes(r.chunkOcclusion(), tuning, kOcclusion);
    reflect::applyLanes(r.postFxSystem(), tuning, kFroxelFog, kFroxel,
                        kMistBlend);
    reflect::applyLanes(r.grassRenderTuning(), tuning, kGrassBlade,
                        kGrassTints, kGrassShading, kGrassBrightness,
                        kGrassShadingTail, kGrassDistance);
    // Startup-only mapping: the scatter bake reads these on first
    // request, no regenerate needed (nothing is resident yet).
    reflect::applyLanes(r.grassScatterTuning(), tuning, kGrassScatter,
                        kGrassScatterWindows, kGrassScatterCutoff,
                        kGrassScatterTint);

    // Vec4 packing (the Form spells the lanes as scalars) and the knobs
    // clamped to their reflected range.
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
    r.tuning.shadowResolution = clampToRange(
        tuning.shadowResolution, rangeOf(&Tuning::shadowResolution));
    r.tuning.reflectionScale = clampToRange(
        tuning.reflectionScale, rangeOf(&Tuning::reflectionScale));
    r.tuning.mistSteps =
        clampToRange(tuning.mistSteps, rangeOf(&Tuning::mistSteps));
    // Vegetation draw budget (the streamer ring and the Hi-Z candidate
    // cap size the reflected ranges).
    render::VegetationSystem& vegetation = r.vegetationSystem();
    vegetation.viewRadius =
        clampToRange(tuning.vegViewRadius, rangeOf(&Tuning::vegViewRadius));
    vegetation.highDetailRadius = clampToRange(
        tuning.vegHighDetailRadius, rangeOf(&Tuning::vegHighDetailRadius));
    vegetation.lowDetailRadius = clampToRange(
        tuning.vegLowDetailRadius, rangeOf(&Tuning::vegLowDetailRadius));

    // Water sim window — startup values for the render panel's "Sim
    // window" knobs. Span/texel land before the first window init, so
    // no re-init dance here.
    render::WaterSystem::SimConfig& sim = r.waterSystem().simConfig();
    reflect::applyLanes(sim, tuning, kWaterSim);
    sim.span = clampToRange(tuning.waterSimSpan, rangeOf(&Tuning::waterSimSpan));
    sim.texel =
        clampToRange(tuning.waterSimTexel, rangeOf(&Tuning::waterSimTexel));
    sim.maxSubsteps = static_cast<u32>(clampToRange(
        tuning.waterSimMaxSubsteps, rangeOf(&Tuning::waterSimMaxSubsteps)));
    sim.timeScale = clampToRange(tuning.waterSimTimeScale,
                                 rangeOf(&Tuning::waterSimTimeScale));
    sim.params.rainRate = clampToRange(tuning.waterSimRainRate,
                                       rangeOf(&Tuning::waterSimRainRate));
    sim.fadeBand = clampToRange(tuning.waterSimFadeBand,
                                rangeOf(&Tuning::waterSimFadeBand));
    sim.pinRiverHalfWidth =
        clampToRange(tuning.waterSimPinRiverHalfWidth,
                     rangeOf(&Tuning::waterSimPinRiverHalfWidth));
    sim.params.reservoirOutflow =
        clampToRange(tuning.waterSimReservoirOutflow,
                     rangeOf(&Tuning::waterSimReservoirOutflow));
    sim.params.bakedDischargeBoost =
        clampToRange(tuning.waterSimBakedDischargeBoost,
                     rangeOf(&Tuning::waterSimBakedDischargeBoost));
}

void RenderTuningIo::applyAtmosphere(
    render::AtmosphereParams& atmos,
    const data::LandscapeTuningForm& tuning) {
    reflect::applyLanes(atmos, tuning, kSunFx, kFog, kFogStart,
                        kFogSunAndClouds, kCloudGeometry, kBloom);
}

void RenderTuningIo::applyTreeTuning(
    render::WorldRenderer& r, const data::LobeTreeTuningForm& lobes,
    const data::ColonizedTreeTuningForm& colonized) {
    r.vegetationSystem().lobeTreeParams = toLobeParams(lobes);
    r.vegetationSystem().colonizedTreeParams = toColonizedParams(colonized);
}

render::LobeTreeParams RenderTuningIo::toLobeParams(
    const data::LobeTreeTuningForm& lobes) {
    // Form -> flat engine params through the lane tables (§4: engine
    // never sees data/). The Trees panel then edits the params live.
    render::LobeTreeParams l;
    reflect::applyLanes(l, lobes, kLobeTrunk, kLobeBranchCount, kLobeCrown);
    return l;
}

render::ColonizedTreeParams RenderTuningIo::toColonizedParams(
    const data::ColonizedTreeTuningForm& colonized) {
    render::ColonizedTreeParams c;
    reflect::applyLanes(c, colonized, kColonizedGrowth, kColonizedAttractors,
                        kColonizedPipe, kColonizedTubeSides, kColonizedWood,
                        kColonizedCurveSubdiv, kColonizedWoodNoise,
                        kColonizedFlareLobes, kColonizedBark, kColonizedTints,
                        kColonizedCrown, kColonizedConifer,
                        kColonizedLeafStyle, kColonizedLeafShape,
                        kColonizedSeason, kColonizedFoliage,
                        kColonizedLeafCount, kColonizedLeafSize,
                        kColonizedLeafLod);
    return c;
}

void RenderTuningIo::applyRcTuning(render::WorldRenderer& r,
                                   const data::RcTuningForm& rc) {
    render::RcTuning& t = r.giTuning();
    reflect::applyLanes(t, rc, kRcResolution, kRcVoxels, kRcUpdateInterval,
                        kRcTileDrift, kRcGridNormals, kRcCascadeCount,
                        kRcInterval, kRcInjection, kRcScheduling, kRcApply,
                        kRcIntervalExtension);
    // The one non-lane field: the Form spells the technique as an i32.
    t.technique = rc.technique == 1 ? render::GiTechnique::RadianceCascades
                                    : render::GiTechnique::Classic;
}

void RenderTuningIo::captureTuning(const render::WorldRenderer& r,
                                   data::LandscapeTuningForm& out) {
    reflect::captureLanes(r.tuning, out, kTerrainFar, kSeasons,
                          kClusteredLights, kInteriorDaylight, kInteriorDust,
                          kFogTail, kSkyClouds, kSkyCloudLook, kMist,
                          kMistCover, kMistSun, kMistNoise, kMistDetail,
                          kExposure, kAutoExposure, kGrade);
    reflect::captureLanes(r.chunkOcclusion(), out, kOcclusion);
    reflect::captureLanes(r.postFxSystem(), out, kFroxelFog, kFroxel,
                          kMistBlend);
    reflect::captureLanes(r.grassRenderTuning(), out, kGrassBlade,
                          kGrassTints, kGrassShading, kGrassBrightness,
                          kGrassShadingTail, kGrassDistance);
    reflect::captureLanes(r.grassScatterTuning(), out, kGrassScatter,
                          kGrassScatterWindows, kGrassScatterCutoff,
                          kGrassScatterTint);

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
    out.terrainViewRadius = r.terrainSystem().viewRadius;
    out.vegViewRadius = r.vegetationSystem().viewRadius;
    out.vegHighDetailRadius = r.vegetationSystem().highDetailRadius;
    out.vegLowDetailRadius = r.vegetationSystem().lowDetailRadius;

    const render::WaterSystem::SimConfig& sim =
        r.waterSystem().simConfig();
    reflect::captureLanes(sim, out, kWaterSim);
    out.waterSimSpan = sim.span;
    out.waterSimTexel = sim.texel;
    out.waterSimMaxSubsteps = static_cast<i32>(sim.maxSubsteps);
    out.waterSimTimeScale = sim.timeScale;
    out.waterSimRainRate = sim.params.rainRate;
    out.waterSimFadeBand = sim.fadeBand;
    out.waterSimPinRiverHalfWidth = sim.pinRiverHalfWidth;
    out.waterSimReservoirOutflow = sim.params.reservoirOutflow;
    out.waterSimBakedDischargeBoost = sim.params.bakedDischargeBoost;
}

void RenderTuningIo::captureAtmosphere(
    const render::AtmosphereParams& atmos,
    data::LandscapeTuningForm& out) {
    reflect::captureLanes(atmos, out, kSunFx, kFog, kFogStart,
                          kFogSunAndClouds, kCloudGeometry, kBloom);
}

void RenderTuningIo::captureRcTuning(const render::WorldRenderer& r,
                                     data::RcTuningForm& out) {
    const render::RcTuning& t = r.giTuning();
    reflect::captureLanes(t, out, kRcResolution, kRcVoxels,
                          kRcUpdateInterval, kRcTileDrift, kRcGridNormals,
                          kRcCascadeCount, kRcInterval, kRcInjection,
                          kRcScheduling, kRcApply, kRcIntervalExtension);
    out.technique =
        t.technique == render::GiTechnique::RadianceCascades ? 1 : 0;
}

void RenderTuningIo::captureTreeTuning(
    const render::WorldRenderer& r, data::LobeTreeTuningForm& lobes,
    data::ColonizedTreeTuningForm& colonized) {
    const render::LobeTreeParams& l = r.vegetationSystem().lobeTreeParams;
    reflect::captureLanes(l, lobes, kLobeTrunk, kLobeBranchCount, kLobeCrown);
    const render::ColonizedTreeParams& c =
        r.vegetationSystem().colonizedTreeParams;
    reflect::captureLanes(c, colonized, kColonizedGrowth,
                          kColonizedAttractors, kColonizedPipe,
                          kColonizedTubeSides, kColonizedWood,
                          kColonizedCurveSubdiv, kColonizedWoodNoise,
                          kColonizedFlareLobes, kColonizedBark,
                          kColonizedTints, kColonizedCrown,
                          kColonizedConifer, kColonizedLeafStyle,
                          kColonizedLeafShape, kColonizedSeason,
                          kColonizedFoliage, kColonizedLeafCount,
                          kColonizedLeafSize, kColonizedLeafLod);
}

} // namespace game
