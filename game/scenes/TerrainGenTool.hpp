#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "game/LevelEditor.hpp"
#include "game/TerrainBakeStreamer.hpp"

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
    // Map-scale bake (chantier CARTES M5.3): the SAME resolved params
    // and cache the runtime streamer uses, so the editor's map bake
    // and the game share slices. Empty mapCacheRoot hides the section
    // (non-map scenes).
    render::terraingen::TileBakeParams mapBakeParams;
    std::filesystem::path mapCacheRoot; // terrain-cache/<seed>
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
    void acceptMap(const GenContext& ctx);

    // The map bake's self-owned packet (Phase-5 idiom): the worker
    // writes the atomics, the panel polls them; the tool dropping the
    // sptr never races the worker.
    struct MapBake {
        std::atomic<u32> landed { 0 };
        u32 total { 0 };
        std::atomic<bool> done { false };
        std::atomic<bool> ok { false };
        i32 mapX { 0 };
        i32 mapZ { 0 };
    };
    sptr<MapBake> mapBake;
    i32 mapX { 0 };
    i32 mapZ { 0 };

    u32 seed { 0 };
    bool seedInit { false };
    f32 regionSize { 2048.0f };
    bool baking { false };
    i32 tileX { 0 };
    i32 tileZ { 0 };
    // The bake runs through a dedicated TerrainBakeStreamer (stage-1
    // registry + disk cache, directory keyed by seed/size): a re-bake
    // of a visited region is a cache read, and neighbouring bakes share
    // their overlapping stage-1s instead of recomputing 9 each.
    uptr<TerrainBakeStreamer> streamer;
    u32 streamerSeed { 0 };
    f32 streamerSize { 0.0f };
    std::optional<render::terraingen::TileBakeResult> result;
};

} // namespace game
