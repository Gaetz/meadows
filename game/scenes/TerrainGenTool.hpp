#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/LandscapeForms.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "game/LevelEditor.hpp"
#include "game/TerrainBakeStreamer.hpp"

namespace game {

// The terrain-generation sub-contract (SculptContext pattern): the
// editor tool bakes a whole bounded MAP on workers (the per-region
// windowed flow died with the windowed streamer, chantier CARTES
// M1.5b); Accept stages assets + records through the EditSession and
// the ordinary Export writes the mod.
struct GenContext {
    data::FormDatabase& forms;
    LevelEditor& levelEditor;
    core::JobSystem* jobs { nullptr };
    Vec3 cameraPos {};
    // Map-scale bake (chantier CARTES M5.3): the SAME resolved params
    // and cache the runtime streamer uses, so the editor's map bake
    // and the game share slices. Empty mapCacheRoot hides the section
    // (non-map scenes).
    render::terraingen::TileBakeParams mapBakeParams;
    std::filesystem::path mapCacheRoot; // terrain-cache/<seed>
    // The generation PUPITRE (docs/PAYSAGE.md §7.7, Z0): the scene's
    // live TerrainGenTuningForm and what the panel may ask of it —
    // re-bake the active map with it (the cache key follows the
    // values), write the overlay (preset "") or a named preset, load
    // a preset. Null genTuning hides the section.
    data::TerrainGenTuningForm* genTuning { nullptr };
    std::function<void()> applyGenTuning;
    std::function<void(const str&)> saveGenTuning;
    std::function<bool(const str&)> loadGenTuning;
    std::filesystem::path genPresetsDir;
};

// Editor panel: bake a bounded map on workers (progress bar), then
// Accept -> slice assets + the map's §5 records (stageMapRecords).
// Control-map painting (tiers/biomes by brush) is a planned next step.
class TerrainGenTool {
public:
    void drawPanel(const GenContext& ctx);

private:
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
    // Pupitre UI state.
    char presetName[64] { "" };
    char fieldFilter[64] { "" };
    str presetPicked;
    void drawPupitre(const GenContext& ctx);
};

} // namespace game
