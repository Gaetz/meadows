#include "world/dungeon/DungeonRecords.hpp"

#include <cmath>
#include <cstdio>

#include <glm/gtc/quaternion.hpp>

#include "data/forms/VisualForms.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {

// Derived-identity family for one dungeon: every record hangs off the
// dungeon guid through the same combine the cell/prefab contracts use.
core::Guid derived(const core::Guid& dungeonId, u64 index) {
    return core::Guid::combine(dungeonId,
                               core::Guid { index, 0x64756E67656F6E31ull });
}

// Fixed index bases per record family (stable across re-Accepts).
constexpr u64 kMaterial = 0x01;
constexpr u64 kTorchLight = 0x02;
constexpr u64 kMarkerKind = 0x03;
constexpr u64 kNavGrid = 0x04;
constexpr u64 kStaticBase = 0x1000;
constexpr u64 kCellRefBase = 0x2000;
constexpr u64 kTorchRefBase = 0x3000;
constexpr u64 kInMarkerBase = 0x4000;
constexpr u64 kOutMarkerBase = 0x4100;
constexpr u64 kDoorInBase = 0x4200;  // outside -> dungeon
constexpr u64 kDoorOutBase = 0x4300; // dungeon -> outside
constexpr u64 kDoorInRefBase = 0x4400;
constexpr u64 kDoorOutRefBase = 0x4500;

// Records are staged twice on purpose: LIVE in the resolved database (so
// this session can stream and travel into the dungeon right away — the
// materializeCell precedent) and as SESSION drafts (so the export ships
// them). Same guid on both sides; the next full resolve unifies them.
struct Stager {
    data::EditSession& session;
    data::FormDatabase& forms;
    WorldModel& model;

    template <typename T>
    void live(const T& form) {
        if (!forms.handleOf(form.id).isValid()) {
            const data::FormHandle handle =
                forms.add(std::make_unique<T>(form), T::staticTypeInfo());
            if constexpr (std::is_same_v<T, ReferenceForm>) {
                model.indexReference(forms, handle);
            }
        }
    }

    void ensure(const reflect::TypeInfo& type, const core::Guid& guid,
                const str& editorId) {
        if (!forms.find(guid) && !session.isCreated(guid)) {
            session.createForm(type.id, editorId, guid);
        }
    }

    template <typename T>
    void set(const reflect::TypeInfo& type, const core::Guid& guid,
             const char* field, const T& value) {
        session.setField(guid, type.findField(field)->id,
                         reflect::Value { value });
    }

    // Session staging of a fully-built reference (the only record kind we
    // emit in numbers — worth the dedicated helper). ensure() runs BEFORE
    // live(): once the live copy exists, forms.find() can no longer tell
    // "shipped by a plugin" (patch it) from "live-added by this pass"
    // (the export must CREATE it — the ensureCell shadowing rule).
    void stageReference(const ReferenceForm& ref, const str& editorId) {
        const reflect::TypeInfo& type = ReferenceForm::staticTypeInfo();
        ensure(type, ref.id, editorId);
        live(ref);
        set(type, ref.id, "baseForm", ref.baseForm);
        set(type, ref.id, "cell", ref.cell);
        set(type, ref.id, "position", ref.position);
        set(type, ref.id, "rotation", ref.rotation);
    }
};

Quat yawRotation(f32 yawDeg) {
    return glm::angleAxis(glm::radians(yawDeg), Vec3 { 0.0f, 1.0f, 0.0f });
}

} // namespace

