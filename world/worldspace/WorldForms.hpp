#pragma once

#include "data/forms/Form.hpp"

// The world data model. These are ordinary Forms — reflected,
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
    // The hard death floor — an actor below it dies outright.
    f32 killZ { -200.0f };
    // H3 (docs/RENDERING.md): below this height the space counts as
    // BURIED — sun-linked lights and the daylight-coupled ambience fade
    // out per POSITION (a cellar under a windowed house settles itself).
    // Default: far below everything = the rule is off.
    f32 buriedBelowY { -1.0e9f };

    REFLECT_BEGIN(WorldspaceForm, data::Form)
        REFLECT_FIELD(cellSize)
        REFLECT_FIELD(interior)
        REFLECT_FIELD(killZ)
        REFLECT_FIELD(buriedBelowY)
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

// Implicit cells: the DETERMINISTIC
// identity of grid square (gx, gy) in a worldspace — derived, never
// minted (§2.5). Every session, save and mod that touches the same
// square talks about the same cell, whatever the load order. Editors
// materialize a CellForm under this guid on first placement; a plugin
// that ships the same square patches it (§5) instead of duplicating.
core::Guid cellGuidFor(const core::Guid& worldspace, i32 gx, i32 gy);

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
    // Prefab membership. A reference with
    // `prefab` set is a TEMPLATE child of that PrefabForm: its transform is
    // relative to the prefab pivot, it belongs to no cell, and the
    // CellLoader never spawns it directly. Placing a reference whose
    // baseForm IS a PrefabForm instantiates every template child with a
    // deterministic derived guid (see Spawner).
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

// A reusable group of references. The form itself is just the group's
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

// A traversable door. Visual through the universal
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

// An authored terrain override: one record per sculpted
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

// A baked terrain REGION: an absolute height grid (.trg asset) replacing
// the procedural base inside its rectangle — the generated-terrain layer
// under the sculpt deltas above. Geometry (origin/size/texel) lives in the
// asset header (one source of truth, like TER1's sample count); the Form
// is identity + the runtime detail-noise character, so a mod can retune
// detail per field or replace the whole grid by asset guid (§5 VFS).
struct TerrainRegionForm : data::Form {
    str displayName;
    core::Guid asset;               // .trg region file
    f32 detailAmplitude { 0.0f };   // meters of runtime detail noise
    f32 detailWavelength { 60.0f }; // meters
    i32 detailOctaves { 3 };

    REFLECT_BEGIN(TerrainRegionForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(asset)
        REFLECT_FIELD(detailAmplitude)
        REFLECT_FIELD(detailWavelength)
        REFLECT_FIELD(detailOctaves)
    REFLECT_END()
};

// A water shading preset (lava, mud, enchanted pools...): referenced by
// GUID from WaterBodyForm/RiverForm records; a null reference = default
// water, and the default-constructed values ARE the current hardcoded
// water look (bit-identical). Pure data, moddable (§5).
struct WaterMaterialForm : data::Form {
    str displayName;
    Vec3 tint { 0.10f, 0.30f, 0.34f }; // shallow-water tint...
    f32 tintStrength { 0.0f };         // ...mixed in by this much
    Vec3 deepColor { 0.008f, 0.045f, 0.055f };
    Vec3 absorption { 0.42f, 0.16f, 0.12f }; // per channel, 1/m
    Vec3 foamColor { 0.75f, 0.82f, 0.85f };
    f32 foamGain { 1.0f };
    Vec3 emissiveColor { 0.0f, 0.0f, 0.0f }; // lava glow
    f32 emissiveStrength { 0.0f };
    f32 flowSpeedScale { 1.0f }; // advection multiplier
    f32 viscosity { 0.0f };      // 0 water .. 1 syrup (slows the flow)
    f32 waveScale { 1.0f };      // ripple frequency multiplier

