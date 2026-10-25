#include "world/dungeon/DungeonRecords.hpp"

#include <cmath>
#include <cstdio>

#include <glm/gtc/quaternion.hpp>

#include "data/forms/VisualForms.hpp"
#include "data/plugins/RecordStager.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {

using dungeon::AnchorKind;
using BakeAnchor = dungeon::DungeonBakeResult::Anchor;

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
constexpr u64 kLeverRefBase = 0x5000; // + lockId (unique per dungeon)
constexpr u64 kChestRefBase = 0x5100;
constexpr u64 kVeinRefBase = 0x5200;
constexpr u64 kEnemyRefBase = 0x5300;
constexpr u64 kNpcRefBase = 0x5400;
constexpr u64 kPatrolMarker = 0x06;    // the shared "patrol" MarkerForm
constexpr u64 kPatrolRefBase = 0x5500; // NPC wander anchors, per room

// How each anchor family becomes references, one row per AnchorKind in
// enum order (pinned by static_assert) — also the STAGING order, which
// the export's record order follows. `indexed` families key their guid
// on the anchor's index within its kind (kRefBase + i) and are staged by
// the generic loop; the lock pair is keyed on the lockId instead
// (barrierForLever) and keeps its own step. `gate` is the kit slot that
// must resolve for the family to exist, `base` the kit slot it is
// instantiated from — null base: the shared patrol MarkerForm.
struct AnchorEmit {
    AnchorKind kind;
    u64 refBase;
    const char* tag; // editorId suffix: <dungeon>_<tag>_<i>
    core::Guid DungeonKit::* gate;
    core::Guid DungeonKit::* base;
    bool indexed;
};
constexpr AnchorEmit kAnchorEmits[] = {
    { AnchorKind::Barrier, 0, "barrier", &DungeonKit::barrier,
      &DungeonKit::barrier, false },
    { AnchorKind::Lever, kLeverRefBase, "lever", &DungeonKit::lever,
      &DungeonKit::lever, false },
    { AnchorKind::Chest, kChestRefBase, "chest", &DungeonKit::chest,
      &DungeonKit::chest, true },
    { AnchorKind::OreVein, kVeinRefBase, "vein", &DungeonKit::oreItem,
      &DungeonKit::oreItem, true },
    { AnchorKind::EnemySpawn, kEnemyRefBase, "enemy", &DungeonKit::enemy,
      &DungeonKit::enemy, true },
    { AnchorKind::NpcSpawn, kNpcRefBase, "npc", &DungeonKit::npc,
      &DungeonKit::npc, true },
    { AnchorKind::PatrolPoint, kPatrolRefBase, "patrol", &DungeonKit::enemy,
      nullptr, true },
};
static_assert(sizeof(kAnchorEmits) / sizeof(kAnchorEmits[0]) ==
              static_cast<size_t>(AnchorKind::Count));

constexpr const AnchorEmit& anchorEmit(AnchorKind kind) {
    return kAnchorEmits[static_cast<size_t>(kind)];
}

Quat yawRotation(f32 yawDeg) {
    return glm::angleAxis(glm::radians(yawDeg), Vec3 { 0.0f, 1.0f, 0.0f });
}

// The cell-grid coordinate a world position falls in.
i32 cellIndex(f32 coord, f32 cellSize) {
    return static_cast<i32>(std::floor(coord / cellSize));
}

// The mutable state one Accept threads through its stages, in staging
// order. Records are staged twice on purpose: LIVE in the resolved
// database (so this session can stream and travel into the dungeon right
// away) and as SESSION drafts (so the export ships them) —
// data::RecordStager. `wsHandle` is set by the worldspace stage and read
// by every cell materialization after it; `editorId` is the shared
// scratch buffer every record name is formatted into.
struct Staging {
    const DungeonStageInputs& in;
    data::RecordStager stage;
    data::FormHandle wsHandle;
    f32 cell;
    char editorId[96];
    u32 leftovers;
};

// A live reference keeps the world model's cell index in step.
void liveReference(Staging& st, const ReferenceForm& ref) {
    data::FormDatabase& forms = st.in.forms;
    WorldModel& model = st.in.model;
    st.stage.live(forms, ref,
                  [&](data::FormHandle handle, const ReferenceForm* before) {
                      if (!before) {
                          model.indexReference(forms, handle);
                      } else if (before->cell != ref.cell) {
                          model.unindexReference(forms.handleOf(before->cell),
                                                 handle);
                          model.indexReference(forms, handle);
                      }
                  });
}