DungeonStageResult stageDungeonRecords(
    data::EditSession& session, data::FormDatabase& forms, WorldModel& model,
    const dungeon::DungeonBakeResult& bake, const core::Guid& dungeonId,
    const str& dungeonName,
    const std::function<core::Guid(i32 cx, i32 cz)>& cellMeshAsset,
    const core::Guid& navAsset, const vector<DungeonAnchor>& anchors,
    const core::Guid& doorModel, const core::Guid& doorMaterial) {
    DungeonStageResult result;
    if (bake.empty()) {
        return result;
    }
    Stager stage { session, forms, model };
    const f32 cell = bake.cellSize;
    char editorId[96];

    // The interior worldspace; the dungeon guid IS the worldspace guid.
    WorldspaceForm ws;
    ws.id = dungeonId;
    ws.editorId = dungeonName;
    ws.cellSize = cell;
    ws.interior = true;
    ws.killZ = bake.boundsMin.y - 50.0f;
    ws.buriedBelowY = bake.boundsMax.y + 10.0f;
    const reflect::TypeInfo& wsType = WorldspaceForm::staticTypeInfo();
    stage.ensure(wsType, dungeonId, dungeonName);
    stage.live(ws);
    stage.set(wsType, dungeonId, "cellSize", ws.cellSize);
    stage.set(wsType, dungeonId, "interior", true);
    stage.set(wsType, dungeonId, "killZ", ws.killZ);
    stage.set(wsType, dungeonId, "buriedBelowY", ws.buriedBelowY);
    result.worldspace = dungeonId;
    const data::FormHandle wsHandle = forms.handleOf(dungeonId);

    // Cells: live materialization (idempotent, deterministic guid) + the
    // session shadow so the export CREATES them (the ensureCell pattern).
    const reflect::TypeInfo& cellType = CellForm::staticTypeInfo();
    const auto stageCell = [&](i32 cx, i32 cz) {
        model.materializeCell(forms, wsHandle, cx, cz);
        const core::Guid guid = cellGuidFor(dungeonId, cx, cz);
        std::snprintf(editorId, sizeof(editorId), "%s_cell_%d_%d",
                      dungeonName.c_str(), cx, cz);
        if (!session.isCreated(guid)) {
            session.createForm(cellType.id, editorId, guid);
        }
        stage.set(cellType, guid, "worldspace", dungeonId);
        stage.set(cellType, guid, "gridX", cx);
        stage.set(cellType, guid, "gridY", cz);
        stage.set(cellType, guid, "interior", true);
        return guid;
    };

    // Cavern rock material: white albedo, vertex colors do the talking.
    data::MaterialForm rock;
    rock.id = derived(dungeonId, kMaterial);
    rock.editorId = dungeonName + "_rock";
    stage.ensure(data::MaterialForm::staticTypeInfo(), rock.id,
                 rock.editorId);
    stage.live(rock);

    // One StaticForm + one ReferenceForm per cavern chunk.
    const reflect::TypeInfo& staticType = data::StaticForm::staticTypeInfo();
    for (size_t i = 0; i < bake.cellMeshes.size(); ++i) {
        const auto& cellMesh = bake.cellMeshes[i];
        const core::Guid cellGuid = stageCell(cellMesh.cx, cellMesh.cz);

        data::StaticForm cavern;
        cavern.id = derived(dungeonId, kStaticBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_cavern_%d_%d",
                      dungeonName.c_str(), cellMesh.cx, cellMesh.cz);
        cavern.editorId = editorId;
        cavern.displayName = editorId;
        cavern.model = cellMeshAsset(cellMesh.cx, cellMesh.cz);
        cavern.material = rock.id;
        cavern.collides = true;
        cavern.snapToGround = false;
        stage.ensure(staticType, cavern.id, cavern.editorId);
        stage.live(cavern);
        stage.set(staticType, cavern.id, "displayName", cavern.displayName);
        stage.set(staticType, cavern.id, "model", cavern.model);
        stage.set(staticType, cavern.id, "material", cavern.material);
        stage.set(staticType, cavern.id, "collides", true);
        stage.set(staticType, cavern.id, "snapToGround", false);

        ReferenceForm ref;
        ref.id = derived(dungeonId, kCellRefBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_cavern_ref_%d_%d",
                      dungeonName.c_str(), cellMesh.cx, cellMesh.cz);
        ref.editorId = editorId;
        ref.baseForm = cavern.id;
        ref.cell = cellGuid;
        ref.position = { static_cast<f32>(cellMesh.cx) * cell, 0.0f,
                         static_cast<f32>(cellMesh.cz) * cell };
        stage.stageReference(ref, ref.editorId);
        ++result.cellCount;
    }

    // The mine torch: one LightForm, one reference per baked anchor.
    data::LightForm torch;
    torch.id = derived(dungeonId, kTorchLight);
    torch.editorId = dungeonName + "_torch";
    torch.color = { 1.0f, 0.62f, 0.32f };
    torch.intensity = 6.0f;
    torch.radius = 11.0f;
    torch.flicker = 0.5f;
    torch.castsShadow = false;
    const reflect::TypeInfo& lightType = data::LightForm::staticTypeInfo();
    stage.ensure(lightType, torch.id, torch.editorId);
    stage.live(torch);
    stage.set(lightType, torch.id, "color", torch.color);
    stage.set(lightType, torch.id, "intensity", torch.intensity);
    stage.set(lightType, torch.id, "radius", torch.radius);
    stage.set(lightType, torch.id, "flicker", torch.flicker);
    stage.set(lightType, torch.id, "castsShadow", false);
    for (size_t i = 0; i < bake.torches.size(); ++i) {
        const auto& anchor = bake.torches[i];
        const i32 cx = static_cast<i32>(std::floor(anchor.position.x / cell));
        const i32 cz = static_cast<i32>(std::floor(anchor.position.z / cell));
        ReferenceForm ref;
        ref.id = derived(dungeonId, kTorchRefBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_torch_%zu",
                      dungeonName.c_str(), i);
        ref.editorId = editorId;
        ref.baseForm = torch.id;
        ref.cell = stageCell(cx, cz);
        ref.position = anchor.position + anchor.wallNormal * 0.3f;
        stage.stageReference(ref, ref.editorId);
        ++result.torchCount;
    }

    // Navigation record: the scene resolves it on interior travel.
    NavGridForm nav;
    nav.id = derived(dungeonId, kNavGrid);
    nav.editorId = dungeonName + "_nav";
    nav.worldspace = dungeonId;
    nav.asset = navAsset;
    const reflect::TypeInfo& navType = NavGridForm::staticTypeInfo();
    stage.ensure(navType, nav.id, nav.editorId);
    stage.live(nav);
    stage.set(navType, nav.id, "worldspace", dungeonId);
    stage.set(navType, nav.id, "asset", navAsset);
    result.navGridRecord = nav.id;

    // Doors, per anchor: arrival markers are ReferenceForms of one shared
    // MarkerForm; each DoorForm targets the OTHER side's marker reference
    // (the transition resolves entirely from records).
    MarkerForm marker;
    marker.id = derived(dungeonId, kMarkerKind);
    marker.editorId = dungeonName + "_door_target";
    stage.ensure(MarkerForm::staticTypeInfo(), marker.id, marker.editorId);
    stage.live(marker);

    const reflect::TypeInfo& doorType = DoorForm::staticTypeInfo();
    const i32 entranceCx =
        static_cast<i32>(std::floor(bake.entrancePos.x / cell));
    const i32 entranceCz =
        static_cast<i32>(std::floor(bake.entrancePos.z / cell));
    const core::Guid entranceCell = stageCell(entranceCx, entranceCz);
    const f32 insideYaw = glm::degrees(
        std::atan2(bake.entranceDir.x, bake.entranceDir.z));

    const auto stageDoorForm = [&](const core::Guid& guid, const str& name,
                                   const str& display,
                                   const core::Guid& target) {
        DoorForm door;
        door.id = guid;
        door.editorId = name;
        door.displayName = display;
        door.model = doorModel;
        door.material = doorMaterial;
        door.targetMarker = target;
        stage.ensure(doorType, guid, name);
        stage.live(door);
        stage.set(doorType, guid, "displayName", display);
        stage.set(doorType, guid, "model", doorModel);
        stage.set(doorType, guid, "material", doorMaterial);
        stage.set(doorType, guid, "targetMarker", target);
    };

    for (size_t i = 0; i < anchors.size(); ++i) {
        const DungeonAnchor& anchor = anchors[i];

        // Arrival spots: inside (a step into the entrance room) and
        // outside (a step in front of the outside door).
        ReferenceForm arriveIn;
        arriveIn.id = derived(dungeonId, kInMarkerBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_arrive_in_%zu",
                      dungeonName.c_str(), i);
        arriveIn.editorId = editorId;
        arriveIn.baseForm = marker.id;
        arriveIn.cell = entranceCell;
        arriveIn.position = bake.entrancePos - bake.entranceDir * 1.5f;
        arriveIn.rotation = yawRotation(insideYaw + 180.0f);
        stage.stageReference(arriveIn, arriveIn.editorId);

        const Vec3 outFacing { std::sin(glm::radians(anchor.yawDeg)), 0.0f,
                               std::cos(glm::radians(anchor.yawDeg)) };
        ReferenceForm arriveOut;
        arriveOut.id = derived(dungeonId, kOutMarkerBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_arrive_out_%zu",
                      dungeonName.c_str(), i);
        arriveOut.editorId = editorId;
        arriveOut.baseForm = marker.id;
        arriveOut.cell = anchor.cell;
        arriveOut.position = anchor.doorPos + outFacing * 1.5f;
        arriveOut.rotation = yawRotation(anchor.yawDeg + 180.0f);
        stage.stageReference(arriveOut, arriveOut.editorId);

        // The door pair.
        std::snprintf(editorId, sizeof(editorId), "%s_entrance_%zu",
                      dungeonName.c_str(), i);
        stageDoorForm(derived(dungeonId, kDoorInBase + i), editorId,
                      dungeonName + " entrance", arriveIn.id);
        std::snprintf(editorId, sizeof(editorId), "%s_exit_%zu",
                      dungeonName.c_str(), i);
        stageDoorForm(derived(dungeonId, kDoorOutBase + i), editorId,
                      dungeonName + " exit", arriveOut.id);

        ReferenceForm doorInRef;
        doorInRef.id = derived(dungeonId, kDoorInRefBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_entrance_ref_%zu",
                      dungeonName.c_str(), i);
        doorInRef.editorId = editorId;
        doorInRef.baseForm = derived(dungeonId, kDoorInBase + i);
        doorInRef.cell = anchor.cell;
        doorInRef.position = anchor.doorPos;
        doorInRef.rotation = yawRotation(anchor.yawDeg);
        stage.stageReference(doorInRef, doorInRef.editorId);
        result.outsideDoorRefs.push_back(doorInRef.id);

        ReferenceForm doorOutRef;
        doorOutRef.id = derived(dungeonId, kDoorOutRefBase + i);
        std::snprintf(editorId, sizeof(editorId), "%s_exit_ref_%zu",
                      dungeonName.c_str(), i);
        doorOutRef.editorId = editorId;
        doorOutRef.baseForm = derived(dungeonId, kDoorOutBase + i);
        doorOutRef.cell = entranceCell;
        doorOutRef.position = bake.entrancePos;
        doorOutRef.rotation = yawRotation(insideYaw);
        stage.stageReference(doorOutRef, doorOutRef.editorId);
    }

    LOG_INFO("Dungeon '{}': staged {} cells, {} torches, {} anchors "
             "(live + session)",
             dungeonName, result.cellCount, result.torchCount,
             anchors.size());
    return result;
}

} // namespace world