    REFLECT_BEGIN(WaterMaterialForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(tint)
        REFLECT_FIELD(tintStrength)
        REFLECT_FIELD(deepColor)
        REFLECT_FIELD(absorption)
        REFLECT_FIELD(foamColor)
        REFLECT_FIELD(foamGain)
        REFLECT_FIELD(emissiveColor)
        REFLECT_FIELD(emissiveStrength)
        REFLECT_FIELD(flowSpeedScale)
        REFLECT_FIELD(viscosity)
        REFLECT_FIELD(waveScale)
    REFLECT_END()
};

// An altitude lake: a flat water surface at its own level, clipped by the
// terrain basin (the sea-shoreline mechanism). The generator emits these
// as ordinary records; a modder raises a lake in pure TOML (§5).
struct WaterBodyForm : data::Form {
    str displayName;
    f32 surfaceLevel { 30.0f };
    f32 minX { 0.0f };
    f32 minZ { 0.0f };
    f32 maxX { 0.0f };
    f32 maxZ { 0.0f };
    Vec3 tint { 0.10f, 0.30f, 0.34f };
    f32 chop { 0.5f };
    core::Guid material; // WaterMaterialForm; null = default water

    REFLECT_BEGIN(WaterBodyForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(surfaceLevel)
        REFLECT_FIELD(minX)
        REFLECT_FIELD(minZ)
        REFLECT_FIELD(maxX)
        REFLECT_FIELD(maxZ)
        REFLECT_FIELD(tint)
        REFLECT_FIELD(chop)
        REFLECT_FIELD(material)
    REFLECT_END()
};

// A river: identity + shading; its course is RiverPointForm child records
// (the §C.1 child-record convention — reflection stays flat).
struct RiverForm : data::Form {
    str displayName;
    Vec3 tint { 0.10f, 0.30f, 0.34f };
    f32 flowSpeed { 1.0f };
    core::Guid material; // WaterMaterialForm; null = default water

    REFLECT_BEGIN(RiverForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(tint)
        REFLECT_FIELD(flowSpeed)
        REFLECT_FIELD(material)
    REFLECT_END()
};

// One node of a river course, downstream order by `index`; position.y =
// water surface (monotone downhill — the generator's contract).
struct RiverPointForm : data::Form {
    core::Guid parent; // RiverForm
    i32 index { 0 };
    Vec3 position { 0.0f };
    f32 halfWidth { 2.0f };

    REFLECT_BEGIN(RiverPointForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(index)
        REFLECT_FIELD(position)
        REFLECT_FIELD(halfWidth)
    REFLECT_END()
};

// One biome: the u8 id painted/derived over the terrain resolves to this
// record's character (render::BiomeParams mirror) — terrain materials,
// scatter presence, climate, and later gameplay tags.
struct BiomeForm : data::Form {
    str displayName;
    i32 paletteIndex { 0 };  // the u8 id; 0 = neutral
    Vec3 editorColor { 0.5f }; // painting-UI swatch
    f32 snowLineOffset { 0.0f };
    f32 rockiness { 0.0f };
    f32 sandiness { 0.0f };
    f32 grassPresence { 1.0f };
    f32 detailAmplitudeScale { 1.0f };
    f32 temperature { 0.0f };
    f32 wetness { 0.0f };
    i32 vegetationSet { 0 };
    str gameplayTag; // e.g. "Biome.Tundra" (GAS tags, later)

    REFLECT_BEGIN(BiomeForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(paletteIndex)
        REFLECT_FIELD(editorColor)
        REFLECT_FIELD(snowLineOffset)
        REFLECT_FIELD(rockiness)
        REFLECT_FIELD(sandiness)
        REFLECT_FIELD(grassPresence)
        REFLECT_FIELD(detailAmplitudeScale)
        REFLECT_FIELD(temperature)
        REFLECT_FIELD(wetness)
        REFLECT_FIELD(vegetationSet)
        REFLECT_FIELD(gameplayTag)
    REFLECT_END()
};

// Per-biome vegetation entry, child of BiomeForm (§C.1).
struct BiomeVegetationForm : data::Form {
    core::Guid parent; // BiomeForm
    core::Guid species;
    f32 density { 1.0f };

    REFLECT_BEGIN(BiomeVegetationForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(species)
        REFLECT_FIELD(density)
    REFLECT_END()
};

// The painted biome index map (scenario mode; sandbox tiles derive their
// ids from the seed instead). Asset: "TBM1" u8 grid.
struct BiomeMapForm : data::Form {
    core::Guid asset;

    REFLECT_BEGIN(BiomeMapForm, data::Form)
        REFLECT_FIELD(asset)
    REFLECT_END()
};

// Registers the world form types; call once at startup, like
// data::registerCoreFormTypes, before loading plugins that place references.
void registerWorldFormTypes(data::FormTypeRegistry& registry);

} // namespace world
