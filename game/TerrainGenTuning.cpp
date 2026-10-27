#include "game/TerrainGenTuning.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "engine/core/Hash.hpp"

#include <algorithm>

#include <glm/glm.hpp>

#include "data/forms/FormQuery.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Record.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Log.hpp"
#include "game/MapBaker.hpp"
#include "engine/reflect/FieldLane.hpp"
#include "engine/reflect/Fields.hpp"

namespace game {

using render::terraingen::TileBakeParams;

namespace {

namespace tg = render::terraingen;
using Tuning = data::TerrainGenTuningForm;
template <typename Params, typename T>
using Lane = reflect::FieldLane<Params, Tuning, T>;

// ONE line per pupitre field <-> bake parameter, walked in BOTH
// directions by applyTerrainGenTuning / captureTerrainGenTuning. What
// is not a plain member-to-member copy (the per-etage arrays, the
// macro tiers, the i32/u32 rampart steps, the fleuve area mirrored
// into the hydrology) stays hand-written in those two functions.
constexpr Lane<tg::WorldLayerParams, f32> kWorldLanesF32[] = {
    { &tg::WorldLayerParams::continentWavelength, &Tuning::worldContinentWavelength },
    { &tg::WorldLayerParams::continentRadius, &Tuning::worldContinentRadius },
    { &tg::WorldLayerParams::continentJitter, &Tuning::worldContinentJitter },
    { &tg::WorldLayerParams::continentShapeWavelength, &Tuning::worldContinentShapeWavelength },
    { &tg::WorldLayerParams::continentShapeAmp, &Tuning::worldContinentShapeAmp },
    { &tg::WorldLayerParams::continentChance, &Tuning::worldContinentChance },
    { &tg::WorldLayerParams::borderMassifMin, &Tuning::worldBorderMassifMin },
    { &tg::WorldLayerParams::seaThreshold, &Tuning::worldSeaThreshold },
    { &tg::WorldLayerParams::etageWavelength, &Tuning::worldEtageWavelength },
    { &tg::WorldLayerParams::massifWavelength, &Tuning::worldMassifWavelength },
    { &tg::WorldLayerParams::massifLift, &Tuning::worldMassifLift },
    { &tg::WorldLayerParams::benchWavelength, &Tuning::worldBenchWavelength },
    { &tg::WorldLayerParams::benchAmp, &Tuning::worldBenchAmp },
    { &tg::WorldLayerParams::plateauWavelength, &Tuning::worldPlateauWavelength },
    { &tg::WorldLayerParams::plateauStep, &Tuning::worldPlateauStep },
    { &tg::WorldLayerParams::plateauEdge, &Tuning::worldPlateauEdge },
    { &tg::WorldLayerParams::plateauStartGap, &Tuning::worldPlateauStartGap },
    { &tg::WorldLayerParams::startEtage, &Tuning::worldStartEtage },
    { &tg::WorldLayerParams::startRadius, &Tuning::worldStartRadius },
    { &tg::WorldLayerParams::startFade, &Tuning::worldStartFade },
    { &tg::WorldLayerParams::anchorRadius, &Tuning::worldAnchorRadius },
    { &tg::WorldLayerParams::anchorFade, &Tuning::worldAnchorFade },
    { &tg::WorldLayerParams::climateWavelength, &Tuning::worldClimateWavelength },
    { &tg::WorldLayerParams::lapsePerKm, &Tuning::worldLapsePerKm },
    { &tg::WorldLayerParams::coverWavelength, &Tuning::worldCoverWavelength },
    { &tg::WorldLayerParams::coverAmp, &Tuning::worldCoverAmp },
};
constexpr Lane<tg::WorldLayerParams, u32> kWorldLanesU32[] = {
    { &tg::WorldLayerParams::continentCellSupers, &Tuning::worldContinentCellSupers },
    { &tg::WorldLayerParams::plateauLevels, &Tuning::worldPlateauLevels },
};
constexpr Lane<tg::WorldLayerParams, bool> kWorldLanesBool[] = {
    { &tg::WorldLayerParams::borderFromWorld, &Tuning::worldBorderFromWorld },
};

constexpr Lane<tg::RhythmParams, f32> kRhythmLanesF32[] = {
    { &tg::RhythmParams::planStoryScale, &Tuning::rhythmPlanStoryScale },
    { &tg::RhythmParams::storyMountainWavelength, &Tuning::rhythmStoryMountainWavelength },
    { &tg::RhythmParams::storyMountainAmplitude, &Tuning::rhythmStoryMountainAmplitude },
    { &tg::RhythmParams::storyMountainMaskLow, &Tuning::rhythmStoryMountainMaskLow },
    { &tg::RhythmParams::storyMountainMaskHigh, &Tuning::rhythmStoryMountainMaskHigh },
    { &tg::RhythmParams::regimeWavelength, &Tuning::rhythmRegimeWavelength },
    { &tg::RhythmParams::regimeHillAmplitude, &Tuning::rhythmRegimeHillAmplitude },
    { &tg::RhythmParams::regimeMassifHeight, &Tuning::rhythmRegimeMassifHeight },
    { &tg::RhythmParams::calmBandWavelength, &Tuning::rhythmCalmBandWavelength },
    { &tg::RhythmParams::intimateCellSize, &Tuning::rhythmIntimateCellSize },
    { &tg::RhythmParams::intimateChance, &Tuning::rhythmIntimateChance },
    { &tg::RhythmParams::intimateHeightMin, &Tuning::rhythmIntimateHeightMin },
    { &tg::RhythmParams::intimateHeightMax, &Tuning::rhythmIntimateHeightMax },
    { &tg::RhythmParams::intimateRadiusMin, &Tuning::rhythmIntimateRadiusMin },
    { &tg::RhythmParams::intimateRadiusMax, &Tuning::rhythmIntimateRadiusMax },
    { &tg::RhythmParams::crestWavelength, &Tuning::rhythmCrestWavelength },
    { &tg::RhythmParams::bedWavelength, &Tuning::rhythmBedWavelength },
    { &tg::RhythmParams::colSpacing, &Tuning::rhythmColSpacing },
    { &tg::RhythmParams::hardnessWavelength, &Tuning::rhythmHardnessWavelength },
    { &tg::RhythmParams::ridgeColWavelength, &Tuning::rhythmRidgeColWavelength },
};
constexpr Lane<tg::RhythmParams, bool> kRhythmLanesBool[] = {
    { &tg::RhythmParams::plan, &Tuning::rhythmPlan },
    { &tg::RhythmParams::zones, &Tuning::rhythmZones },
};

constexpr Lane<tg::PoiPlanParams, f32> kPoiLanesF32[] = {
    { &tg::PoiPlanParams::moyenCell, &Tuning::poiMoyenCell },
    { &tg::PoiPlanParams::petitCell, &Tuning::poiPetitCell },
    { &tg::PoiPlanParams::petitChance, &Tuning::poiPetitChance },
    { &tg::PoiPlanParams::grandHeightMin, &Tuning::poiGrandHeightMin },
    { &tg::PoiPlanParams::grandHeightMax, &Tuning::poiGrandHeightMax },
    { &tg::PoiPlanParams::moyenHeightMin, &Tuning::poiMoyenHeightMin },
    { &tg::PoiPlanParams::moyenHeightMax, &Tuning::poiMoyenHeightMax },
    { &tg::PoiPlanParams::petitHeightMin, &Tuning::poiPetitHeightMin },
    { &tg::PoiPlanParams::petitHeightMax, &Tuning::poiPetitHeightMax },
    { &tg::PoiPlanParams::edgeReach, &Tuning::poiEdgeReach },
    { &tg::PoiPlanParams::waterPoiReach, &Tuning::poiWaterReach },
    { &tg::PoiPlanParams::grandStartClearance, &Tuning::poiGrandStartClearance },
    { &tg::PoiPlanParams::coneSlopeMinDeg, &Tuning::poiConeSlopeMinDeg },
    { &tg::PoiPlanParams::coneSlopeMaxDeg, &Tuning::poiConeSlopeMaxDeg },
    { &tg::PoiPlanParams::verticality, &Tuning::poiVerticality },
    { &tg::PoiPlanParams::coneSlopeSteepMinDeg, &Tuning::poiConeSlopeSteepMinDeg },
    { &tg::PoiPlanParams::coneSlopeSteepMaxDeg, &Tuning::poiConeSlopeSteepMaxDeg },
    { &tg::PoiPlanParams::corridorHalfWidthMin, &Tuning::poiCorridorHalfWidthMin },
    { &tg::PoiPlanParams::corridorHalfWidthMax, &Tuning::poiCorridorHalfWidthMax },
    { &tg::PoiPlanParams::screenHeightMin, &Tuning::poiScreenHeightMin },
    { &tg::PoiPlanParams::screenHeightMax, &Tuning::poiScreenHeightMax },
    { &tg::PoiPlanParams::screenNotch, &Tuning::poiScreenNotch },
};
constexpr Lane<tg::PoiPlanParams, u32> kPoiLanesU32[] = {
    { &tg::PoiPlanParams::edgeMax, &Tuning::poiEdgeMax },
};

constexpr Lane<tg::MacroParams, f32> kMacroLanesF32[] = {
    { &tg::MacroParams::terraceStep, &Tuning::macroTerraceStep },
    { &tg::MacroParams::terraceEdge, &Tuning::macroTerraceEdge },
    { &tg::MacroParams::cliffStep, &Tuning::macroCliffStep },
    { &tg::MacroParams::cliffEdge, &Tuning::macroCliffEdge },
    { &tg::MacroParams::warpWavelength, &Tuning::macroWarpWavelength },
    { &tg::MacroParams::warpStrength, &Tuning::macroWarpStrength },
    { &tg::MacroParams::valleyStretch, &Tuning::macroValleyStretch },
};
constexpr Lane<tg::MacroParams, i32> kMacroLanesI32[] = {
    { &tg::MacroParams::reliefOctaves, &Tuning::macroReliefOctaves },
};

constexpr Lane<TileBakeParams, f32> kBakeLanesF32[] = {
    { &TileBakeParams::macroTexel, &Tuning::bakeMacroTexel },
    { &TileBakeParams::calmCut, &Tuning::bakeCalmCut },
    { &TileBakeParams::roughCut, &Tuning::bakeRoughCut },
    { &TileBakeParams::dimpleFillMax, &Tuning::bakeDimpleFillMax },
    { &TileBakeParams::keepCrestFade, &Tuning::bakeKeepCrestFade },
};
constexpr Lane<tg::FluvialParams, f32> kFluvialLanesF32[] = {
    { &tg::FluvialParams::k, &Tuning::bakeFluvialK },
    { &tg::FluvialParams::upliftRate, &Tuning::bakeUpliftRate },
};
constexpr Lane<tg::FluvialParams, i32> kFluvialLanesI32[] = {
    { &tg::FluvialParams::iterations, &Tuning::bakeFluvialIterations },
};
constexpr Lane<tg::ThermalParams, f32> kThermalLanesF32[] = {
    { &tg::ThermalParams::talusTan, &Tuning::bakeTalusTan },
};
constexpr Lane<tg::ThermalParams, i32> kThermalLanesI32[] = {
    { &tg::ThermalParams::iterations, &Tuning::bakeThermalIterations },
};
constexpr Lane<tg::RidgeRoundParams, f32> kRoundingLanesF32[] = {
    { &tg::RidgeRoundParams::strength, &Tuning::bakeRoundingStrength },
};

constexpr Lane<tg::ZoneParams, f32> kZoneLanesF32[] = {
    { &tg::ZoneParams::cellSize, &Tuning::zoneCellSize },
    { &tg::ZoneParams::jitter, &Tuning::zoneJitter },
    { &tg::ZoneParams::borderWarp, &Tuning::zoneBorderWarp },
    { &tg::ZoneParams::borderWarpWavelength, &Tuning::zoneBorderWarpWavelength },
    { &tg::ZoneParams::stepHeight, &Tuning::zoneStepHeight },
    { &tg::ZoneParams::trendWavelength, &Tuning::zoneTrendWavelength },
    { &tg::ZoneParams::trendContrast, &Tuning::zoneTrendContrast },
    { &tg::ZoneParams::wallWidthOne, &Tuning::zoneWallWidthOne },
    { &tg::ZoneParams::wallWidthHigh, &Tuning::zoneWallWidthHigh },
    { &tg::ZoneParams::rampWidth, &Tuning::zoneRampWidth },
    { &tg::ZoneParams::startGap, &Tuning::zoneStartGap },
    { &tg::ZoneParams::gateHalfWidth, &Tuning::zoneGateHalfWidth },
    { &tg::ZoneParams::gateExtra, &Tuning::zoneGateExtra },
    { &tg::ZoneParams::ridgeHeight, &Tuning::zoneRidgeHeight },
};
constexpr Lane<tg::ZoneParams, u32> kZoneLanesU32[] = {
    { &tg::ZoneParams::storeys, &Tuning::zoneStoreys },
};

constexpr Lane<tg::MasterNetworkParams, f32> kNetworkLanesF32[] = {
    { &tg::MasterNetworkParams::fleuveArea, &Tuning::networkFleuveArea },
    { &tg::MasterNetworkParams::riverExtractArea, &Tuning::networkRiverExtractArea },
    { &tg::MasterNetworkParams::texel, &Tuning::networkTexel },
    { &tg::MasterNetworkParams::runoffDryMin, &Tuning::hydroRunoffDryMin },
    { &tg::MasterNetworkParams::runoffWetMax, &Tuning::hydroRunoffWetMax },
    { &tg::MasterNetworkParams::snowBonus, &Tuning::hydroSnowBonus },
    { &tg::MasterNetworkParams::snowlineTemp, &Tuning::hydroSnowlineTemp },
    { &tg::MasterNetworkParams::lakeDepth, &Tuning::networkLakeDepth },
    { &tg::MasterNetworkParams::routeDither, &Tuning::networkRouteDither },
    { &tg::MasterNetworkParams::glacierTemp, &Tuning::hydroGlacierTemp },
    { &tg::MasterNetworkParams::neveTemp, &Tuning::hydroNeveTemp },
};
constexpr Lane<tg::MasterNetworkParams, bool> kNetworkLanesBool[] = {
    { &tg::MasterNetworkParams::routeOnShaped, &Tuning::networkRouteOnShaped },
    { &tg::MasterNetworkParams::runoffWeights, &Tuning::hydroRunoffWeights },
};

constexpr Lane<tg::HydrologyParams, f32> kHydrologyLanesF32[] = {
    { &tg::HydrologyParams::fleuveRampLow, &Tuning::hydroFleuveRampLow },
    { &tg::HydrologyParams::fleuveRampHigh, &Tuning::hydroFleuveRampHigh },
    { &tg::HydrologyParams::tier2MinHalfWidth, &Tuning::hydroTier2MinHalfWidth },
    { &tg::HydrologyParams::riverArea, &Tuning::hydroRiverArea },
    { &tg::HydrologyParams::riviereArea, &Tuning::hydroRiviereArea },
    { &tg::HydrologyParams::widthCoef, &Tuning::hydroWidthCoef },
    { &tg::HydrologyParams::widthExponent, &Tuning::hydroWidthExponent },
    { &tg::HydrologyParams::fleuveWidthScale, &Tuning::hydroFleuveWidthScale },
    { &tg::HydrologyParams::minLakeDepth, &Tuning::hydroMinLakeDepth },
    { &tg::HydrologyParams::minLakeArea, &Tuning::hydroMinLakeArea },
    { &tg::HydrologyParams::mouthFlare, &Tuning::hydroMouthFlare },
    { &tg::HydrologyParams::mouthFlareLength, &Tuning::hydroMouthFlareLength },
    { &tg::HydrologyParams::lakeKeepDepth, &Tuning::hydroLakeKeepDepth },
    { &tg::HydrologyParams::lakeDeepPierce, &Tuning::hydroLakeDeepPierce },
    { &tg::HydrologyParams::lakeKeepArea, &Tuning::hydroLakeKeepArea },
};
constexpr Lane<tg::HydrologyParams, bool> kHydrologyLanesBool[] = {
    { &tg::HydrologyParams::lakePiercing, &Tuning::hydroLakePiercing },
};

constexpr Lane<tg::MasterImprintParams, f32> kImprintLanesF32[] = {
    { &tg::MasterImprintParams::bedDepth, &Tuning::imprintBedDepth },
    { &tg::MasterImprintParams::plainFactor, &Tuning::imprintPlainFactor },
    { &tg::MasterImprintParams::minGradient, &Tuning::imprintMinGradient },
};
constexpr Lane<tg::WaterSolveParams, f32> kWaterSolveLanesF32[] = {
    { &tg::WaterSolveParams::rainRate, &Tuning::waterRainRate },
};

} // namespace

// The lane tables above are the ONE place a pupitre field meets its
// bake parameter; only the copies a member pointer cannot express are
// spelled out here (and mirrored in captureTerrainGenTuning). The
// tiers table has four rows by construction.
void applyTerrainGenTuning(const data::TerrainGenTuningForm& form,
                           TileBakeParams& params) {
    if (params.macro.tiers.size() < 4) {
        params.macro.tiers.resize(4);
    }
    reflect::applyLanes(params.controls.world, form, kWorldLanesF32);
    reflect::applyLanes(params.controls.world, form, kWorldLanesU32);
    reflect::applyLanes(params.controls.world, form, kWorldLanesBool);
    reflect::applyLanes(params.controls.rhythm, form, kRhythmLanesF32);
    reflect::applyLanes(params.controls.rhythm, form, kRhythmLanesBool);
    params.controls.rhythm.crestAmplitudeByEtage[0] = form.rhythmCrestAmplitudeEtage0;
    params.controls.rhythm.crestAmplitudeByEtage[1] = form.rhythmCrestAmplitudeEtage1;
    params.controls.rhythm.crestAmplitudeByEtage[2] = form.rhythmCrestAmplitudeEtage2;
    params.controls.rhythm.crestAmplitudeByEtage[3] = form.rhythmCrestAmplitudeEtage3;
    params.controls.rhythm.bedDepthByEtage[0] = form.rhythmBedDepthEtage0;
    params.controls.rhythm.bedDepthByEtage[1] = form.rhythmBedDepthEtage1;
    params.controls.rhythm.bedDepthByEtage[2] = form.rhythmBedDepthEtage2;
    params.controls.rhythm.bedDepthByEtage[3] = form.rhythmBedDepthEtage3;
    reflect::applyLanes(params.controls.poi, form, kPoiLanesF32);
    reflect::applyLanes(params.controls.poi, form, kPoiLanesU32);
    reflect::applyLanes(params.macro, form, kMacroLanesF32);
    reflect::applyLanes(params.macro, form, kMacroLanesI32);
    params.macro.tiers[0].reliefAmplitude = form.macroReliefAmplitudeEtage0;
    params.macro.tiers[1].reliefAmplitude = form.macroReliefAmplitudeEtage1;
    params.macro.tiers[2].reliefAmplitude = form.macroReliefAmplitudeEtage2;
    params.macro.tiers[3].reliefAmplitude = form.macroReliefAmplitudeEtage3;
    params.macro.tiers[0].reliefWavelength = form.macroReliefWavelengthEtage0;
    params.macro.tiers[1].reliefWavelength = form.macroReliefWavelengthEtage1;
    params.macro.tiers[2].reliefWavelength = form.macroReliefWavelengthEtage2;
    params.macro.tiers[3].reliefWavelength = form.macroReliefWavelengthEtage3;
    params.macro.tiers[2].terrace = form.macroPlateauTerrace;
    reflect::applyLanes(params, form, kBakeLanesF32);
    reflect::applyLanes(params.fluvial, form, kFluvialLanesF32);
    reflect::applyLanes(params.fluvial, form, kFluvialLanesI32);
    reflect::applyLanes(params.thermal, form, kThermalLanesF32);
    reflect::applyLanes(params.thermal, form, kThermalLanesI32);
    reflect::applyLanes(params.rounding, form, kRoundingLanesF32);
    reflect::applyLanes(params.controls.zones, form, kZoneLanesF32);
    reflect::applyLanes(params.controls.zones, form, kZoneLanesU32);
    params.controls.zones.rampartSteps =
        static_cast<i32>(form.zoneRampartSteps);
    reflect::applyLanes(params.network, form, kNetworkLanesF32);
    reflect::applyLanes(params.network, form, kNetworkLanesBool);
    reflect::applyLanes(params.hydrology, form, kHydrologyLanesF32);
    reflect::applyLanes(params.hydrology, form, kHydrologyLanesBool);
    // The tier-2 cut of classifyRivers mirrors the network's.
    params.hydrology.fleuveArea = form.networkFleuveArea;
    reflect::applyLanes(params.imprint, form, kImprintLanesF32);
    reflect::applyLanes(params.waterSolve, form, kWaterSolveLanesF32);
}

data::TerrainGenTuningForm
captureTerrainGenTuning(const TileBakeParams& params) {
    data::TerrainGenTuningForm form;
    if (params.macro.tiers.size() < 4) {
        return form;
    }
    reflect::captureLanes(params.controls.world, form, kWorldLanesF32);
    reflect::captureLanes(params.controls.world, form, kWorldLanesU32);
    reflect::captureLanes(params.controls.world, form, kWorldLanesBool);
    reflect::captureLanes(params.controls.rhythm, form, kRhythmLanesF32);
    reflect::captureLanes(params.controls.rhythm, form, kRhythmLanesBool);
    form.rhythmCrestAmplitudeEtage0 = params.controls.rhythm.crestAmplitudeByEtage[0];
    form.rhythmCrestAmplitudeEtage1 = params.controls.rhythm.crestAmplitudeByEtage[1];
    form.rhythmCrestAmplitudeEtage2 = params.controls.rhythm.crestAmplitudeByEtage[2];
    form.rhythmCrestAmplitudeEtage3 = params.controls.rhythm.crestAmplitudeByEtage[3];
    form.rhythmBedDepthEtage0 = params.controls.rhythm.bedDepthByEtage[0];
    form.rhythmBedDepthEtage1 = params.controls.rhythm.bedDepthByEtage[1];
    form.rhythmBedDepthEtage2 = params.controls.rhythm.bedDepthByEtage[2];
    form.rhythmBedDepthEtage3 = params.controls.rhythm.bedDepthByEtage[3];
    reflect::captureLanes(params.controls.poi, form, kPoiLanesF32);
    reflect::captureLanes(params.controls.poi, form, kPoiLanesU32);
    reflect::captureLanes(params.macro, form, kMacroLanesF32);
    reflect::captureLanes(params.macro, form, kMacroLanesI32);
    form.macroReliefAmplitudeEtage0 = params.macro.tiers[0].reliefAmplitude;
    form.macroReliefAmplitudeEtage1 = params.macro.tiers[1].reliefAmplitude;
    form.macroReliefAmplitudeEtage2 = params.macro.tiers[2].reliefAmplitude;
    form.macroReliefAmplitudeEtage3 = params.macro.tiers[3].reliefAmplitude;
    form.macroReliefWavelengthEtage0 = params.macro.tiers[0].reliefWavelength;
    form.macroReliefWavelengthEtage1 = params.macro.tiers[1].reliefWavelength;
    form.macroReliefWavelengthEtage2 = params.macro.tiers[2].reliefWavelength;
    form.macroReliefWavelengthEtage3 = params.macro.tiers[3].reliefWavelength;
    form.macroPlateauTerrace = params.macro.tiers[2].terrace;
    reflect::captureLanes(params, form, kBakeLanesF32);
    reflect::captureLanes(params.fluvial, form, kFluvialLanesF32);
    reflect::captureLanes(params.fluvial, form, kFluvialLanesI32);
    reflect::captureLanes(params.thermal, form, kThermalLanesF32);
    reflect::captureLanes(params.thermal, form, kThermalLanesI32);
    reflect::captureLanes(params.rounding, form, kRoundingLanesF32);
    reflect::captureLanes(params.controls.zones, form, kZoneLanesF32);
    reflect::captureLanes(params.controls.zones, form, kZoneLanesU32);
    form.zoneRampartSteps =
        static_cast<u32>(glm::max(params.controls.zones.rampartSteps, 0));
    // networkFleuveArea reads the network's copy; the hydrology mirror
    // is write-only.
    reflect::captureLanes(params.network, form, kNetworkLanesF32);
    reflect::captureLanes(params.network, form, kNetworkLanesBool);
    reflect::captureLanes(params.hydrology, form, kHydrologyLanesF32);
    reflect::captureLanes(params.hydrology, form, kHydrologyLanesBool);
    reflect::captureLanes(params.imprint, form, kImprintLanesF32);
    reflect::captureLanes(params.waterSolve, form, kWaterSolveLanesF32);
    return form;
}

vector<render::terraingen::ZoneArchetype>
resolveZoneArchetypes(const data::FormDatabase& forms) {
    struct Row {
        u32 rank;
        render::terraingen::ZoneArchetype a;
    };
    vector<Row> rows;
    data::forEach<data::ZoneArchetypeForm>(
        forms, [&](const data::ZoneArchetypeForm& f) {
            render::terraingen::ZoneArchetype a;
            a.name = f.name;
            a.weight = f.weight;
            a.minEtage = f.minEtage;
            a.maxEtage = f.maxEtage;
            a.minMassif = f.minMassif;
            a.maxMassif = f.maxMassif;
            a.minCoast = f.minCoast;
            a.maxCoast = f.maxCoast;
            a.minMoisture = f.minMoisture;
            a.maxMoisture = f.maxMoisture;
            a.storeyBias = f.storeyBias;
            a.reliefMul = f.reliefMul;
            a.wavelengthMul = f.wavelengthMul;
            a.terrace = f.terrace;
            a.cliffStep = f.cliffStep;
            a.hillCrests = f.hillCrests;
            a.hardBias = f.hardBias;
            a.wetBias = f.wetBias;
            a.coverBias = f.coverBias;
            a.palette = f.palette;
            a.piece = f.piece;
            a.pieceHeight = f.pieceHeight;
            a.pieceRadius = f.pieceRadius;
            a.erosionCut = f.erosionCut;
            rows.push_back({ f.rank, std::move(a) });
        });
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& l, const Row& r) { return l.rank < r.rank; });
    vector<render::terraingen::ZoneArchetype> out;
    out.reserve(rows.size());
    for (Row& row : rows) {
        out.push_back(std::move(row.a));
    }
    return out;
}

