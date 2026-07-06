#pragma once

#include "data/forms/Form.hpp"

// The world data model (Phase 2, brick c). These are ordinary Forms — reflected,
// moddable, resolved by the §5 plugin resolver like any other record. A placed
// instance (ReferenceForm) is therefore a Form too, so placement, move, disable
// and (later) saves are all field-level patches through the existing resolver
// (§2.4). See docs/PHASE-2.md for the rationale.

namespace data {
class FormTypeRegistry;
}

namespace world {

// A top-level space: an exterior region or a single interior. Cells live inside
// it on a regular grid of `cellSize` world units.
struct WorldspaceForm : data::Form {
    f32 cellSize { 16.0f };
    bool interior { false };

    REFLECT_BEGIN(WorldspaceForm, data::Form)
        REFLECT_FIELD(cellSize)
        REFLECT_FIELD(interior)
    REFLECT_END()
};

// One cell of a worldspace. Exterior cells are keyed by integer grid coords;
// `worldspace` ties the cell to its parent. The cell does NOT list its
// references: membership is a field on each ReferenceForm (reflection v1 has no
// container type, and lists do not patch cleanly under last-writer-wins).
struct CellForm : data::Form {
    core::Guid worldspace;
    i32 gridX { 0 };
    i32 gridY { 0 };
    bool interior { false };

    REFLECT_BEGIN(CellForm, data::Form)
        REFLECT_FIELD(worldspace)
        REFLECT_FIELD(gridX)
        REFLECT_FIELD(gridY)
        REFLECT_FIELD(interior)
    REFLECT_END()
};

// A placed instance of a base Form (the `baseForm` it instantiates), carrying
// instance-level overrides. Transform is 3D-ready (§2.6): the 2D phase projects
// it. `cell` is the owning cell; patching it moves the reference, patching
// `enabled` disables it — all clean field-level edits (§5).
struct ReferenceForm : data::Form {
    core::Guid baseForm;
    core::Guid cell;
    Vec3 position { 0.0f, 0.0f, 0.0f };
    Quat rotation { 1.0f, 0.0f, 0.0f, 0.0f }; // file order is [x, y, z, w]
    Vec3 scale { 1.0f, 1.0f, 1.0f };
    bool enabled { true };
    i32 count { 1 };
    // Prefab membership (H1, appended — ordinals stable). A reference with
    // `prefab` set is a TEMPLATE child of that PrefabForm: its transform is
    // relative to the prefab pivot, it belongs to no cell, and the
    // CellLoader never spawns it directly. Placing a reference whose
    // baseForm IS a PrefabForm instantiates every template child with a
    // deterministic derived guid (see Spawner, H8).
    core::Guid prefab;

    REFLECT_BEGIN(ReferenceForm, data::Form)
        REFLECT_FIELD(baseForm)
        REFLECT_FIELD(cell)
        REFLECT_FIELD(position)
        REFLECT_FIELD(rotation)
        REFLECT_FIELD(scale)
        REFLECT_FIELD(enabled)
        REFLECT_FIELD(count)
        REFLECT_FIELD(prefab)
    REFLECT_END()
};

// A reusable group of references (H1). The form itself is just the group's
// identity: its content is every ReferenceForm whose `prefab` points here
// (child-record convention, data::childrenOf). Authoring lives in the
// level editor ("create prefab from selection"); nesting = a template
// child whose baseForm is another PrefabForm.
struct PrefabForm : data::Form {
    str displayName;

    REFLECT_BEGIN(PrefabForm, data::Form)
        REFLECT_FIELD(displayName)
    REFLECT_END()
};

// An invisible authoring/AI anchor (spawn points, patrol stops, schedule
// locations, heading markers). Placed like anything else; `kind` is free
// vocabulary the systems agree on ("spawn", "patrol", "idle"...).
struct MarkerForm : data::Form {
    str kind { "marker" };

    REFLECT_BEGIN(MarkerForm, data::Form)
        REFLECT_FIELD(kind)
    REFLECT_END()
};

// A gameplay volume: fires `event` on the EventBus (and/or runs `script`)
// when something enters/leaves. Extent is a local-space box half-size,
// scaled/rotated by the placing reference like any transform.
struct TriggerForm : data::Form {
    Vec3 halfExtents { 1.0f, 1.0f, 1.0f };
    str event;        // EventBus event name ("" = none)
    str script;       // Lua snippet ("" = none)
    bool once { false };

    REFLECT_BEGIN(TriggerForm, data::Form)
        REFLECT_FIELD(halfExtents)
        REFLECT_FIELD(event)
        REFLECT_FIELD(script)
        REFLECT_FIELD(once)
    REFLECT_END()
};

// A traversable door (chantier 2 B7). Visual through the universal
// reflected model/material wiring (like StaticForm); `targetMarker` is the
// GUID of a placed marker REFERENCE (a ReferenceForm record) — the arrival
// spot. Its record carries position AND cell, whose CellForm names the
// destination worldspace: the transition resolves entirely from records,
// so it works even while the destination cells are unloaded.
struct DoorForm : data::Form {
    str displayName;
    core::Guid model;    // glTF door leaf
    core::Guid material; // MaterialForm (0 = vertex colors)
    bool collides { true };
    bool snapToGround { false }; // doors sit in authored walls
    core::Guid targetMarker;     // ReferenceForm of a MarkerForm

    REFLECT_BEGIN(DoorForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(collides)
        REFLECT_FIELD(snapToGround)
        REFLECT_FIELD(targetMarker)
    REFLECT_END()
};

// An authored terrain override (chantier 2 B8): one record per sculpted
// 64 m terrain chunk, pointing at a `.ter` delta-grid ASSET (reflection is
// flat — grids never live in Forms). Final height = procedural noise +
// bilinear(delta). Mods override the grid by shipping the asset guid (§5
// VFS) or patch the record. IO + overlay building: world/terrain/.
struct TerrainPatchForm : data::Form {
    i32 chunkX { 0 };
    i32 chunkZ { 0 };
    core::Guid asset; // .ter grid file

    REFLECT_BEGIN(TerrainPatchForm, data::Form)
        REFLECT_FIELD(chunkX)
        REFLECT_FIELD(chunkZ)
        REFLECT_FIELD(asset)
    REFLECT_END()
};

// Registers the world form types; call once at startup, like
// data::registerCoreFormTypes, before loading plugins that place references.
void registerWorldFormTypes(data::FormTypeRegistry& registry);

} // namespace world
