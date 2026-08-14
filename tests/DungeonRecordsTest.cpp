#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/forms/LocForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
#include "quest/Dialogue.hpp"
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

DungeonKit testKit() {
    DungeonKit kit;
    kit.barrier = core::Guid::combine(kDungeonId, { 1, 0x517 });
    kit.lever = core::Guid::combine(kDungeonId, { 2, 0x517 });
    kit.chest = core::Guid::combine(kDungeonId, { 3, 0x517 });
    kit.oreItem = core::Guid::combine(kDungeonId, { 4, 0x517 });
    kit.enemy = core::Guid::combine(kDungeonId, { 5, 0x517 });
    kit.npc = core::Guid::combine(kDungeonId, { 6, 0x517 });
    return kit;
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
        params.voxelSize = 1.8f;
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
        meshAssetFor, kNavAsset, fx.anchors, testKit());

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

    // Gameplay anchors: the chest and every lever/barrier pair, the
    // barrier's guid deriving from its lever's (the scene's pull-time
    // inversion contract).
    const DungeonKit kit = testKit();
    REQUIRE_FALSE(fx.bake.chests.empty());
    const auto* chestRef = static_cast<const ReferenceForm*>(
        session.view(core::Guid::combine(
            kDungeonId, core::Guid { 0x5100, 0x64756E67656F6E31ull })));
    REQUIRE(chestRef != nullptr);
    CHECK(chestRef->baseForm == kit.chest);
    for (const auto& lever : fx.bake.levers) {
        const core::Guid leverRef = core::Guid::combine(
            kDungeonId,
            core::Guid { 0x5000 + lever.lockId, 0x64756E67656F6E31ull });
        const auto* leverForm =
            static_cast<const ReferenceForm*>(session.view(leverRef));
        REQUIRE(leverForm != nullptr);
        CHECK(leverForm->baseForm == kit.lever);
        const auto* barrierForm = static_cast<const ReferenceForm*>(
            session.view(barrierForLever(leverRef)));
        REQUIRE(barrierForm != nullptr);
        CHECK(barrierForm->baseForm == kit.barrier);
    }

    // Enemies: at least the goal guardian, each staged at its bake anchor.
    REQUIRE_FALSE(fx.bake.enemySpawns.empty());
    for (size_t i = 0; i < fx.bake.enemySpawns.size(); ++i) {
        const auto* enemyRef = static_cast<const ReferenceForm*>(
            session.view(core::Guid::combine(
                kDungeonId,
                core::Guid { 0x5300 + i, 0x64756E67656F6E31ull })));
        REQUIRE(enemyRef != nullptr);
        CHECK(enemyRef->baseForm == kit.enemy);
        CHECK(enemyRef->position == fx.bake.enemySpawns[i].position);
    }
}

// The SHIPPED kit file, through the real parser and resolver: a typo in
// mine-kit.toml silently strips the mine of its gameplay (the tool skips
// unresolved kit families) — this is the tripwire.
TEST_CASE("dungeon records: the shipped mine-kit resolves every kit form") {
    const auto kitPath = std::filesystem::path(__FILE__)
                             .parent_path()
                             .parent_path() /
                         "game" / "data" / "base" / "mine-kit.toml";
    REQUIRE_MESSAGE(std::filesystem::exists(kitPath),
                    "mine-kit.toml not found at ", kitPath.string());

    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    data::registerVisualFormTypes(types);
    data::registerLocFormTypes(types);
    registerWorldFormTypes(types);
    gameplay::registerFurnitureFormTypes(types);
    gameplay::registerCharacterFormTypes(types);
    quest::registerDialogueFormTypes(types);

    std::ifstream file { kitPath };
    std::stringstream content;
    content << file.rdbuf();
    const auto plugin =
        data::parsePluginToml(content.str(), types, "mine-kit");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    const auto* chest =
        data::findByEditorId<gameplay::FurnitureForm>(db, "MineChest");
    CHECK(data::findByEditorId<data::StaticForm>(db, "MineBarrier"));
    CHECK(data::findByEditorId<gameplay::FurnitureForm>(db, "MineLever"));
    REQUIRE(chest != nullptr);
    CHECK(chest->category == "container");
    CHECK(data::findByEditorId<data::MiscItemForm>(db, "OreChunk"));
    CHECK(data::findByEditorId<data::ActorForm>(db, "MineHermit"));
    CHECK(data::findByEditorId<data::LocStringForm>(db, "mine.lever.pulled"));
    CHECK(data::findByEditorId<data::LocStringForm>(db, "mine.lever.stuck"));

    // The chest's loadout children resolved as its children.
    i32 loadoutEntries = 0;
    data::forEach<gameplay::LoadoutEntryForm>(
        db, [&](const gameplay::LoadoutEntryForm& entry) {
            if (entry.parent == chest->id) {
                ++loadoutEntries;
            }
        });
    CHECK(loadoutEntries == 2);
}

