#pragma once

#include "data/forms/Form.hpp"              // data::FormHandle
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"             // core::Guid
#include "engine/ecs/World.hpp"             // ecs::Entity, ecs::World
#include "game/scenes/DungeonGenTool.hpp"    // DungeonGenTool + DungeonGenContext
#include "game/scenes/TerrainGenTool.hpp"    // TerrainGenTool + GenContext
#include "game/scenes/TerrainSculptTool.hpp" // TerrainSculptTool + SculptContext

namespace render {
class FlyCamera;
struct TerrainParams;
}
namespace data {
class FormDatabase;
}
namespace world {
class WorldModel;
class Spawner;
class CellLoader;
class CellStreamer;
class FormCategoryRegistry;
}

namespace render {
class MeshCache;
}

namespace game {

class LevelEditor;

// The scene systems the in-world level editor touches, bundled so the editor
// (pick / place / gizmo / palette / sculpt) is decoupled from LandscapeScene
//. The scene rebuilds it each frame from its own members — cheap:
// references plus the sculpt sub-contract by value. This is the editor↔scene
// contract; the target is to let the editor become a stacked SceneStack layer.
struct EditorContext {
    render::FlyCamera& camera;               // ray + gizmo view/proj, capturing()
    ecs::World& world;                       // pick query, live spawn / destruct
    render::MeshCache& meshCache;            // pick AABBs (CpuMesh bounds)
    bool interiorMode;                       // ground pick: floor plane vs terrain
    const render::TerrainParams& terrainParams; // ground raymarch (height)
    data::FormDatabase& forms;               // palette, cell + base lookups
    LevelEditor& levelEditor;                // every edit op (EditSession)
    data::FormHandle activeWorldspace;       // cell resolution for placement
    world::WorldModel& worldModel;           // cellAt / materializeCell
    world::FormCategoryRegistry& categories; // SpawnContext
    world::CellLoader& cellLoader;           // cellEntity for live placement
    world::CellStreamer& streamer;           // adopt() — implicit-cell load
    world::Spawner& spawner;                 // live spawn of placed drafts
    SculptContext sculpt;                    // the terrain-sculpt sub-contract
    GenContext gen;                          // the terrain-generation sub-contract
    DungeonGenContext dungeonGen;            // the dungeon-generation sub-contract
};

// The in-world level editor, extracted from LandscapeScene. Owns
// the editor state (selection, armed palette, gizmo op, the terrain sculpt
// tool); every edit lands in the LevelEditor's EditSession (§5). Interaction
// and rendering (ImGui/ImGuizmo) live here; the scene owns the systems and
// passes them each frame through EditorContext.
class SceneEditor {
public:
    // The editor frame: gizmo on the selection, click-to-pick / click-to-place,
    // sculpt strokes, and the editor window (palette, ops, session, export).
    void draw(const EditorContext& ctx);

    // Clear the selection and disarm the palette (on leaving Edit mode, and on
    // scene (re-)enter).
    void deselect();

private:
    Vec3 mouseRayDirection(const EditorContext& ctx, const Vec2& mousePx) const;
    bool pickEntity(const EditorContext& ctx, const Vec2& mousePx,
                    ecs::Entity& out) const;
    bool groundUnderMouse(const EditorContext& ctx, const Vec2& mousePx,
                          Vec3& out) const;

    ecs::Entity editSelection {};
    core::Guid placementBase {}; // armed palette entry (0 = none)
    i32 gizmoOperation { 0 };    // 0 translate, 1 rotate, 2 scale
    bool gizmoWasUsing { false };
    // Grid snap: fed to ImGuizmo while
    // manipulating — translate steps snapStep meters, rotate a 15° lattice.
    bool snapEnabled { false };
    f32 snapStep { 1.0f };
    TerrainSculptTool sculptTool;
    TerrainGenTool genTool;
    DungeonGenTool dungeonGenTool;
};

} // namespace game