// ensure() runs BEFORE live(): the ensureCell shadowing rule.
void stageReference(Staging& st, const ReferenceForm& ref) {
    st.stage.ensure(ReferenceForm::staticTypeInfo(), ref.id, ref.editorId);
    liveReference(st, ref);
    st.stage.setAll(ref);
}

// Cells: live materialization (idempotent, deterministic guid) + the
// session shadow so the export CREATES them (the ensureCell pattern).
core::Guid stageCell(Staging& st, i32 cx, i32 cz) {
    const DungeonStageInputs& in = st.in;
    in.model.materializeCell(in.forms, st.wsHandle, cx, cz);
    const core::Guid guid = cellGuidFor(in.dungeonId, cx, cz);
    std::snprintf(st.editorId, sizeof(st.editorId), "%s_cell_%d_%d",
                  in.dungeonName.c_str(), cx, cz);
    const reflect::TypeInfo& cellType = CellForm::staticTypeInfo();
    if (!in.session.isCreated(guid)) {
        in.session.createForm(cellType.id, st.editorId, guid);
    }
    st.stage.set(cellType, guid, "worldspace", in.dungeonId);
    st.stage.set(cellType, guid, "gridX", cx);
    st.stage.set(cellType, guid, "gridY", cz);
    st.stage.set(cellType, guid, "interior", true);
    return guid;
}

// The interior worldspace; the dungeon guid IS the worldspace guid.
void stageWorldspace(Staging& st, DungeonStageResult& result) {
    const DungeonStageInputs& in = st.in;
    WorldspaceForm ws;
    ws.id = in.dungeonId;
    ws.editorId = in.dungeonName;
    ws.cellSize = st.cell;
    ws.interior = true;
    ws.killZ = in.bake.boundsMin.y - 50.0f;
    ws.buriedBelowY = in.bake.boundsMax.y + 10.0f;
    const reflect::TypeInfo& wsType = WorldspaceForm::staticTypeInfo();
    st.stage.ensure(wsType, in.dungeonId, in.dungeonName);
    st.stage.live(in.forms, ws);
    st.stage.set(wsType, in.dungeonId, "cellSize", ws.cellSize);
    st.stage.set(wsType, in.dungeonId, "interior", true);
    st.stage.set(wsType, in.dungeonId, "killZ", ws.killZ);
    st.stage.set(wsType, in.dungeonId, "buriedBelowY", ws.buriedBelowY);
    result.worldspace = in.dungeonId;
    st.wsHandle = in.forms.handleOf(in.dungeonId);
}

// The cavern rock material, then one StaticForm + one ReferenceForm per
// cavern chunk (each chunk materializing its cell first).
void stageCaverns(Staging& st, DungeonStageResult& result) {
    const DungeonStageInputs& in = st.in;

    // Cavern rock material: white albedo, vertex colors do the talking.
    data::MaterialForm rock;
    rock.id = derived(in.dungeonId, kMaterial);
    rock.editorId = in.dungeonName + "_rock";
    st.stage.ensure(data::MaterialForm::staticTypeInfo(), rock.id,
                    rock.editorId);
    st.stage.live(in.forms, rock);

    const reflect::TypeInfo& staticType = data::StaticForm::staticTypeInfo();
    for (size_t i = 0; i < in.bake.cellMeshes.size(); ++i) {
        const auto& cellMesh = in.bake.cellMeshes[i];
        const core::Guid cellGuid = stageCell(st, cellMesh.cx, cellMesh.cz);

        data::StaticForm cavern;
        cavern.id = derived(in.dungeonId, kStaticBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_cavern_%d_%d",
                      in.dungeonName.c_str(), cellMesh.cx, cellMesh.cz);
        cavern.editorId = st.editorId;
        cavern.displayName = st.editorId;
        cavern.model = in.cellMeshAsset(cellMesh.cx, cellMesh.cz);
        cavern.material = rock.id;
        cavern.collides = true;
        cavern.snapToGround = false;
        st.stage.ensure(staticType, cavern.id, cavern.editorId);
        st.stage.live(in.forms, cavern);
        st.stage.set(staticType, cavern.id, "displayName",
                     cavern.displayName);
        st.stage.set(staticType, cavern.id, "model", cavern.model);
        st.stage.set(staticType, cavern.id, "material", cavern.material);
        st.stage.set(staticType, cavern.id, "collides", true);
        st.stage.set(staticType, cavern.id, "snapToGround", false);

        ReferenceForm ref;
        ref.id = derived(in.dungeonId, kCellRefBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_cavern_ref_%d_%d",
                      in.dungeonName.c_str(), cellMesh.cx, cellMesh.cz);
        ref.editorId = st.editorId;
        ref.baseForm = cavern.id;
        ref.cell = cellGuid;
        ref.position = { static_cast<f32>(cellMesh.cx) * st.cell, 0.0f,
                         static_cast<f32>(cellMesh.cz) * st.cell };
        stageReference(st, ref);
        ++result.cellCount;
    }
}

