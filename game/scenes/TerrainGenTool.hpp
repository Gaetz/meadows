#pragma once

#include <functional>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "game/LevelEditor.hpp"

namespace game {

// The terrain-generation sub-contract (SculptContext pattern): the editor
// tool bakes a region from a seed, the SCENE owns the publish
// consequences; Accept stages assets + records through the EditSession
// and the ordinary Export writes the mod.
struct GenContext {
    data::FormDatabase& forms;
    LevelEditor& levelEditor;
    core::JobSystem* jobs { nullptr };
    u32 defaultSeed { 1337 };
    Vec3 cameraPos {};
    // Lands a finished bake into the live world (region + water preview).
    std::function<void(render::terraingen::TileBakeResult&&, i32 tx,
                       i32 tz)>
        publishPreview;
};

// Editor panel: bake a generated region (S1..S6 pipeline) around the
// camera, preview it live, then Accept -> .trg asset + TerrainRegionForm
// + WaterBodyForm/RiverForm records. Control-map painting (tiers/biomes
// by brush) is the planned next step; v1 derives controls from the seed.
class TerrainGenTool {
public:
    void drawPanel(const GenContext& ctx);

private:
    void accept(const GenContext& ctx);

    u32 seed { 0 };
    bool seedInit { false };
    f32 regionSize { 2048.0f };
    bool baking { false };
    i32 tileX { 0 };
    i32 tileZ { 0 };
    sptr<core::ConcurrentQueue<render::terraingen::TileBakeResult>> done {
        std::make_shared<
            core::ConcurrentQueue<render::terraingen::TileBakeResult>>()
    };
    std::optional<render::terraingen::TileBakeResult> result;
};

} // namespace game
