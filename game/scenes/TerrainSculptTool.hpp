#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // render::TerrainParams, render::terrain::height
#include "engine/terrain/HeightPatches.hpp"         // render::HeightPatch / HeightPatches

namespace data {
class FormDatabase;
}

namespace game {

class LevelEditor;

// The scene systems the sculpt tool reads and affects, bundled so the tool
// stays decoupled from LandscapeScene (audit U4-5). The scene rebuilds it each
// frame from its own members — cheap: two refs, a pointer and one std::function.
// This is the first slice of the editor↔scene contract; brick B widens it (into
// EditorContext) for the rest of the editor (pick / place / gizmo / palette).
struct SculptContext {
    const render::TerrainParams& terrainParams;    // height() reads (live base + patches)
    const render::HeightPatches* publishedPatches; // current overlay, may be null
    data::FormDatabase& forms;
    LevelEditor& levelEditor;
    // Publish a freshly sculpted overlay. The tool computes the immutable
    // HeightPatches and the list of changed chunk keys (keyOf); the SCENE owns
    // the consequences. `commit` is false during a stroke — a LIVE PREVIEW:
    // swap the overlay and re-mesh only those terrain chunks (seamless), no
    // collision/scatter churn — and true on release: the permanent publish
    // (collision rebuild, cell snap, grass/veg re-scatter). Passing the changed
    // chunks is what keeps the sculpt local instead of reloading the world.
    std::function<void(std::shared_ptr<render::HeightPatches>,
                       const std::vector<u64>&, bool commit)>
        republishTerrain;
};

// Chantier 2 B9 terrain sculpt, extracted verbatim from LandscapeScene (audit
// U4-5). Brushes edit WORKING grids; a stroke's release publishes a fresh
// immutable HeightPatches (in-flight workers stay race-free). "Save terrain"
// writes .ter files + TerrainPatchForm records into the mod. Owns all sculpt
// state; the scene owns interaction (ray under cursor) and the publish effects.
class TerrainSculptTool {
public:
    bool active() const { return mode; }

    // The editor sub-panel (brush controls + save button), drawn into the
    // current ImGui window.
    void drawPanel(const SculptContext& ctx);

    // Stroke lifecycle, driven by the editor's mouse handling. `ground` is the
    // terrain hit under the cursor; dt paces the brush. endStroke publishes
    // once per stroke (a no-op if no stroke is in progress).
    void stroke(const SculptContext& ctx, const Vec3& ground, f32 dt);
    void endStroke(const SculptContext& ctx);

private:
    render::HeightPatch& gridFor(const SculptContext& ctx, i32 cx, i32 cz);
    void applyBrush(const SculptContext& ctx, const Vec3& center, f32 dt);
    void publish(const SculptContext& ctx, bool commit);
    void saveToMod(const SculptContext& ctx);

    bool mode { false };
    i32 brushKind { 0 }; // 0 raise, 1 lower, 2 flatten, 3 smooth
    f32 brushRadius { 6.0f };
    f32 brushStrength { 2.0f };
    bool strokeActive { false };
    f32 flattenTarget { 0.0f }; // grabbed at stroke start
    f32 previewTimer { 0.0f };  // throttles the live-preview re-mesh (~20 Hz)
    std::unordered_map<u64, render::HeightPatch> grids;
};

} // namespace game
