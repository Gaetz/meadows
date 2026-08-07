#pragma once

#include <functional>

#include "data/plugins/EditSession.hpp"
#include "engine/dungeon/DungeonBake.hpp"
#include "world/worldspace/WorldModel.hpp"

// Stage D7 of the dungeon pipeline (docs/DUNGEON-GEN.md) — the bake mapped
// onto ORDINARY records: an interior WorldspaceForm, materialized CellForms,
// a StaticForm + ReferenceForm per cavern chunk, torch lights, entry/exit
// doors and their arrival markers, and the NavGridForm. Everything derives
// its guid from `dungeonId` (Guid::combine), so a re-Accept PATCHES the same
// records instead of duplicating them, and hand retouches layer on top (§5).
// Records go through the EditSession (export = an ordinary mod) AND live
// cells through WorldModel::materializeCell so the current session can
// travel there before the next resolve.

namespace world {

// Where a door into the dungeon stands, in an EXISTING worldspace — the
// overworld or another interior. Several anchors = several entrances.
struct DungeonAnchor {
    core::Guid cell;   // the cell record holding the outside door
    Vec3 doorPos {};   // outside door position (that worldspace's frame)
    f32 yawDeg { 0.0f };
};

struct DungeonStageResult {
    core::Guid worldspace;
    core::Guid navGridRecord;
    vector<core::Guid> outsideDoorRefs; // one per anchor
    u32 cellCount { 0 };
    u32 torchCount { 0 };
};

// `cellMeshAsset` maps a baked cell to its registered `.cmesh` asset guid;
// `navAsset` is the registered `.nvg` guid; `doorModel`/`doorMaterial`
// dress the door leaves (null = placeholder visuals). The tool registers
// those assets (live + export list) before calling this.
DungeonStageResult stageDungeonRecords(
    data::EditSession& session, data::FormDatabase& forms, WorldModel& model,
    const dungeon::DungeonBakeResult& bake, const core::Guid& dungeonId,
    const str& dungeonName,
    const std::function<core::Guid(i32 cx, i32 cz)>& cellMeshAsset,
    const core::Guid& navAsset, const vector<DungeonAnchor>& anchors,
    const core::Guid& doorModel = {}, const core::Guid& doorMaterial = {});

} // namespace world