// The mine torch: one LightForm, one reference per baked anchor.
void stageTorches(Staging& st, DungeonStageResult& result) {
    const DungeonStageInputs& in = st.in;
    data::LightForm torch;
    torch.id = derived(in.dungeonId, kTorchLight);
    torch.editorId = in.dungeonName + "_torch";
    torch.color = { 1.0f, 0.62f, 0.32f };
    torch.intensity = 2.9f;
    torch.radius = 11.0f;
    torch.flicker = 0.5f;
    torch.castsShadow = false;
    const reflect::TypeInfo& lightType = data::LightForm::staticTypeInfo();
    st.stage.ensure(lightType, torch.id, torch.editorId);
    st.stage.live(in.forms, torch);
    st.stage.set(lightType, torch.id, "color", torch.color);
    st.stage.set(lightType, torch.id, "intensity", torch.intensity);
    st.stage.set(lightType, torch.id, "radius", torch.radius);
    st.stage.set(lightType, torch.id, "flicker", torch.flicker);
    st.stage.set(lightType, torch.id, "castsShadow", false);
    for (size_t i = 0; i < in.bake.torches.size(); ++i) {
        const auto& anchor = in.bake.torches[i];
        const i32 cx = cellIndex(anchor.position.x, st.cell);
        const i32 cz = cellIndex(anchor.position.z, st.cell);
        ReferenceForm ref;
        ref.id = derived(in.dungeonId, kTorchRefBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_torch_%zu",
                      in.dungeonName.c_str(), i);
        ref.editorId = st.editorId;
        ref.baseForm = torch.id;
        ref.cell = stageCell(st, cx, cz);
        ref.position = anchor.position + anchor.wallNormal * 0.3f;
        stageReference(st, ref);
        ++result.torchCount;
    }
}

// Navigation record: the scene resolves it on interior travel.
void stageNav(Staging& st, DungeonStageResult& result) {
    const DungeonStageInputs& in = st.in;
    NavGridForm nav;
    nav.id = derived(in.dungeonId, kNavGrid);
    nav.editorId = in.dungeonName + "_nav";
    nav.worldspace = in.dungeonId;
    nav.asset = in.navAsset;
    const reflect::TypeInfo& navType = NavGridForm::staticTypeInfo();
    st.stage.ensure(navType, nav.id, nav.editorId);
    st.stage.live(in.forms, nav);
    st.stage.set(navType, nav.id, "worldspace", in.dungeonId);
    st.stage.set(navType, nav.id, "asset", in.navAsset);
    result.navGridRecord = nav.id;
}

// One door leaf, dressed from the kit's door model/material, leading to the
// OTHER side's arrival marker reference.
void stageDoorForm(Staging& st, const core::Guid& guid, const str& name,
                   const str& display, const core::Guid& target) {
    const DungeonStageInputs& in = st.in;
    const reflect::TypeInfo& doorType = DoorForm::staticTypeInfo();
    DoorForm door;
    door.id = guid;
    door.editorId = name;
    door.displayName = display;
    door.model = in.doorModel;
    door.material = in.doorMaterial;
    door.targetMarker = target;
    st.stage.ensure(doorType, guid, name);
    st.stage.live(in.forms, door);
    st.stage.set(doorType, guid, "displayName", display);
    st.stage.set(doorType, guid, "model", in.doorModel);
    st.stage.set(doorType, guid, "material", in.doorMaterial);
    st.stage.set(doorType, guid, "targetMarker", target);
}

