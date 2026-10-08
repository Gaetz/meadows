#include "game/TerrainGenTuning.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "engine/core/Hash.hpp"

#include <algorithm>

#include "data/forms/FormQuery.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Record.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Log.hpp"
#include "engine/reflect/Visit.hpp"

namespace game {

using render::terraingen::TileBakeParams;

// The ONE place a pupitre field meets its bake parameter (both
// directions, same order): add a knob = one line in each list plus the
// Form field. The tiers table has four rows by construction.
void applyTerrainGenTuning(const data::TerrainGenTuningForm& form,
                           TileBakeParams& params) {
    if (params.macro.tiers.size() < 4) {
        params.macro.tiers.resize(4);
    }
    params.controls.world.continentWavelength = form.worldContinentWavelength;
    params.controls.world.seaThreshold = form.worldSeaThreshold;
    params.controls.world.etageWavelength = form.worldEtageWavelength;
    params.controls.world.massifWavelength = form.worldMassifWavelength;
    params.controls.world.massifLift = form.worldMassifLift;
    params.controls.world.benchWavelength = form.worldBenchWavelength;
    params.controls.world.benchAmp = form.worldBenchAmp;
    params.controls.world.plateauWavelength = form.worldPlateauWavelength;
    params.controls.world.plateauStep = form.worldPlateauStep;
    params.controls.world.plateauLevels = form.worldPlateauLevels;
    params.controls.world.plateauEdge = form.worldPlateauEdge;
    params.controls.world.plateauStartGap = form.worldPlateauStartGap;
    params.controls.world.startEtage = form.worldStartEtage;
    params.controls.world.startRadius = form.worldStartRadius;
    params.controls.world.startFade = form.worldStartFade;
    params.controls.world.startLowRadius = form.worldStartLowRadius;
    params.controls.world.startLowFade = form.worldStartLowFade;
    params.controls.world.anchorRadius = form.worldAnchorRadius;
    params.controls.world.anchorFade = form.worldAnchorFade;
    params.controls.world.climateWavelength = form.worldClimateWavelength;
    params.controls.world.lapsePerKm = form.worldLapsePerKm;
    params.controls.world.coverWavelength = form.worldCoverWavelength;
    params.controls.world.coverAmp = form.worldCoverAmp;
    params.controls.rhythm.plan = form.rhythmPlan;
    params.controls.rhythm.planStoryScale = form.rhythmPlanStoryScale;
    params.controls.rhythm.storyMountainWavelength = form.rhythmStoryMountainWavelength;
    params.controls.rhythm.storyMountainAmplitude = form.rhythmStoryMountainAmplitude;
    params.controls.rhythm.storyMountainMaskLow = form.rhythmStoryMountainMaskLow;
    params.controls.rhythm.storyMountainMaskHigh = form.rhythmStoryMountainMaskHigh;
    params.controls.rhythm.regimeWavelength = form.rhythmRegimeWavelength;
    params.controls.rhythm.regimeHillAmplitude = form.rhythmRegimeHillAmplitude;
    params.controls.rhythm.regimeMassifHeight = form.rhythmRegimeMassifHeight;
    params.controls.rhythm.calmBandWavelength = form.rhythmCalmBandWavelength;
    params.controls.rhythm.intimateCellSize = form.rhythmIntimateCellSize;
    params.controls.rhythm.intimateChance = form.rhythmIntimateChance;
    params.controls.rhythm.intimateHeightMin = form.rhythmIntimateHeightMin;
    params.controls.rhythm.intimateHeightMax = form.rhythmIntimateHeightMax;
    params.controls.rhythm.intimateRadiusMin = form.rhythmIntimateRadiusMin;
    params.controls.rhythm.intimateRadiusMax = form.rhythmIntimateRadiusMax;
    params.controls.rhythm.crestWavelength = form.rhythmCrestWavelength;
    params.controls.rhythm.crestAmplitudeByEtage[0] = form.rhythmCrestAmplitudeEtage0;
    params.controls.rhythm.crestAmplitudeByEtage[1] = form.rhythmCrestAmplitudeEtage1;
    params.controls.rhythm.crestAmplitudeByEtage[2] = form.rhythmCrestAmplitudeEtage2;
    params.controls.rhythm.crestAmplitudeByEtage[3] = form.rhythmCrestAmplitudeEtage3;
    params.controls.rhythm.bedWavelength = form.rhythmBedWavelength;
    params.controls.rhythm.bedDepthByEtage[0] = form.rhythmBedDepthEtage0;
    params.controls.rhythm.bedDepthByEtage[1] = form.rhythmBedDepthEtage1;
    params.controls.rhythm.bedDepthByEtage[2] = form.rhythmBedDepthEtage2;
    params.controls.rhythm.bedDepthByEtage[3] = form.rhythmBedDepthEtage3;
    params.controls.rhythm.colSpacing = form.rhythmColSpacing;
    params.controls.rhythm.hardnessWavelength = form.rhythmHardnessWavelength;
    params.controls.rhythm.ridgeColWavelength = form.rhythmRidgeColWavelength;
    params.controls.poi.moyenCell = form.poiMoyenCell;
    params.controls.poi.petitCell = form.poiPetitCell;
    params.controls.poi.petitChance = form.poiPetitChance;
    params.controls.poi.grandHeightMin = form.poiGrandHeightMin;
    params.controls.poi.grandHeightMax = form.poiGrandHeightMax;
    params.controls.poi.moyenHeightMin = form.poiMoyenHeightMin;
    params.controls.poi.moyenHeightMax = form.poiMoyenHeightMax;
    params.controls.poi.petitHeightMin = form.poiPetitHeightMin;
    params.controls.poi.petitHeightMax = form.poiPetitHeightMax;
    params.controls.poi.edgeReach = form.poiEdgeReach;
    params.controls.poi.edgeMax = form.poiEdgeMax;
    params.controls.poi.waterPoiReach = form.poiWaterReach;
    params.controls.poi.grandStartClearance = form.poiGrandStartClearance;
    params.controls.poi.coneSlopeMinDeg = form.poiConeSlopeMinDeg;
    params.controls.poi.coneSlopeMaxDeg = form.poiConeSlopeMaxDeg;
    params.controls.poi.verticality = form.poiVerticality;
    params.controls.poi.coneSlopeSteepMinDeg = form.poiConeSlopeSteepMinDeg;
    params.controls.poi.coneSlopeSteepMaxDeg = form.poiConeSlopeSteepMaxDeg;
    params.controls.poi.corridorHalfWidthMin = form.poiCorridorHalfWidthMin;
    params.controls.poi.corridorHalfWidthMax = form.poiCorridorHalfWidthMax;
    params.controls.poi.screenHeightMin = form.poiScreenHeightMin;
    params.controls.poi.screenHeightMax = form.poiScreenHeightMax;
    params.controls.poi.screenNotch = form.poiScreenNotch;
    params.macro.reliefOctaves = form.macroReliefOctaves;
    params.macro.tiers[0].reliefAmplitude = form.macroReliefAmplitudeEtage0;
    params.macro.tiers[1].reliefAmplitude = form.macroReliefAmplitudeEtage1;
    params.macro.tiers[2].reliefAmplitude = form.macroReliefAmplitudeEtage2;
    params.macro.tiers[3].reliefAmplitude = form.macroReliefAmplitudeEtage3;
    params.macro.tiers[0].reliefWavelength = form.macroReliefWavelengthEtage0;
    params.macro.tiers[1].reliefWavelength = form.macroReliefWavelengthEtage1;
    params.macro.tiers[2].reliefWavelength = form.macroReliefWavelengthEtage2;
    params.macro.tiers[3].reliefWavelength = form.macroReliefWavelengthEtage3;
    params.macro.tiers[2].terrace = form.macroPlateauTerrace;
    params.macro.terraceStep = form.macroTerraceStep;
    params.macro.terraceEdge = form.macroTerraceEdge;
    params.macro.cliffStep = form.macroCliffStep;
    params.macro.cliffEdge = form.macroCliffEdge;
    params.macro.warpWavelength = form.macroWarpWavelength;
    params.macro.warpStrength = form.macroWarpStrength;
    params.macro.valleyStretch = form.macroValleyStretch;
    params.macroTexel = form.bakeMacroTexel;
    params.calmCut = form.bakeCalmCut;
    params.roughCut = form.bakeRoughCut;
    params.dimpleFillMax = form.bakeDimpleFillMax;
    params.keepCrestFade = form.bakeKeepCrestFade;
    params.fluvial.iterations = form.bakeFluvialIterations;
    params.fluvial.k = form.bakeFluvialK;
    params.fluvial.upliftRate = form.bakeUpliftRate;
    params.thermal.iterations = form.bakeThermalIterations;
    params.thermal.talusTan = form.bakeTalusTan;
    params.rounding.strength = form.bakeRoundingStrength;
    params.controls.rhythm.zones = form.rhythmZones;
    params.controls.zones.cellSize = form.zoneCellSize;
    params.controls.zones.jitter = form.zoneJitter;
    params.controls.zones.borderWarp = form.zoneBorderWarp;
    params.controls.zones.borderWarpWavelength = form.zoneBorderWarpWavelength;
    params.controls.zones.stepHeight = form.zoneStepHeight;
    params.controls.zones.storeys = form.zoneStoreys;
    params.controls.zones.trendWavelength = form.zoneTrendWavelength;
    params.controls.zones.trendContrast = form.zoneTrendContrast;
    params.controls.zones.wallWidthOne = form.zoneWallWidthOne;
    params.controls.zones.wallWidthHigh = form.zoneWallWidthHigh;
    params.controls.zones.rampWidth = form.zoneRampWidth;
    params.controls.zones.startGap = form.zoneStartGap;
}

data::TerrainGenTuningForm
captureTerrainGenTuning(const TileBakeParams& params) {
    data::TerrainGenTuningForm form;
    if (params.macro.tiers.size() < 4) {
        return form;
    }
    form.worldContinentWavelength = params.controls.world.continentWavelength;
    form.worldSeaThreshold = params.controls.world.seaThreshold;
    form.worldEtageWavelength = params.controls.world.etageWavelength;
    form.worldMassifWavelength = params.controls.world.massifWavelength;
    form.worldMassifLift = params.controls.world.massifLift;
    form.worldBenchWavelength = params.controls.world.benchWavelength;
    form.worldBenchAmp = params.controls.world.benchAmp;
    form.worldPlateauWavelength = params.controls.world.plateauWavelength;
    form.worldPlateauStep = params.controls.world.plateauStep;
    form.worldPlateauLevels = params.controls.world.plateauLevels;
    form.worldPlateauEdge = params.controls.world.plateauEdge;
    form.worldPlateauStartGap = params.controls.world.plateauStartGap;
    form.worldStartEtage = params.controls.world.startEtage;
    form.worldStartRadius = params.controls.world.startRadius;
    form.worldStartFade = params.controls.world.startFade;
    form.worldStartLowRadius = params.controls.world.startLowRadius;
    form.worldStartLowFade = params.controls.world.startLowFade;
    form.worldAnchorRadius = params.controls.world.anchorRadius;
    form.worldAnchorFade = params.controls.world.anchorFade;
    form.worldClimateWavelength = params.controls.world.climateWavelength;
    form.worldLapsePerKm = params.controls.world.lapsePerKm;
    form.worldCoverWavelength = params.controls.world.coverWavelength;
    form.worldCoverAmp = params.controls.world.coverAmp;
    form.rhythmPlan = params.controls.rhythm.plan;
    form.rhythmPlanStoryScale = params.controls.rhythm.planStoryScale;
    form.rhythmStoryMountainWavelength = params.controls.rhythm.storyMountainWavelength;
    form.rhythmStoryMountainAmplitude = params.controls.rhythm.storyMountainAmplitude;
    form.rhythmStoryMountainMaskLow = params.controls.rhythm.storyMountainMaskLow;
    form.rhythmStoryMountainMaskHigh = params.controls.rhythm.storyMountainMaskHigh;
    form.rhythmRegimeWavelength = params.controls.rhythm.regimeWavelength;
    form.rhythmRegimeHillAmplitude = params.controls.rhythm.regimeHillAmplitude;
    form.rhythmRegimeMassifHeight = params.controls.rhythm.regimeMassifHeight;
    form.rhythmCalmBandWavelength = params.controls.rhythm.calmBandWavelength;
    form.rhythmIntimateCellSize = params.controls.rhythm.intimateCellSize;
    form.rhythmIntimateChance = params.controls.rhythm.intimateChance;
    form.rhythmIntimateHeightMin = params.controls.rhythm.intimateHeightMin;
    form.rhythmIntimateHeightMax = params.controls.rhythm.intimateHeightMax;
    form.rhythmIntimateRadiusMin = params.controls.rhythm.intimateRadiusMin;
    form.rhythmIntimateRadiusMax = params.controls.rhythm.intimateRadiusMax;
    form.rhythmCrestWavelength = params.controls.rhythm.crestWavelength;
    form.rhythmCrestAmplitudeEtage0 = params.controls.rhythm.crestAmplitudeByEtage[0];
    form.rhythmCrestAmplitudeEtage1 = params.controls.rhythm.crestAmplitudeByEtage[1];
    form.rhythmCrestAmplitudeEtage2 = params.controls.rhythm.crestAmplitudeByEtage[2];
    form.rhythmCrestAmplitudeEtage3 = params.controls.rhythm.crestAmplitudeByEtage[3];
    form.rhythmBedWavelength = params.controls.rhythm.bedWavelength;
    form.rhythmBedDepthEtage0 = params.controls.rhythm.bedDepthByEtage[0];
    form.rhythmBedDepthEtage1 = params.controls.rhythm.bedDepthByEtage[1];
    form.rhythmBedDepthEtage2 = params.controls.rhythm.bedDepthByEtage[2];
    form.rhythmBedDepthEtage3 = params.controls.rhythm.bedDepthByEtage[3];
    form.rhythmColSpacing = params.controls.rhythm.colSpacing;
    form.rhythmHardnessWavelength = params.controls.rhythm.hardnessWavelength;
    form.rhythmRidgeColWavelength = params.controls.rhythm.ridgeColWavelength;
    form.poiMoyenCell = params.controls.poi.moyenCell;
    form.poiPetitCell = params.controls.poi.petitCell;
    form.poiPetitChance = params.controls.poi.petitChance;
    form.poiGrandHeightMin = params.controls.poi.grandHeightMin;
    form.poiGrandHeightMax = params.controls.poi.grandHeightMax;
    form.poiMoyenHeightMin = params.controls.poi.moyenHeightMin;
    form.poiMoyenHeightMax = params.controls.poi.moyenHeightMax;
    form.poiPetitHeightMin = params.controls.poi.petitHeightMin;
    form.poiPetitHeightMax = params.controls.poi.petitHeightMax;
    form.poiEdgeReach = params.controls.poi.edgeReach;
    form.poiEdgeMax = params.controls.poi.edgeMax;
    form.poiWaterReach = params.controls.poi.waterPoiReach;
    form.poiGrandStartClearance = params.controls.poi.grandStartClearance;
    form.poiConeSlopeMinDeg = params.controls.poi.coneSlopeMinDeg;
    form.poiConeSlopeMaxDeg = params.controls.poi.coneSlopeMaxDeg;
    form.poiVerticality = params.controls.poi.verticality;
    form.poiConeSlopeSteepMinDeg = params.controls.poi.coneSlopeSteepMinDeg;
    form.poiConeSlopeSteepMaxDeg = params.controls.poi.coneSlopeSteepMaxDeg;
    form.poiCorridorHalfWidthMin = params.controls.poi.corridorHalfWidthMin;
    form.poiCorridorHalfWidthMax = params.controls.poi.corridorHalfWidthMax;
    form.poiScreenHeightMin = params.controls.poi.screenHeightMin;
    form.poiScreenHeightMax = params.controls.poi.screenHeightMax;
    form.poiScreenNotch = params.controls.poi.screenNotch;
    form.macroReliefOctaves = params.macro.reliefOctaves;
    form.macroReliefAmplitudeEtage0 = params.macro.tiers[0].reliefAmplitude;
    form.macroReliefAmplitudeEtage1 = params.macro.tiers[1].reliefAmplitude;
    form.macroReliefAmplitudeEtage2 = params.macro.tiers[2].reliefAmplitude;
    form.macroReliefAmplitudeEtage3 = params.macro.tiers[3].reliefAmplitude;
    form.macroReliefWavelengthEtage0 = params.macro.tiers[0].reliefWavelength;
    form.macroReliefWavelengthEtage1 = params.macro.tiers[1].reliefWavelength;
    form.macroReliefWavelengthEtage2 = params.macro.tiers[2].reliefWavelength;
    form.macroReliefWavelengthEtage3 = params.macro.tiers[3].reliefWavelength;
    form.macroPlateauTerrace = params.macro.tiers[2].terrace;
    form.macroTerraceStep = params.macro.terraceStep;
    form.macroTerraceEdge = params.macro.terraceEdge;
    form.macroCliffStep = params.macro.cliffStep;
    form.macroCliffEdge = params.macro.cliffEdge;
    form.macroWarpWavelength = params.macro.warpWavelength;
    form.macroWarpStrength = params.macro.warpStrength;
    form.macroValleyStretch = params.macro.valleyStretch;
    form.bakeMacroTexel = params.macroTexel;
    form.bakeCalmCut = params.calmCut;
    form.bakeRoughCut = params.roughCut;
    form.bakeDimpleFillMax = params.dimpleFillMax;
    form.bakeKeepCrestFade = params.keepCrestFade;
    form.bakeFluvialIterations = params.fluvial.iterations;
    form.bakeFluvialK = params.fluvial.k;
    form.bakeUpliftRate = params.fluvial.upliftRate;
    form.bakeThermalIterations = params.thermal.iterations;
    form.bakeTalusTan = params.thermal.talusTan;
    form.bakeRoundingStrength = params.rounding.strength;
    form.rhythmZones = params.controls.rhythm.zones;
    form.zoneCellSize = params.controls.zones.cellSize;
    form.zoneJitter = params.controls.zones.jitter;
    form.zoneBorderWarp = params.controls.zones.borderWarp;
    form.zoneBorderWarpWavelength = params.controls.zones.borderWarpWavelength;
    form.zoneStepHeight = params.controls.zones.stepHeight;
    form.zoneStoreys = params.controls.zones.storeys;
    form.zoneTrendWavelength = params.controls.zones.trendWavelength;
    form.zoneTrendContrast = params.controls.zones.trendContrast;
    form.zoneWallWidthOne = params.controls.zones.wallWidthOne;
    form.zoneWallWidthHigh = params.controls.zones.wallWidthHigh;
    form.zoneRampWidth = params.controls.zones.rampWidth;
    form.zoneStartGap = params.controls.zones.startGap;
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

u64 hashTerrainGenTuning(const TileBakeParams& params) {
    const data::TerrainGenTuningForm form = captureTerrainGenTuning(params);
    u64 h = 1469598103934665603ull; // FNV-1a
    const auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    reflect::forEachField(
        data::TerrainGenTuningForm::staticTypeInfo(),
        [&](const reflect::FieldInfo& field) {
            if ((field.flags & reflect::Transient) != 0) {
                return;
            }
            mix(&field.id, sizeof(field.id));
            reflect::visit(field.get(&form), reflect::overloaded {
                [&](bool v) { const u8 b = v ? 1 : 0; mix(&b, 1); },
                [&](i32 v) { mix(&v, sizeof(v)); },
                [&](u32 v) { mix(&v, sizeof(v)); },
                [&](f32 v) { mix(&v, sizeof(v)); },
                [&](f64 v) { mix(&v, sizeof(v)); },
                [&](Vec2 v) { mix(&v, sizeof(v)); },
                [&](Vec3 v) { mix(&v, sizeof(v)); },
                [&](Vec4 v) { mix(&v, sizeof(v)); },
                [&](Quat v) { mix(&v, sizeof(v)); },
                [&](const str& v) { mix(v.data(), v.size()); },
                [&](const core::Guid& v) {
                    const str text = v.toString();
                    mix(text.data(), text.size());
                },
            });
        });
    return h;
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
    // Border transitions on (bakeMap fills the grid spec; the streamer
    // overwrites it from its own map config either way).
    params.mapGrid.valid = true;
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