TileBakeParams makeTerrainBakeParams(const data::LandscapeTuningForm& tuning,
                                     const data::TerrainGenTuningForm& gen,
                                     const data::FormDatabase* forms) {
    TileBakeParams params;
    params.worldSeed = tuning.terrainSeed;
    params.controls.seed = tuning.terrainSeed;
    params.macro.seaLevel = tuning.seaLevel;
    params.macro.recurveLow = tuning.terrainRecurveLow;
    params.macro.recurveMid = tuning.terrainRecurveMid;
    params.macro.recurveHigh = tuning.terrainRecurveHigh;
    // Border transitions on, the spec complete from here (bakeMap and
    // the streamer re-fill it from their own map config either way;
    // the master network routes on it — E3).
    params.mapGrid.valid = true;
    params.mapGrid.seed = tuning.terrainSeed;
    params.mapGrid.mapSize =
        params.tileSize * static_cast<f32>(kMapTilesPerSide);
    params.mapGrid.seaLevel = tuning.seaLevel;
    // The zones read the map lattice (their rampart rule): the same
    // size bakeMap puts in the grid spec.
    params.controls.zones.mapSize =
        params.tileSize * static_cast<f32>(kMapTilesPerSide);
    applyTerrainGenTuning(gen, params);
    if (forms) {
        // The archetype table of the zones (records, moddable); none =
        // the C++ default table.
        params.controls.zones.archetypes = resolveZoneArchetypes(*forms);
    }
    return params;
}