// Doors, per anchor: arrival markers are ReferenceForms of one shared
// MarkerForm; each DoorForm targets the OTHER side's marker reference
// (the transition resolves entirely from records).
void stageDoors(Staging& st, DungeonStageResult& result) {
    const DungeonStageInputs& in = st.in;
    const dungeon::DungeonBakeResult& bake = in.bake;

    MarkerForm marker;
    marker.id = derived(in.dungeonId, kMarkerKind);
    marker.editorId = in.dungeonName + "_door_target";
    st.stage.ensure(MarkerForm::staticTypeInfo(), marker.id,
                    marker.editorId);
    st.stage.live(in.forms, marker);

    const i32 entranceCx = cellIndex(bake.entrancePos.x, st.cell);
    const i32 entranceCz = cellIndex(bake.entrancePos.z, st.cell);
    const core::Guid entranceCell = stageCell(st, entranceCx, entranceCz);
    const f32 insideYaw =
        glm::degrees(std::atan2(bake.entranceDir.x, bake.entranceDir.z));

    for (size_t i = 0; i < in.anchors.size(); ++i) {
        const DungeonAnchor& anchor = in.anchors[i];

        // Arrival spots: inside (a step into the entrance room) and
        // outside (a step in front of the outside door).
        ReferenceForm arriveIn;
        arriveIn.id = derived(in.dungeonId, kInMarkerBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_arrive_in_%zu",
                      in.dungeonName.c_str(), i);
        arriveIn.editorId = st.editorId;
        arriveIn.baseForm = marker.id;
        arriveIn.cell = entranceCell;
        arriveIn.position = bake.entrancePos - bake.entranceDir * 1.5f;
        arriveIn.rotation = yawRotation(insideYaw + 180.0f);
        stageReference(st, arriveIn);

        const Vec3 outFacing { std::sin(glm::radians(anchor.yawDeg)), 0.0f,
                               std::cos(glm::radians(anchor.yawDeg)) };
        ReferenceForm arriveOut;
        arriveOut.id = derived(in.dungeonId, kOutMarkerBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_arrive_out_%zu",
                      in.dungeonName.c_str(), i);
        arriveOut.editorId = st.editorId;
        arriveOut.baseForm = marker.id;
        arriveOut.cell = anchor.cell;
        arriveOut.position = anchor.doorPos + outFacing * 1.5f;
        arriveOut.rotation = yawRotation(anchor.yawDeg + 180.0f);
        stageReference(st, arriveOut);

        // The door pair.
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_entrance_%zu",
                      in.dungeonName.c_str(), i);
        stageDoorForm(st, derived(in.dungeonId, kDoorInBase + i), st.editorId,
                      in.dungeonName + " entrance", arriveIn.id);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_exit_%zu",
                      in.dungeonName.c_str(), i);
        stageDoorForm(st, derived(in.dungeonId, kDoorOutBase + i),
                      st.editorId, in.dungeonName + " exit", arriveOut.id);

        ReferenceForm doorInRef;
        doorInRef.id = derived(in.dungeonId, kDoorInRefBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_entrance_ref_%zu",
                      in.dungeonName.c_str(), i);
        doorInRef.editorId = st.editorId;
        doorInRef.baseForm = derived(in.dungeonId, kDoorInBase + i);
        doorInRef.cell = anchor.cell;
        doorInRef.position = anchor.doorPos;
        doorInRef.rotation = yawRotation(anchor.yawDeg);
        stageReference(st, doorInRef);
        result.outsideDoorRefs.push_back(doorInRef.id);

        ReferenceForm doorOutRef;
        doorOutRef.id = derived(in.dungeonId, kDoorOutRefBase + i);
        std::snprintf(st.editorId, sizeof(st.editorId), "%s_exit_ref_%zu",
                      in.dungeonName.c_str(), i);
        doorOutRef.editorId = st.editorId;
        doorOutRef.baseForm = derived(in.dungeonId, kDoorOutBase + i);
        doorOutRef.cell = entranceCell;
        doorOutRef.position = bake.entrancePos;
        doorOutRef.rotation = yawRotation(insideYaw);
        stageReference(st, doorOutRef);
    }
}

