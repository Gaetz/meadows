#include <doctest/doctest.h>

#include <filesystem>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "world/terrain/MapRecords.hpp"
#include "world/terrain/TerrainRegions.hpp"
#include "world/worldspace/WorldForms.hpp"

using namespace world;

namespace {

const core::Guid kMapGuid =
    *core::Guid::fromString("aaaa0001-0000-4000-8000-000000000001");
const core::Guid kSliceAssetA =
    *core::Guid::fromString("aaaa0002-0000-4000-8000-000000000001");
const core::Guid kSliceAssetB =
    *core::Guid::fromString("aaaa0002-0000-4000-8000-000000000002");

render::TerrainRegion makeRegion(f32 originX, f32 originZ, f32 base) {
    render::TerrainRegion r;
    r.width = 9;
    r.height = 9;
    r.originX = originX;
    r.originZ = originZ;
    r.texelSize = 8.0f;
    r.edgeBlend = 0.0f;
    r.heights.resize(81);
    for (u32 i = 0; i < 81; ++i) {
        r.heights[i] = base + static_cast<f32>(i) * 0.25f;
    }
    return r;
}

vector<MapSliceRecord> testSlices() {
    return {
        { 0, 0, kSliceAssetA, 1.5f, 40.0f, 2 },
        { 1, 0, kSliceAssetB, 0.5f, 60.0f, 3 },
    };
}

render::terraingen::Lake testLake() {
    render::terraingen::Lake lake;
    lake.level = 55.5f;
    lake.minX = 10.0f;
    lake.minZ = 20.0f;
    lake.maxX = 40.0f;
    lake.maxZ = 60.0f;
    return lake;
}

render::terraingen::River testRiver() {
    render::terraingen::River river;
    river.points = { { 5.0f, 5.0f, 52.0f, 3.0f },
                     { 15.0f, 8.0f, 51.0f, 4.0f },
                     { 25.0f, 12.0f, 50.0f, 5.0f } };
    return river;
}

} // namespace

TEST_CASE("map records: bake -> stage -> export -> resolve -> "
          "buildTerrainBase round-trip") {
    const auto dir =
        std::filesystem::temp_directory_path() / "meadows-maprec";
    std::filesystem::create_directories(dir);
    const render::TerrainRegion regionA = makeRegion(0.0f, 0.0f, 80.0f);
    const render::TerrainRegion regionB = makeRegion(64.0f, 0.0f, 120.0f);
    REQUIRE(writeTrgFile(dir / "slice_0_0.trg", regionA));
    REQUIRE(writeTrgFile(dir / "slice_1_0.trg", regionB));

    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    registerWorldFormTypes(types);
    data::FormDatabase db;
    data::EditSession session { db, types };

    const MapStageResult staged = stageMapRecords(
        session, db, kMapGuid, "TestMap", 2, -1, 24576.0f, 777u,
        testSlices(), { testLake() }, { testRiver() });
    CHECK(staged.worldspace == kMapGuid);
    CHECK(staged.slices == 2);
    CHECK(staged.lakes == 1);
    CHECK(staged.rivers == 1);

    // The export is an ORDINARY plugin; resolving it alone rebuilds the
    // whole map world (the §5 "a mod adds a map" contract).
    const data::Plugin plugin = session.exportPlugin(
        *core::Guid::fromString("aaaa00ff-0000-4000-8000-000000000001"),
        "test-map");
    data::FormDatabase resolved;
    data::resolve({ &plugin }, types, resolved);

    const auto* space = resolved.find<WorldspaceForm>(kMapGuid);
    REQUIRE(space != nullptr);
    CHECK(space->bounded);
    CHECK(space->mapX == 2);
    CHECK(space->mapZ == -1);
    CHECK(space->mapSize == doctest::Approx(24576.0f));
    CHECK(space->mapSeed == 777u);

    // The record set IS the slice list: buildTerrainBase scoped to the
    // map reloads the slices bit-identically.
    assets::AssetDatabase assetDb;
    assetDb.add(kSliceAssetA, dir, "slice_0_0.trg");
    assetDb.add(kSliceAssetB, dir, "slice_1_0.trg");
    const auto base = buildTerrainBase(resolved, assetDb,
                                       WorldspaceFilter { kMapGuid });
    REQUIRE(base->regions.size() == 2);
    for (const auto& region : base->regions) {
        const render::TerrainRegion& original =
            region->originX == regionA.originX ? regionA : regionB;
        CHECK(region->heights == original.heights); // bit-exact
    }
    // A detail knob travelled through the record, not the asset.
    bool sawDetail = false;
    for (const auto& region : base->regions) {
        if (region->detailAmplitude == doctest::Approx(1.5f)) {
            sawDetail = true;
        }
    }
    CHECK(sawDetail);

    // Scoping: another bounded map sees NONE of these slices.
    const auto other = buildTerrainBase(
        resolved, assetDb,
        WorldspaceFilter { *core::Guid::fromString(
            "bbbb0001-0000-4000-8000-000000000001") });
    CHECK(other->regions.empty());

    // Water records: scoped to the map, values carried through.
    u32 lakes = 0;
    data::forEach<WaterBodyForm>(
        resolved, [&](const WaterBodyForm& lake) {
            ++lakes;
            CHECK(lake.worldspace == kMapGuid);
            CHECK(lake.surfaceLevel == doctest::Approx(55.5f));
        });
    CHECK(lakes == 1);
    u32 riverPoints = 0;
    core::Guid riverGuid;
    data::forEach<RiverForm>(resolved, [&](const RiverForm& river) {
        CHECK(river.worldspace == kMapGuid);
        riverGuid = river.id;
    });
    data::forEach<RiverPointForm>(
        resolved, [&](const RiverPointForm& pt) {
            ++riverPoints;
            CHECK(pt.parent == riverGuid);
        });
    CHECK(riverPoints == 3);
}