bool saveTerrainGenTuning(const data::TerrainGenTuningForm& form,
                          const std::filesystem::path& path,
                          const data::FormTypeRegistry& types,
                          const char* pluginName) {
    data::Plugin plugin;
    // One plugin id per file name: presets are distinct plugins, the
    // overlay keeps its own (stable across saves).
    const u32 nameHash = core::fnv1a(pluginName);
    char guidText[40];
    std::snprintf(guidText, sizeof(guidText),
                  "aaaaaaaa-0000-4000-8000-0000%08x", nameHash);
    if (const auto id = core::Guid::fromString(guidText)) {
        plugin.id = *id;
    }
    plugin.name = pluginName;
    data::Record record;
    record.formId = data::terrainGenTuningGuid();
    record.typeId = data::TerrainGenTuningForm::staticTypeInfo().id;
    record.creates = false;
    reflect::forEachField(
        data::TerrainGenTuningForm::staticTypeInfo(),
        [&](const reflect::FieldInfo& field) {
            if ((field.flags & reflect::Transient) != 0) {
                return;
            }
            record.fields[field.id] = field.get(&form);
        });
    plugin.records.push_back(std::move(record));
    std::error_code errc;
    std::filesystem::create_directories(path.parent_path(), errc);
    std::ofstream file { path, std::ios::trunc };
    if (!file) {
        LOG_ERROR("Terrain gen tuning: cannot write {}", path.string());
        return false;
    }
    file << data::writePluginToml(plugin, types);
    LOG_INFO("Terrain gen tuning saved -> {}", path.string());
    return true;
}

bool loadTerrainGenTuning(const std::filesystem::path& path,
                          const data::FormTypeRegistry& types,
                          data::TerrainGenTuningForm& out) {
    const auto loaded = data::loadPluginFile(path, types);
    if (!loaded) {
        LOG_ERROR("Terrain gen tuning: cannot read {}", path.string());
        return false;
    }
    const reflect::TypeInfo& type =
        data::TerrainGenTuningForm::staticTypeInfo();
    bool found = false;
    for (const data::Record& record : loaded->records) {
        if (record.formId != data::terrainGenTuningGuid()) {
            continue;
        }
        found = true;
        for (const auto& [fieldId, value] : record.fields) {
            if (const reflect::FieldInfo* field = type.findField(fieldId)) {
                field->set(&out, value);
            }
        }
    }
    if (!found) {
        LOG_WARN("Terrain gen tuning: no pupitre record in {}",
                 path.string());
    }
    return found;
}

} // namespace game