// One gameplay anchor as a reference of a kit form, in the cell under it.
void stageAnchored(Staging& st, const BakeAnchor& anchor,
                   const core::Guid& refGuid, const core::Guid& baseForm,
                   const char* tag, size_t i, const Vec3& scale) {
    const i32 cx = cellIndex(anchor.position.x, st.cell);
    const i32 cz = cellIndex(anchor.position.z, st.cell);
    ReferenceForm ref;
    ref.id = refGuid;
    std::snprintf(st.editorId, sizeof(st.editorId), "%s_%s_%zu",
                  st.in.dungeonName.c_str(), tag, i);
    ref.editorId = st.editorId;
    ref.baseForm = baseForm;
    ref.cell = stageCell(st, cx, cz);
    ref.position = anchor.position;
    ref.rotation = yawRotation(anchor.yawDeg);
    ref.scale = scale;
    stageReference(st, ref);
}

// The lock pairs from the mission semantics (bake.populateAnchors): the
// lever's guid is keyed on the lockId; its barrier derives from the lever
// reference (barrierForLever — the scene inverts the pairing at pull
// time). Both kit slots must resolve for the family to exist.
void stageLocks(Staging& st, const vector<BakeAnchor>& levers) {
    const DungeonStageInputs& in = st.in;
    const AnchorEmit& leverEmit = anchorEmit(AnchorKind::Lever);
    const AnchorEmit& barrierEmit = anchorEmit(AnchorKind::Barrier);
    if (!(in.kit.*leverEmit.gate).isValid() ||
        !(in.kit.*barrierEmit.gate).isValid()) {
        return;
    }
    const Vec3 unit { 1.0f };
    const vector<BakeAnchor> barriers = in.bake.anchorsOf(AnchorKind::Barrier);
    for (size_t i = 0; i < levers.size(); ++i) {
        const auto& lever = levers[i];
        const core::Guid leverRef =
            derived(in.dungeonId, leverEmit.refBase + lever.lockId);
        stageAnchored(st, lever, leverRef, in.kit.*leverEmit.base,
                      leverEmit.tag, i, unit);
        for (const auto& barrier : barriers) {
            if (barrier.lockId == lever.lockId) {
                // Stretched across the ~5 m tunnel (wider on a turn,
                // bake.width); hand-retouchable like any reference.
                // ONE barrier per lock: barrierForLever(leverRef) is
                // the whole identity — a second match would silently
                // overwrite the first and leave its tunnel open.
                stageAnchored(st, barrier, barrierForLever(leverRef),
                              in.kit.*barrierEmit.base, barrierEmit.tag, i,
                              { 3.0f * barrier.width, 2.5f, 1.0f });
                break;
            }
        }
    }
}

// Patrol anchors: without loaded "patrol" markers the NPCs never wander —
// frozen bandits neither look around nor spot the player. The shared
// MarkerForm is staged with its family.
core::Guid stagePatrolMarker(Staging& st) {
    const DungeonStageInputs& in = st.in;
    MarkerForm patrol;
    patrol.id = derived(in.dungeonId, kPatrolMarker);
    patrol.editorId = in.dungeonName + "_patrol";
    patrol.kind = "patrol";
    const reflect::TypeInfo& markerType = MarkerForm::staticTypeInfo();
    st.stage.ensure(markerType, patrol.id, patrol.editorId);
    st.stage.live(in.forms, patrol);
    st.stage.set(markerType, patrol.id, "kind", patrol.kind);
    return patrol.id;
}

// The indexed anchor families (bake.populateAnchors), instantiated from
// the mine kit per kAnchorEmits in table (= enum) order; a null kit guid
// skips its family.
void stageKitAnchors(Staging& st) {
    const DungeonStageInputs& in = st.in;
    const Vec3 unit { 1.0f };
    for (const AnchorEmit& row : kAnchorEmits) {
        if (!row.indexed || !(in.kit.*row.gate).isValid()) {
            continue;
        }
        const vector<BakeAnchor> family = in.bake.anchorsOf(row.kind);
        core::Guid base;
        if (row.base) {
            base = in.kit.*row.base;
        } else if (!family.empty()) {
            base = stagePatrolMarker(st);
        }
        for (size_t i = 0; i < family.size(); ++i) {
            stageAnchored(st, family[i], derived(in.dungeonId, row.refBase + i),
                          base, row.tag, i, unit);
        }
    }
}

// Disables one leftover reference, live + session. False when no reference
// record carries that guid (the end of a contiguous family).
bool disableReference(Staging& st, const core::Guid& guid) {
    data::FormDatabase& forms = st.in.forms;
    const reflect::TypeInfo& refType = ReferenceForm::staticTypeInfo();
    const data::FormHandle handle = forms.handleOf(guid);
    if (!handle.isValid() || !forms.typeOf(handle)->isA(refType.id)) {
        return false;
    }
    auto* ref = static_cast<ReferenceForm*>(forms.getMutable(handle));
    if (ref->enabled) {
        ref->enabled = false;
        st.stage.set(refType, guid, "enabled", false);
        ++st.leftovers;
    }
    return true;
}