TEST_CASE("dungeon records: re-Accept patches the same records, no duplicates") {
    Fixture fx;
    REQUIRE_FALSE(fx.bake.empty());
    data::EditSession session { fx.db, fx.types };

    const DungeonStageResult first = stageDungeonRecords(
        session, fx.db, fx.model, fx.bake, kDungeonId, "TestMine",
        meshAssetFor, kNavAsset, fx.anchors, testKit());
    const u32 dirtyAfterFirst = session.dirtyCount();

    const DungeonStageResult second = stageDungeonRecords(
        session, fx.db, fx.model, fx.bake, kDungeonId, "TestMine",
        meshAssetFor, kNavAsset, fx.anchors, testKit());

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

TEST_CASE("dungeon records: re-Accept updates live records and disables "
          "leftovers") {
    Fixture fx;
    REQUIRE_FALSE(fx.bake.empty());
    REQUIRE_FALSE(fx.bake.enemySpawns.empty());
    data::EditSession session { fx.db, fx.types };

    stageDungeonRecords(session, fx.db, fx.model, fx.bake, kDungeonId,
                        "TestMine", meshAssetFor, kNavAsset, fx.anchors,
                        testKit());

    // A layout change with the same identities: every anchor moves, and
    // the enemy family shrinks by one (the shrunk index must not linger
    // at its old spot).
    dungeon::DungeonBakeResult moved = fx.bake;
    const Vec3 shift { 10.0f, 0.0f, 10.0f };
    for (auto* family : { &moved.enemySpawns, &moved.oreVeins,
                          &moved.chests, &moved.patrolPoints }) {
        for (auto& anchor : *family) {
            anchor.position += shift;
        }
    }
    for (auto& torch : moved.torches) {
        torch.position += shift;
    }
    const size_t dropped = moved.enemySpawns.size() - 1;
    moved.enemySpawns.pop_back();
    stageDungeonRecords(session, fx.db, fx.model, moved, kDungeonId,
                        "TestMine", meshAssetFor, kNavAsset, fx.anchors,
                        testKit());

    // The LIVE records now serve the new layout (same guids, new fields).
    REQUIRE_FALSE(moved.torches.empty());
    const core::Guid firstTorch = core::Guid::combine(
        kDungeonId, core::Guid { 0x3000, 0x64756E67656F6E31ull });
    const auto* liveTorch = fx.db.find<ReferenceForm>(firstTorch);
    REQUIRE(liveTorch != nullptr);
    CHECK(liveTorch->position ==
          moved.torches[0].position + moved.torches[0].wallNormal * 0.3f);
    CHECK(liveTorch->enabled);

    // The dropped index is disabled, live and in the export draft.
    const core::Guid droppedEnemy = core::Guid::combine(
        kDungeonId, core::Guid { 0x5300 + dropped, 0x64756E67656F6E31ull });
    const auto* liveDropped = fx.db.find<ReferenceForm>(droppedEnemy);
    REQUIRE(liveDropped != nullptr);
    CHECK_FALSE(liveDropped->enabled);
    const auto* draftDropped =
        static_cast<const ReferenceForm*>(session.view(droppedEnemy));
    REQUIRE(draftDropped != nullptr);
    CHECK_FALSE(draftDropped->enabled);

    // A reference whose cell changed is indexed under the new cell only.
    const data::FormHandle enemyHandle = fx.db.handleOf(firstTorch);
    u32 listedIn = 0;
    for (const data::FormHandle cell : fx.model.cells()) {
        for (const data::FormHandle ref : fx.model.referencesIn(cell)) {
            if (ref.value == enemyHandle.value) {
                ++listedIn;
            }
        }
    }
    CHECK(listedIn == 1);
}
