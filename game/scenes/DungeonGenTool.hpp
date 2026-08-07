#pragma once

#include <functional>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/dungeon/DungeonBake.hpp"
#include "game/LevelEditor.hpp"
#include "world/worldspace/WorldModel.hpp"

namespace game {

// The dungeon-generation sub-contract (TerrainGenTool's sibling): the tool
// bakes a mine from a seed on a worker, then Accept writes the `.cmesh` +
// `.nvg` assets (registered LIVE so the session can travel there, and on
// the export list so the mod ships them) and stages every record through
// world::stageDungeonRecords. The outside door lands at the camera, in the
// active worldspace. docs/DUNGEON-GEN.md.
struct DungeonGenContext {
    data::FormDatabase& forms;
    LevelEditor& levelEditor;
    world::WorldModel& worldModel;
    assets::AssetDatabase& assetDb;
    data::FormHandle activeWorldspace; // where the outside door stands
    core::JobSystem* jobs { nullptr };
    u32 defaultSeed { 1337 };
    Vec3 cameraPos {};
    f32 cameraYawDeg { 0.0f };
    core::Guid doorModel {};    // door leaf dressing (null = placeholder)
    core::Guid doorMaterial {};
};

class DungeonGenTool {
public:
    void drawPanel(const DungeonGenContext& ctx);

private:
    void accept(const DungeonGenContext& ctx);

    u32 seed { 0 };
    bool seedInit { false };
    i32 floors { 2 };
    i32 gridXZ { 6 };
    i32 subCycles { 1 };
    bool baking { false };
    sptr<core::ConcurrentQueue<dungeon::DungeonBakeResult>> done {
        std::make_shared<core::ConcurrentQueue<dungeon::DungeonBakeResult>>()
    };
    std::optional<dungeon::DungeonBakeResult> result;
};

} // namespace game