// Disables one contiguous guid family past its new end.
void disablePast(Staging& st, u64 base, size_t from) {
    for (size_t i = from;
         disableReference(st, derived(st.in.dungeonId, base + i)); ++i) {
    }
}

// A re-Accept can shrink a family (fewer enemies, fewer cells): the
// shipped records past the new count would linger with their OLD
// layout's positions. Disable them, live + session, scanning each
// contiguous guid family past its new end.
void disableLeftovers(Staging& st, const vector<BakeAnchor>& levers) {
    const DungeonStageInputs& in = st.in;
    disablePast(st, kCellRefBase, in.bake.cellMeshes.size());
    disablePast(st, kTorchRefBase, in.bake.torches.size());
    for (const AnchorEmit& row : kAnchorEmits) {
        if (row.indexed) {
            disablePast(st, row.refBase, in.bake.countOf(row.kind));
        }
    }
    disablePast(st, kInMarkerBase, in.anchors.size());
    disablePast(st, kOutMarkerBase, in.anchors.size());
    disablePast(st, kDoorInRefBase, in.anchors.size());
    disablePast(st, kDoorOutRefBase, in.anchors.size());
    // Levers are keyed by lockId, not by index: sweep a bounded id range
    // and disable any lever (and its derived barrier) no current lock owns.
    const AnchorEmit& leverEmit = anchorEmit(AnchorKind::Lever);
    for (u64 id = 0; id < 64; ++id) {
        bool owned = false;
        for (const auto& lever : levers) {
            owned = owned || lever.lockId == id;
        }
        if (owned) {
            continue;
        }
        const core::Guid leverRef =
            derived(in.dungeonId, leverEmit.refBase + id);
        if (disableReference(st, leverRef)) {
            disableReference(st, barrierForLever(leverRef));
        }
    }
}

} // namespace

core::Guid barrierForLever(const core::Guid& leverReference) {
    return core::Guid::combine(leverReference,
                               core::Guid { 1, 0x6261727269657231ull });
}

DungeonStageResult stageDungeonRecords(const DungeonStageInputs& in) {
    DungeonStageResult result;
    if (in.bake.empty()) {
        return result;
    }
    Staging st { in, data::RecordStager { in.session, in.forms }, {},
                 in.bake.cellSize, {}, 0 };

    stageWorldspace(st, result);
    stageCaverns(st, result);
    stageTorches(st, result);
    stageNav(st, result);
    stageDoors(st, result);
    const vector<BakeAnchor> levers = in.bake.anchorsOf(AnchorKind::Lever);
    stageLocks(st, levers);
    stageKitAnchors(st);
    disableLeftovers(st, levers);

    const auto stagedCount = [&](AnchorKind kind) {
        const AnchorEmit& row = anchorEmit(kind);
        return (in.kit.*row.gate).isValid() ? in.bake.countOf(kind) : 0;
    };
    LOG_INFO("Dungeon '{}': staged {} cells, {} torches, {} anchors, "
             "{} locks, {} enemies, {} veins, {} patrols "
             "({} leftovers disabled, live + session)",
             in.dungeonName, result.cellCount, result.torchCount,
             in.anchors.size(), levers.size(),
             stagedCount(AnchorKind::EnemySpawn),
             stagedCount(AnchorKind::OreVein),
             stagedCount(AnchorKind::PatrolPoint), st.leftovers);
    return result;
}

DungeonStageResult stageDungeonRecords(
    data::EditSession& session, data::FormDatabase& forms, WorldModel& model,
    const dungeon::DungeonBakeResult& bake, const core::Guid& dungeonId,
    const str& dungeonName,
    const std::function<core::Guid(i32 cx, i32 cz)>& cellMeshAsset,
    const core::Guid& navAsset, const vector<DungeonAnchor>& anchors,
    const DungeonKit& kit, const core::Guid& doorModel,
    const core::Guid& doorMaterial) {
    return stageDungeonRecords(DungeonStageInputs {
        session, forms, model, bake, dungeonId, dungeonName, cellMeshAsset,
        navAsset, anchors, kit, doorModel, doorMaterial });
}

} // namespace world
