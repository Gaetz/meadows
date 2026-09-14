#pragma once

#include "data/plugins/EditSession.hpp"
#include "engine/terrain/generation/Hydrology.hpp"

// Chantier CARTES M5 (docs/TERRAIN-MAPS.md): a baked bounded map mapped
// onto ORDINARY §5 records — the WorldspaceForm IS the map (its guid is
// the map guid), the set of TerrainRegionForm records IS the slice
// list, the water bodies are WaterBodyForm/RiverForm records. Every
// record derives its guid from the map guid (Guid::combine), so a
// re-bake PATCHES the same records instead of duplicating them, and
// hand retouches layer on top (§5). The manifest stays a runtime cache
// sidecar; at export this information BECOMES the records — no
// parallel manifest Form (§5.1: never two resolution mechanisms).
//
// Records are staged as SESSION drafts only (export = an ordinary
// mod): the running session already plays the map through the bake
// cache, so there is no live-staging need (the dungeon precedent's
// live half does not apply).

namespace world {

// One baked slice, as the caller registered it: the .trg asset guid
// plus the detail params buildTerrainBase applies over it.
struct MapSliceRecord {
    i32 tx { 0 };
    i32 tz { 0 };
    core::Guid asset; // registered .trg asset
    f32 detailAmplitude { 0.0f };
    f32 detailWavelength { 60.0f };
    i32 detailOctaves { 3 };
};

struct MapStageResult {
    core::Guid worldspace;
    u32 slices { 0 };
    u32 lakes { 0 };
    u32 rivers { 0 };
};

// Deterministic identity of a PROCEDURAL map (§2.5 — derived, never
// minted): every session, save and mod that bakes the same
// (worldSeed, mapX, mapZ) talks about the same worldspace. An
// authored map mints its guid at creation instead.
core::Guid mapWorldspaceGuid(u32 worldSeed, i32 mapX, i32 mapZ);

// The .trg asset identity of one slice, derived from the map guid —
// stable across re-bakes, so a re-export patches the same records
// and overwrites the same asset entries.
core::Guid mapSliceAssetGuid(const core::Guid& mapGuid, u32 sliceIndex);

// Stage one map: 1 WorldspaceForm (guid = mapGuid, bounded) + one
// TerrainRegionForm per slice + WaterBodyForm per lake + RiverForm/
// RiverPointForm per course, all worldspace-scoped to the map.
// `lakes`/`rivers` are the merged .twb contents of the slices (the
// caller reads them; lake masks stay in the .twb tier — a §5 water
// record is the rect + level, like the editor's Accept path).
// `riverThin` > 0 drops course points closer than that many meters to
// the previous kept one (ends always kept): a full map's hydrology at
// bake spacing is tens of thousands of records — the record tier only
// needs ribbon fidelity (the far-water spacing, 48 m, is plenty).
MapStageResult stageMapRecords(
    data::EditSession& session, const data::FormDatabase& forms,
    const core::Guid& mapGuid, const str& mapName, i32 mapX, i32 mapZ,
    f32 mapSize, u32 mapSeed, const vector<MapSliceRecord>& slices,
    const vector<render::terraingen::Lake>& lakes,
    const vector<render::terraingen::River>& rivers,
    f32 riverThin = 0.0f);

} // namespace world
