#pragma once

#include "data/forms/LandscapeForms.hpp"
#include "engine/core/Defines.hpp"
#include "engine/reflect/FieldLane.hpp"
#include "engine/render/AtmosphereParams.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "engine/render/landscape/TreeGenerator.hpp" // by-value params

namespace render {
struct HeightPatches;
struct TerrainBase;
}

namespace game {

// ONE definition per knob: the lane tables below pair each engine param
// with its Form field and carry the panel label; the slider range is
// reflected metadata on the Form field (REFLECT_FIELD_RANGE). Every
// table is walked in THREE directions — RenderTuningIo applies and
// captures it, RenderTuningPanels draws it — so a knob added to a table
// is loaded, saved and shown, and a knob forgotten in one of three
// hand-written lists cannot happen.
//
// A table is one uninterrupted run of the panel: the tables of a
// Params/Form pair are split and ordered exactly as the panel draws
// them (an int lane between two float lanes, a hand-drawn widget in the
// middle of a section). Tables marked "not drawn" feed knobs whose
// widget stays hand-written (colour edits, range pairs).
namespace tuning_lanes {

using RenderTuning = render::WorldRenderer::RenderTuning;
using Tuning = data::LandscapeTuningForm;
template <typename Params, typename T>
using WorldLane = reflect::FieldLane<Params, Tuning, T>;
template <typename T>
using LobeLane = reflect::FieldLane<render::LobeTreeParams, data::LobeTreeTuningForm, T>;
template <typename T>
using ColonizedLane = reflect::FieldLane<render::ColonizedTreeParams, data::ColonizedTreeTuningForm, T>;
template <typename T>
using RcLane = reflect::FieldLane<render::RcTuning, data::RcTuningForm, T>;

// --- Tree builder panel: "Lobe trees (classic)" ---
inline constexpr LobeLane<f32> kLobeTrunk[] = {
    { &render::LobeTreeParams::trunkHeightMin, &data::LobeTreeTuningForm::trunkHeightMin, "Trunk height min" },
    { &render::LobeTreeParams::trunkHeightMax, &data::LobeTreeTuningForm::trunkHeightMax, "Trunk height max" },
    { &render::LobeTreeParams::trunkRadiusMin, &data::LobeTreeTuningForm::trunkRadiusMin, "Trunk radius min" },
    { &render::LobeTreeParams::trunkRadiusMax, &data::LobeTreeTuningForm::trunkRadiusMax, "Trunk radius max" },
    { &render::LobeTreeParams::trunkTaper, &data::LobeTreeTuningForm::trunkTaper, "Trunk taper" },
    { &render::LobeTreeParams::lean, &data::LobeTreeTuningForm::lean, "Lean" },
};
inline constexpr LobeLane<i32> kLobeBranchCount[] = {
    { &render::LobeTreeParams::branchCountMin, &data::LobeTreeTuningForm::branchCountMin, "Branches min" },
    { &render::LobeTreeParams::branchCountMax, &data::LobeTreeTuningForm::branchCountMax, "Branches max" },
};
inline constexpr LobeLane<f32> kLobeCrown[] = {
    { &render::LobeTreeParams::branchLengthMin, &data::LobeTreeTuningForm::branchLengthMin, "Branch length min" },
    { &render::LobeTreeParams::branchLengthMax, &data::LobeTreeTuningForm::branchLengthMax, "Branch length max" },
    { &render::LobeTreeParams::crownLobeRadiusMin, &data::LobeTreeTuningForm::crownLobeRadiusMin, "Crown lobe min" },
    { &render::LobeTreeParams::crownLobeRadiusMax, &data::LobeTreeTuningForm::crownLobeRadiusMax, "Crown lobe max" },
    { &render::LobeTreeParams::branchLobeRadiusMin, &data::LobeTreeTuningForm::branchLobeRadiusMin, "Branch lobe min" },
    { &render::LobeTreeParams::branchLobeRadiusMax, &data::LobeTreeTuningForm::branchLobeRadiusMax, "Branch lobe max" },
    { &render::LobeTreeParams::lobeFlatten, &data::LobeTreeTuningForm::lobeFlatten, "Lobe flatten" },
    { &render::LobeTreeParams::normalSpherize, &data::LobeTreeTuningForm::normalSpherize, "Normal spherize" },
};

// --- Tree builder panel: "Space colonization" ---
// Skeleton (Runions)
inline constexpr ColonizedLane<f32> kColonizedGrowth[] = {
    { &render::ColonizedTreeParams::segment, &data::ColonizedTreeTuningForm::segment, "Growth step D (m)" },
    { &render::ColonizedTreeParams::killDistance, &data::ColonizedTreeTuningForm::killDistance, "Kill distance (m)" },
};
inline constexpr ColonizedLane<i32> kColonizedAttractors[] = {
    { &render::ColonizedTreeParams::attractorCount, &data::ColonizedTreeTuningForm::attractorCount, "Attractors" },
};
inline constexpr ColonizedLane<f32> kColonizedPipe[] = {
    { &render::ColonizedTreeParams::pipeExponent, &data::ColonizedTreeTuningForm::pipeExponent, "Pipe exponent" },
    { &render::ColonizedTreeParams::tropism, &data::ColonizedTreeTuningForm::tropism, "Tropism (up bias)" },
};
// Wood
inline constexpr ColonizedLane<i32> kColonizedTubeSides[] = {
    { &render::ColonizedTreeParams::tubeSides, &data::ColonizedTreeTuningForm::tubeSides, "Tube sides (max)" },
};
inline constexpr ColonizedLane<f32> kColonizedWood[] = {
    { &render::ColonizedTreeParams::sideMinFraction, &data::ColonizedTreeTuningForm::sideMinFraction, "Side taper floor" },
    { &render::ColonizedTreeParams::curvePreserve, &data::ColonizedTreeTuningForm::curvePreserve, "Curve preserve" },
};
inline constexpr ColonizedLane<i32> kColonizedCurveSubdiv[] = {
    { &render::ColonizedTreeParams::curveSubdiv, &data::ColonizedTreeTuningForm::curveSubdiv, "Curve subdivision" },
};
inline constexpr ColonizedLane<f32> kColonizedWoodNoise[] = {
    { &render::ColonizedTreeParams::pathJitter, &data::ColonizedTreeTuningForm::pathJitter, "Path noise (kinks)" },
    { &render::ColonizedTreeParams::ringIrregularity, &data::ColonizedTreeTuningForm::ringIrregularity, "Ring irregularity" },
    { &render::ColonizedTreeParams::flareAmount, &data::ColonizedTreeTuningForm::flareAmount, "Root flare amount" },
    { &render::ColonizedTreeParams::flareHeight, &data::ColonizedTreeTuningForm::flareHeight, "Root flare height (m)" },
};
inline constexpr ColonizedLane<i32> kColonizedFlareLobes[] = {
    { &render::ColonizedTreeParams::flareLobes, &data::ColonizedTreeTuningForm::flareLobes, "Root flare lobes" },
};
// Bark material (live, no rebuild)
inline constexpr ColonizedLane<f32> kColonizedBark[] = {
    { &render::ColonizedTreeParams::barkTileScale, &data::ColonizedTreeTuningForm::barkTileScale, "Bark tile (tiles/m)" },
    { &render::ColonizedTreeParams::barkHexCell, &data::ColonizedTreeTuningForm::barkHexCell, "Hex cell (uv units)" },
    { &render::ColonizedTreeParams::barkHexSharpness, &data::ColonizedTreeTuningForm::barkHexSharpness, "Hex seam sharpness" },
};
// Colour edits (not drawn through the tables).
inline constexpr ColonizedLane<Vec3> kColonizedTints[] = {
    { &render::ColonizedTreeParams::barkTint, &data::ColonizedTreeTuningForm::barkTint, "Bark tint" },
    { &render::ColonizedTreeParams::autumnTint, &data::ColonizedTreeTuningForm::autumnTint, "Autumn tint" },
};
// Crown envelope
inline constexpr ColonizedLane<f32> kColonizedCrown[] = {
    { &render::ColonizedTreeParams::trunkBaseMin, &data::ColonizedTreeTuningForm::trunkBaseMin, "Bare trunk min (m)" },
    { &render::ColonizedTreeParams::trunkBaseMax, &data::ColonizedTreeTuningForm::trunkBaseMax, "Bare trunk max (m)" },
    { &render::ColonizedTreeParams::crownHeightMin, &data::ColonizedTreeTuningForm::crownHeightMin, "Crown height min" },
    { &render::ColonizedTreeParams::crownHeightMax, &data::ColonizedTreeTuningForm::crownHeightMax, "Crown height max" },
    { &render::ColonizedTreeParams::crownRadiusMin, &data::ColonizedTreeTuningForm::crownRadiusMin, "Crown radius min" },
    { &render::ColonizedTreeParams::crownRadiusMax, &data::ColonizedTreeTuningForm::crownRadiusMax, "Crown radius max" },
};
// Conifer habit
inline constexpr ColonizedLane<f32> kColonizedConifer[] = {
    { &render::ColonizedTreeParams::crownTaper, &data::ColonizedTreeTuningForm::crownTaper, "Crown taper (cone)" },
    { &render::ColonizedTreeParams::leaderBias, &data::ColonizedTreeTuningForm::leaderBias, "Leader bias" },
    { &render::ColonizedTreeParams::lateralFlatten, &data::ColonizedTreeTuningForm::lateralFlatten, "Lateral flatten" },
    { &render::ColonizedTreeParams::sprayFoliage, &data::ColonizedTreeTuningForm::sprayFoliage, "Spray foliage" },
};
// Leaf style (atlas slot + season); leafShape is a combo (not drawn).
inline constexpr ColonizedLane<i32> kColonizedLeafStyle[] = {
    { &render::ColonizedTreeParams::leafStyle, &data::ColonizedTreeTuningForm::leafStyle, "Atlas slot" },
};
inline constexpr ColonizedLane<i32> kColonizedLeafShape[] = {
    { &render::ColonizedTreeParams::leafShape, &data::ColonizedTreeTuningForm::leafShape, "Leaf shape" },
};
inline constexpr ColonizedLane<f32> kColonizedSeason[] = {
    { &render::ColonizedTreeParams::seasonality, &data::ColonizedTreeTuningForm::seasonality, "Seasonality" },
};
// Foliage SDF + cards
inline constexpr ColonizedLane<f32> kColonizedFoliage[] = {
    { &render::ColonizedTreeParams::tipBallRadius, &data::ColonizedTreeTuningForm::tipBallRadius, "Tip ball radius" },
    { &render::ColonizedTreeParams::tipBallMin, &data::ColonizedTreeTuningForm::tipBallMin, "Tip ball floor" },
    { &render::ColonizedTreeParams::tipOrderFalloff, &data::ColonizedTreeTuningForm::tipOrderFalloff, "Tip order falloff" },
    { &render::ColonizedTreeParams::smoothK, &data::ColonizedTreeTuningForm::smoothK, "Smooth-min k" },
    { &render::ColonizedTreeParams::cardHalfSizeMin, &data::ColonizedTreeTuningForm::cardHalfSizeMin, "Card size min" },
    { &render::ColonizedTreeParams::cardHalfSizeMax, &data::ColonizedTreeTuningForm::cardHalfSizeMax, "Card size max" },
    { &render::ColonizedTreeParams::densityGradient, &data::ColonizedTreeTuningForm::densityGradient, "Density gradient G" },
    { &render::ColonizedTreeParams::foliageDensity, &data::ColonizedTreeTuningForm::foliageDensity, "Card density x" },
};
// Leaf mask (card texture)
inline constexpr ColonizedLane<i32> kColonizedLeafCount[] = {
    { &render::ColonizedTreeParams::leafCount, &data::ColonizedTreeTuningForm::leafCount, "Leaf count" },
};
inline constexpr ColonizedLane<f32> kColonizedLeafSize[] = {
    { &render::ColonizedTreeParams::leafSizeMin, &data::ColonizedTreeTuningForm::leafSizeMin, "Leaf size min" },
    { &render::ColonizedTreeParams::leafSizeMax, &data::ColonizedTreeTuningForm::leafSizeMax, "Leaf size max" },
};
// Live shader window (no rebuild).
inline constexpr ColonizedLane<f32> kColonizedLeafLod[] = {
    { &render::ColonizedTreeParams::leafSolidStart, &data::ColonizedTreeTuningForm::leafSolidStart, "Leaf solid start (mip)" },
    { &render::ColonizedTreeParams::leafSolidEnd, &data::ColonizedTreeTuningForm::leafSolidEnd, "Leaf solid end (mip)" },
};

// --- Render panel: "Global illumination" (technique and debug view are
// combos, hand-drawn) ---
inline constexpr RcLane<i32> kRcResolution[] = {
    { &render::RcTuning::resolution, &data::RcTuningForm::resolution, "Resolution (voxels)" },
};
inline constexpr RcLane<f32> kRcVoxels[] = {
    { &render::RcTuning::fineVoxel, &data::RcTuningForm::fineVoxel, "Fine voxel (m)" },
    { &render::RcTuning::coarseVoxel, &data::RcTuningForm::coarseVoxel, "Coarse voxel (m)" },
};
inline constexpr RcLane<i32> kRcUpdateInterval[] = {
    { &render::RcTuning::updateInterval, &data::RcTuningForm::updateInterval, "Update every N frames" },
};
inline constexpr RcLane<f32> kRcTileDrift[] = {
    { &render::RcTuning::tileRebakeDrift, &data::RcTuningForm::tileRebakeDrift, "GI tile rebake drift" },
};
inline constexpr RcLane<bool> kRcGridNormals[] = {
    { &render::RcTuning::gridNormals, &data::RcTuningForm::gridNormals, "GI tile grid normals" },
};
inline constexpr RcLane<i32> kRcCascadeCount[] = {
    { &render::RcTuning::cascadeCount, &data::RcTuningForm::cascadeCount, "Cascade count" },
};
inline constexpr RcLane<f32> kRcInterval[] = {
    { &render::RcTuning::interval0, &data::RcTuningForm::interval0, "Interval 0 (m)" },
};
inline constexpr RcLane<f32> kRcInjection[] = {
    { &render::RcTuning::skyFactor, &data::RcTuningForm::skyFactor, "Sky factor" },
    { &render::RcTuning::emitterBoost, &data::RcTuningForm::emitterBoost, "Light emitter boost" },
    { &render::RcTuning::lightSplatBounce, &data::RcTuningForm::lightSplatBounce, "Light splat bounce (clustered)" },
    { &render::RcTuning::bounceFeedback, &data::RcTuningForm::bounceFeedback, "Bounce feedback" },
};
inline constexpr RcLane<bool> kRcScheduling[] = {
    { &render::RcTuning::pipelined, &data::RcTuningForm::pipelined, "Pipelined GI (frame N-1)" },
    { &render::RcTuning::asyncCompute, &data::RcTuningForm::asyncCompute, "Async compute GI (2nd queue)" },
    { &render::RcTuning::rcOnlyLights, &data::RcTuningForm::rcOnlyLights, "Lights via RC only (penumbra experiment)" },
};
inline constexpr RcLane<f32> kRcApply[] = {
    { &render::RcTuning::intensity, &data::RcTuningForm::intensity, "Intensity" },
    { &render::RcTuning::giFloor, &data::RcTuningForm::giFloor, "Ambient floor (x classic)" },
    { &render::RcTuning::edgeFade, &data::RcTuningForm::edgeFade, "Edge fade (m)" },
    { &render::RcTuning::bandCount, &data::RcTuningForm::bandCount, "Band count (0 = smooth)" },
    { &render::RcTuning::bandAa, &data::RcTuningForm::bandAa, "Band AA" },
};
inline constexpr RcLane<bool> kRcIntervalExtension[] = {
    { &render::RcTuning::intervalExtension, &data::RcTuningForm::intervalExtension, "Interval extension (x4 march reach)" },
};

// --- Terrain panel ---
inline constexpr WorldLane<RenderTuning, bool> kTerrainFar[] = {
    { &RenderTuning::farTerrain, &Tuning::farTerrain, "Far terrain (silhouettes)" },
    { &RenderTuning::farHorizon, &Tuning::farHorizon, "Far horizon ring (24 km)" },
    { &RenderTuning::farImpostorsFromGrid, &Tuning::farImpostorsFromGrid, "Far impostors from grid" },
    { &RenderTuning::sharedHeightField, &Tuning::sharedHeightField, "Shared height field" },
};
inline constexpr WorldLane<RenderTuning, f32> kSeasons[] = {
    { &RenderTuning::seasonAutumn, &Tuning::seasonAutumn, "Season: autumn" },
    { &RenderTuning::seasonLeafFall, &Tuning::seasonLeafFall, "Season: leaf fall" },
};
inline constexpr WorldLane<render::ChunkOcclusion, f32> kOcclusion[] = {
    { &render::ChunkOcclusion::rebuildDistance, &Tuning::occlusionRebuildDistance, "Occlusion rebake drift (m)" },
};

// --- Render panel: "Grass" ---
inline constexpr WorldLane<render::GrassRenderTuning, f32> kGrassBlade[] = {
    { &render::GrassRenderTuning::bladeHeight, &Tuning::grassBladeHeight, "Height (m)" },
    { &render::GrassRenderTuning::bladeHalfWidth, &Tuning::grassBladeHalfWidth, "Half width (m)" },
};
// Colour edits (not drawn through the tables).
inline constexpr WorldLane<render::GrassRenderTuning, Vec3> kGrassTints[] = {
    { &render::GrassRenderTuning::baseTint, &Tuning::grassBaseTint, "Base tint (x ground)" },
    { &render::GrassRenderTuning::tipTint, &Tuning::grassTipTint, "Tip tint (x ground)" },
};
inline constexpr WorldLane<render::GrassRenderTuning, f32> kGrassShading[] = {
    { &render::GrassRenderTuning::rootAo, &Tuning::grassRootAo, "Root AO" },
    { &render::GrassRenderTuning::bladeNormals, &Tuning::grassBladeNormals, "Blade normals" },
};
// Range pair (not drawn through the tables).
inline constexpr WorldLane<render::GrassRenderTuning, f32> kGrassBrightness[] = {
    { &render::GrassRenderTuning::brightMin, &Tuning::grassBrightMin, "Blade brightness min" },
    { &render::GrassRenderTuning::brightMax, &Tuning::grassBrightMax, "Blade brightness max" },
};
inline constexpr WorldLane<render::GrassRenderTuning, f32> kGrassShadingTail[] = {
    { &render::GrassRenderTuning::middleDarken, &Tuning::grassMiddleDarken, "Middle darken" },
    { &render::GrassRenderTuning::backscatter, &Tuning::grassBackscatter, "Backscatter" },
    { &render::GrassRenderTuning::sheen, &Tuning::grassSheen, "Tip sheen" },
};
inline constexpr WorldLane<render::GrassRenderTuning, f32> kGrassDistance[] = {
    { &render::GrassRenderTuning::detailNear, &Tuning::grassDetailNear, "Detail near (m)" },
    { &render::GrassRenderTuning::detailFar, &Tuning::grassDetailFar, "Detail far (m)" },
    { &render::GrassRenderTuning::thinStart, &Tuning::grassThinStart, "Thin start (m)" },
    { &render::GrassRenderTuning::thinEnd, &Tuning::grassThinEnd, "Thin end (m)" },
    { &render::GrassRenderTuning::farDensity, &Tuning::grassFarDensity, "Far density" },
    { &render::GrassRenderTuning::widthCompensation, &Tuning::grassWidthCompensation, "Far width comp" },
    { &render::GrassRenderTuning::fadeStart, &Tuning::grassFadeStart, "Fade start (m)" },
    { &render::GrassRenderTuning::fadeEnd, &Tuning::grassFadeEnd, "Fade end (m)" },
};
// Scatter (re-bakes on release)
inline constexpr WorldLane<render::GrassScatterTuning, f32> kGrassScatter[] = {
    { &render::GrassScatterTuning::spacing, &Tuning::grassSpacing, "Blade spacing (m)" },
    { &render::GrassScatterTuning::patchBroadScale, &Tuning::grassPatchBroadScale, "Patch scale (m)" },
    { &render::GrassScatterTuning::patchDetailScale, &Tuning::grassPatchDetailScale, "Clump detail (m)" },
};
// Range pairs (not drawn through the tables).
inline constexpr WorldLane<render::GrassScatterTuning, f32> kGrassScatterWindows[] = {
    { &render::GrassScatterTuning::patchThresholdLo, &Tuning::grassPatchThresholdLo, "Patch threshold lo" },
    { &render::GrassScatterTuning::patchThresholdHi, &Tuning::grassPatchThresholdHi, "Patch threshold hi" },
    { &render::GrassScatterTuning::presenceLo, &Tuning::grassPresenceLo, "Presence rim" },
    { &render::GrassScatterTuning::presenceHi, &Tuning::grassPresenceHi, "Presence solid" },
};
inline constexpr WorldLane<render::GrassScatterTuning, f32> kGrassScatterCutoff[] = {
    { &render::GrassScatterTuning::materialCutoff, &Tuning::grassMaterialCutoff, "Material cutoff" },
};
inline constexpr WorldLane<render::GrassScatterTuning, bool> kGrassScatterTint[] = {
    { &render::GrassScatterTuning::coarseTint, &Tuning::grassCoarseTint, "Coarse tint lattice" },
};

// --- Render panel: "Lighting & shadows" ---
inline constexpr WorldLane<RenderTuning, bool> kClusteredLights[] = {
    { &RenderTuning::clusteredLights, &Tuning::clusteredLights, "Clustered lights (64-light budget)" },
};
inline constexpr WorldLane<RenderTuning, f32> kInteriorDaylight[] = {
    { &RenderTuning::interiorDaylightWeight, &Tuning::interiorDaylightWeight, "Interior daylight coupling" },
};

// --- Render panel: "Sun FX" ---
// The global atmosphere baseline: the scene-owned AtmosphereParams <->
// the tuning record. Per-weather states ride WeatherController's table
// (AtmosphereParams <-> WeatherForm): a different Form type, so the two
// tables cannot be one; fogSunPhase is global-only (no weather field).
inline constexpr WorldLane<render::AtmosphereParams, f32> kSunFx[] = {
    { &render::AtmosphereParams::godRayIntensity, &Tuning::godRayIntensity, "God rays intensity" },
    { &render::AtmosphereParams::volumetric, &Tuning::volumetricIntensity, "Volumetric shafts" },
};
inline constexpr WorldLane<render::PostFx, bool> kFroxelFog[] = {
    { &render::PostFx::froxelFog, &Tuning::froxelFog, "Froxel fog (V4/H4 — off = 2D march)" },
};
inline constexpr WorldLane<render::PostFx, f32> kFroxel[] = {
    { &render::PostFx::froxelTemporalBlend, &Tuning::froxelTemporalBlend, "Froxel temporal blend" },
    { &render::PostFx::froxelDustNoise, &Tuning::froxelDustNoise, "Dust wisps (sparse)" },
};
inline constexpr WorldLane<RenderTuning, f32> kInteriorDust[] = {
    { &RenderTuning::interiorDustDensity, &Tuning::interiorDustDensity, "Interior dust" },
};

// --- Render panel: "Fog & clouds" ---
inline constexpr WorldLane<render::AtmosphereParams, f32> kFog[] = {
    { &render::AtmosphereParams::fogDensity, &Tuning::fogDensity, "Fog density" },
    { &render::AtmosphereParams::fogHeightFalloff, &Tuning::fogHeightFalloff, "Fog height falloff" },
    { &render::AtmosphereParams::fogLowBoost, &Tuning::fogLowBoost, "Fog low-altitude boost" },
};
inline constexpr WorldLane<RenderTuning, f32> kFogTail[] = {
    { &RenderTuning::fogHorizonTail, &Tuning::fogHorizonTail, "Fog horizon tail" },
};
inline constexpr WorldLane<render::AtmosphereParams, f32> kFogStart[] = {
    { &render::AtmosphereParams::fogStart, &Tuning::fogStart, "Fog start (m)" },
};
// (fogSunScatter is weather-only: hand-drawn between the two runs.)
inline constexpr WorldLane<render::AtmosphereParams, f32> kFogSunAndClouds[] = {
    { &render::AtmosphereParams::fogSunPhase, &Tuning::fogSunPhase, "Fog sun phase exp" },
    { &render::AtmosphereParams::fogCeiling, &Tuning::fogCeiling, "Fog ceiling falloff" },
    { &render::AtmosphereParams::cloudCoverage, &Tuning::cloudCoverage, "Cloud coverage" },
    { &render::AtmosphereParams::cloudShadow, &Tuning::cloudShadowStrength, "Cloud shadow strength" },
};
// Weather-driven cloud geometry (no panel slider; not drawn).
inline constexpr WorldLane<render::AtmosphereParams, f32> kCloudGeometry[] = {
    { &render::AtmosphereParams::cloudHeight, &Tuning::cloudHeight, "Cloud height (m)" },
    { &render::AtmosphereParams::cloudScale, &Tuning::cloudScale, "Cloud scale" },
};
// Sky clouds (volumetric); the Vec4-packed shape/light knobs are
// hand-drawn between these runs.
inline constexpr WorldLane<RenderTuning, bool> kSkyClouds[] = {
    { &RenderTuning::skyClouds, &Tuning::skyCloudsVolumetric, "Volumetric clouds (A/B)" },
};
inline constexpr WorldLane<RenderTuning, f32> kSkyCloudLook[] = {
    { &RenderTuning::skyCloudLiningLobe, &Tuning::skyCloudLiningLobe, "Cloud lining lobe g" },
    { &RenderTuning::skyCloudPowder, &Tuning::skyCloudPowder, "Cloud powder" },
    { &RenderTuning::skyCloudPuffiness, &Tuning::skyCloudPuffiness, "Cloud puffiness" },
    { &RenderTuning::skyCloudRimGain, &Tuning::skyCloudRimGain, "Cloud rim gain" },
    { &RenderTuning::skyCloudRimLobe, &Tuning::skyCloudRimLobe, "Cloud rim lobe g" },
    { &RenderTuning::skyCloudBaseDark, &Tuning::skyCloudBaseDark, "Cloud storm darkening" },
};
// Ground mist (density/coverage are weather-owned: hand-drawn).
inline constexpr WorldLane<RenderTuning, bool> kMist[] = {
    { &RenderTuning::mist, &Tuning::mistEnabled, "Mist (A/B)" },
};
inline constexpr WorldLane<RenderTuning, f32> kMistCover[] = {
    { &RenderTuning::mistCoverageSoftness, &Tuning::mistCoverageSoftness, "Mist cover softness" },
    { &RenderTuning::mistReach, &Tuning::mistReach, "Mist reach (m)" },
};
inline constexpr WorldLane<RenderTuning, f32> kMistSun[] = {
    { &RenderTuning::mistSunBoost, &Tuning::mistSunBoost, "Mist sun boost" },
};
inline constexpr WorldLane<RenderTuning, bool> kMistNoise[] = {
    { &RenderTuning::mistNoiseTex, &Tuning::mistNoiseTexture, "Mist noise texture (A/B)" },
};
inline constexpr WorldLane<RenderTuning, f32> kMistDetail[] = {
    { &RenderTuning::mistDetailDropout, &Tuning::mistDetailDropout, "Mist detail dropout (m)" },
    { &RenderTuning::mistPuffiness, &Tuning::mistPuffiness, "Mist puffiness" },
};
inline constexpr WorldLane<render::PostFx, f32> kMistBlend[] = {
    { &render::PostFx::mistTemporalBlend, &Tuning::mistTemporalBlend, "Mist temporal blend" },
};

// --- Render panel: "Water" > Sim window (switches; the SameLine layout
// keeps their checkboxes hand-drawn, the clamped scalars too) ---
inline constexpr WorldLane<render::WaterSystem::SimConfig, bool> kWaterSim[] = {
    { &render::WaterSystem::SimConfig::enabled, &Tuning::waterSimEnabled, "Sim enabled" },
    { &render::WaterSystem::SimConfig::settleGated, &Tuning::waterSimSettleGated, "Settle-gated reveal" },
    { &render::WaterSystem::SimConfig::rainPuddles, &Tuning::waterSimRainPuddles, "Rain puddles" },
};

// --- Render panel: "Post-processing" ---
inline constexpr WorldLane<RenderTuning, f32> kExposure[] = {
    { &RenderTuning::exposure, &Tuning::exposure, "Exposure" },
};
inline constexpr WorldLane<RenderTuning, f32> kAutoExposure[] = {
    { &RenderTuning::autoExposureMin, &Tuning::autoExposureMin, "Auto-expo min" },
    { &RenderTuning::autoExposureMax, &Tuning::autoExposureMax, "Auto-expo max" },
};
inline constexpr WorldLane<render::AtmosphereParams, f32> kBloom[] = {
    { &render::AtmosphereParams::bloomIntensity, &Tuning::bloomIntensity, "Bloom intensity" },
};
inline constexpr WorldLane<RenderTuning, f32> kGrade[] = {
    { &RenderTuning::gradeVibrance, &Tuning::gradeVibrance, "Vibrance" },
    { &RenderTuning::gradeSplitTone, &Tuning::gradeSplitTone, "Split tone" },
    { &RenderTuning::gradeContrast, &Tuning::gradeContrast, "Contrast" },
};

} // namespace tuning_lanes

// The Forms<->flat-params seam of the world renderer (CLAUDE.md §4: the
// engine never sees a Form). apply* seeds the renderer's live knobs from
// the resolved tuning records at startup (§5: the TOML sets where
// everything starts); capture* is the reverse mapping for the panels'
// "Save render tuning" button — fills the records from the CURRENT live
// values (fields the panels don't own keep `out`'s values; the scene
// overlays its atmosphere-owned fields, then writes the overlay plugin).
// Stateless: the seam edits the knobs in place through the renderer's
// public tuning accessors.
class RenderTuningIo {
public:
    // `terrain` is the scene-owned world params (RenderView::terrain):
    // the terrain shape half of the record lands there, apply-only.
    // `activeSnowLine` is the MODE-resolved value (story snowLine or
    // sandboxSnowLine) — the scene owns that decision; passing the raw
    // form value here silently re-lowers the CPU tree line in sandbox
    // while the shader snow stays high.
    static void applyTuning(render::WorldRenderer& r,
                            render::TerrainParams& terrain,
                            const data::LandscapeTuningForm& tuning,
                            const sptr<const render::HeightPatches>& patches,
                            const sptr<const render::TerrainBase>& base,
                            f32 activeSnowLine);
    // Tree builder: the two *TreeTuningForm records mapped onto the
    // generators' flat engine params (the TerrainParams pattern) —
    // startup values; the Trees panel edits them live.
    static void applyTreeTuning(render::WorldRenderer& r,
                                const data::LobeTreeTuningForm& lobes,
                                const data::ColonizedTreeTuningForm& colonized);
    // The same field-for-field mappings as pure functions — the species
    // wiring (named tree-type records -> variant slots) reuses them.
    static render::LobeTreeParams toLobeParams(
        const data::LobeTreeTuningForm& lobes);
    static render::ColonizedTreeParams toColonizedParams(
        const data::ColonizedTreeTuningForm& colonized);
    // GI tuning record -> the live RcTuning struct (same contract).
    static void applyRcTuning(render::WorldRenderer& r,
                              const data::RcTuningForm& rc);
    // The atmosphere half of the tuning record -> the scene's live
    // AtmosphereParams (the global baseline the weather crossfade and the
    // sliders then write over). The scene owns `atmos`, not the renderer,
    // hence the separate entry point; captureAtmosphere is its mirror.
    static void applyAtmosphere(render::AtmosphereParams& atmos,
                                const data::LandscapeTuningForm& tuning);

    static void captureTuning(const render::WorldRenderer& r,
                              data::LandscapeTuningForm& out);
    static void captureAtmosphere(const render::AtmosphereParams& atmos,
                                  data::LandscapeTuningForm& out);
    static void captureRcTuning(const render::WorldRenderer& r,
                                data::RcTuningForm& out);
    static void captureTreeTuning(const render::WorldRenderer& r,
                                  data::LobeTreeTuningForm& lobes,
                                  data::ColonizedTreeTuningForm& colonized);
};

} // namespace game