TEST_CASE("map records: riverThin keeps the ends, drops dense "
          "midpoints") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    registerWorldFormTypes(types);
    data::FormDatabase db;
    data::EditSession session { db, types };

    render::terraingen::River dense;
    for (i32 i = 0; i < 10; ++i) {
        dense.points.push_back(
            { static_cast<f32>(i) * 10.0f, 0.0f, 60.0f - i, 3.0f });
    }
    stageMapRecords(session, db, kMapGuid, "ThinMap", 0, 0, 24576.0f,
                    1u, {}, {}, { dense }, 48.0f);
    const data::Plugin plugin = session.exportPlugin(
        *core::Guid::fromString("aaaa00fe-0000-4000-8000-000000000001"),
        "thin-map");
    data::FormDatabase resolved;
    data::resolve({ &plugin }, types, resolved);
    vector<f32> xs;
    data::forEach<RiverPointForm>(
        resolved, [&](const RiverPointForm& pt) {
            xs.push_back(pt.position.x);
        });
    // 10 points at 10 m: the first, the first >= 48 m on (x=50), and
    // the LAST (always kept even under spacing).
    CHECK(xs.size() == 3);
    CHECK(std::count(xs.begin(), xs.end(), 0.0f) == 1);
    CHECK(std::count(xs.begin(), xs.end(), 90.0f) == 1);
}

TEST_CASE("map records: re-stage patches the same records, no duplicates") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    registerWorldFormTypes(types);
    data::FormDatabase db;
    data::EditSession session { db, types };

    const MapStageResult first = stageMapRecords(
        session, db, kMapGuid, "TestMap", 0, 0, 24576.0f, 1u,
        testSlices(), { testLake() }, { testRiver() });
    const u32 dirtyAfterFirst = session.dirtyCount();

    const MapStageResult second = stageMapRecords(
        session, db, kMapGuid, "TestMap", 0, 0, 24576.0f, 1u,
        testSlices(), { testLake() }, { testRiver() });
    CHECK(second.worldspace == first.worldspace);
    CHECK(session.dirtyCount() == dirtyAfterFirst);
}
