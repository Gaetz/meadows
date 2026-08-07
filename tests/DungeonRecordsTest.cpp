#include <doctest/doctest.h>

#include <memory>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/EditSession.hpp"
#include "world/dungeon/DungeonRecords.hpp"
#include "world/worldspace/WorldForms.hpp"

using namespace world;

namespace {

const core::Guid kDungeonId =
    *core::Guid::fromString("dddd0001-0000-4000-8000-000000000001");
const core::Guid kExteriorId =
    *core::Guid::fromString("dddd0002-0000-4000-8000-000000000001");
const core::Guid kNavAsset =
    *core::Guid::fromString("dddd0003-0000-4000-8000-000000000001");

core::Guid meshAssetFor(i32 cx, i32 cz) {
    const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) |
                    static_cast<u64>(static_cast<u32>(cz));
    return core::Guid::combine(kDungeonId, core::Guid { key, 0xA55E7 });
}

struct Fixture {
    data::FormTypeRegistry types;
    data::FormDatabase db;
    WorldModel model;
    dungeon::DungeonBakeResult bake;
    vector<DungeonAnchor> anchors;

    Fixture() {
        data::registerCoreFormTypes(types);
        data::registerVisualFormTypes(types);
        registerWorldFormTypes(types);

        auto exterior = std::make_unique<WorldspaceForm>();
        exterior->id = kExteriorId;
        exterior->editorId = "Overworld";
        exterior->cellSize = 64.0f;
        db.add(std::move(exterior), WorldspaceForm::staticTypeInfo());
        model = WorldModel::build(db);
        model.materializeCell(db, db.handleOf(kExteriorId), 10, 12);

        dungeon::DungeonParams params;
        params.seed = 42;
        params.space.gridX = 5;
        params.space.gridZ = 5;
        params.space.floors = 2;
        params.voxelSize = 1.4f;
        bake = dungeon::bakeDungeon(params);

        DungeonAnchor anchor;
        anchor.cell = cellGuidFor(kExteriorId, 10, 12);
        anchor.doorPos = { 645.0f, 12.0f, 800.0f };
        anchor.yawDeg = 90.0f;
        anchors.push_back(anchor);
    }
};

} // namespace

TEST_CASE("dungeon records: staging emits worldspace, cells, meshes, nav, doors") {
    Fixture fx;
    REQUIRE_FALSE(fx.bake.empty());
    data::EditSession session { fx.db, fx.types };

    const DungeonStageResult staged = stageDungeonRecords(
        session, fx.db, fx.model, fx.bake, kDungeonId, "TestMine",
        meshAssetFor, kNavAsset, fx.anchors);

    CHECK(staged.worldspace == kDungeonId);
    CHECK(staged.cellCount == fx.bake.cellMeshes.size());
    CHECK(staged.torchCount == fx.bake.torches.size());
    REQUIRE(staged.outsideDoorRefs.size() == 1);

    // The worldspace exists LIVE (travel this session)...
    REQUIRE(fx.db.handleOf(kDungeonId).isValid());
    const auto* liveWs = static_cast<const WorldspaceForm*>(
        fx.db.get(fx.db.handleOf(kDungeonId)));
    CHECK(liveWs->interior);
    CHECK(liveWs->cellSize == fx.bake.cellSize);

    // ...and as a session draft (the export ships it).
    const auto* draftWs =
        static_cast<const WorldspaceForm*>(session.view(kDungeonId));
    REQUIRE(draftWs != nullptr);
    CHECK(draftWs->interior);
    CHECK(session.isCreated(kDungeonId));

    // Cells are live (streamable now) and drafted (exported later).
    const auto& firstMesh = fx.bake.cellMeshes.front();
    const core::Guid firstCell =
        cellGuidFor(kDungeonId, firstMesh.cx, firstMesh.cz);
    CHECK(fx.db.handleOf(firstCell).isValid());
    CHECK(session.isCreated(firstCell));

    // The nav record points at the asset.
    const auto* nav =
        static_cast<const NavGridForm*>(session.view(staged.navGridRecord));
    REQUIRE(nav != nullptr);
    CHECK(nav->worldspace == kDungeonId);
    CHECK(nav->asset == kNavAsset);

    // The outside door stands in the anchor cell and leads inside.
    const auto* doorRef = static_cast<const ReferenceForm*>(
        session.view(staged.outsideDoorRefs[0]));
    REQUIRE(doorRef != nullptr);
    CHECK(doorRef->cell == fx.anchors[0].cell);
    const auto* door =
        static_cast<const DoorForm*>(session.view(doorRef->baseForm));
    REQUIRE(door != nullptr);
    const auto* arriveIn =
        static_cast<const ReferenceForm*>(session.view(door->targetMarker));
    REQUIRE(arriveIn != nullptr);
    const auto* arriveCell =
        static_cast<const CellForm*>(session.view(arriveIn->cell));
    REQUIRE(arriveCell != nullptr);
    CHECK(arriveCell->worldspace == kDungeonId);
}

TEST_CASE("dungeon records: re-Accept patches the same records, no duplicates") {
    Fixture fx;
    REQUIRE_FALSE(fx.bake.empty());
    data::EditSession session { fx.db, fx.types };

    const DungeonStageResult first = stageDungeonRecords(
        session, fx.db, fx.model, fx.bake, kDungeonId, "TestMine",
        meshAssetFor, kNavAsset, fx.anchors);
    const u32 dirtyAfterFirst = session.dirtyCount();

    const DungeonStageResult second = stageDungeonRecords(
        session, fx.db, fx.model, fx.bake, kDungeonId, "TestMine",
        meshAssetFor, kNavAsset, fx.anchors);

    CHECK(second.worldspace == first.worldspace);
    CHECK(second.navGridRecord == first.navGridRecord);
    CHECK(second.outsideDoorRefs == first.outsideDoorRefs);
    CHECK(session.dirtyCount() == dirtyAfterFirst);

    // The export is an ordinary plugin carrying the whole dungeon.
    const data::Plugin plugin = session.exportPlugin(
        *core::Guid::fromString("dddd00ff-0000-4000-8000-000000000001"),
        "test-mine");
    CHECK(plugin.records.size() == dirtyAfterFirst);
}
