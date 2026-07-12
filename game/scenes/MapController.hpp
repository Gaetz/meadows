#pragma once

#include <atomic>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Defines.hpp"
#include "game/MapRaster.hpp"

namespace core {
class JobSystem;
}
namespace ui {
class UiSystem;
}
namespace world {
struct WorldspaceForm;
}

namespace game {

class ScreenStack;

// Chantier 9 C9.6 — the in-game map screen (Map action = M / d-pad left):
// a stylized top-down raster of the active EXTERIOR worldspace, generated
// on the CPU (game/MapRaster) from the same terrain functions the world
// uses, shown through the runtime:// texture facade, with the player
// marker and the door POIs placed through the SAME world->UV mapping
// (mapUv) — one mapping function, no duplicated math. Indoors, the map
// shows the overworld and hides the marker.
//
// The raster generates on the JobSystem (measured 261 ms for 512² in
// Debug — way past a frame): open() computes the extent synchronously
// (markers need only that), the worker fills the pixels into its own
// self-owned packet, updateOpen() polls and pushes the texture from the
// MAIN thread (Phase-5 rule: workers touch no GPU — and the runtime://
// re-push releases a GL texture). Until it lands, the screen shows the
// placeholder the scene seeded.
struct MapContext {
    const data::FormDatabase& forms;
    const render::TerrainParams& terrain; // renderer ground truth
    core::JobSystem& jobs;                // raster generation (async)
    ::ui::UiSystem& ui;                   // the "map" data model
    ScreenStack& screenStack;
    data::FormHandle activeWorldspace; // may be an interior
    data::FormHandle overworld;        // the exterior fallback
    bool interior;                     // active worldspace is an interior
    Vec3 playerPos;                    // Play focus (capsule feet)
};

class MapController {
public:
    // The Map hotkey: extent + door POI rows + marker synchronously,
    // the raster kicked to a worker (only when the exterior worldspace
    // changed since the last one), then show the screen.
    void open(const MapContext& ctx);

    // Per-frame while the map is the top modal: publish a finished
    // raster (main thread), keep the marker on the player (setNumber
    // dedupes unchanged values).
    void updateOpen(const MapContext& ctx);

    // The "map" data-model events (back).
    void handleEvent(const MapContext& ctx, const str& event);

    // onExit: the UiSystem (and its runtime pixels) die with the scene —
    // forget the raster so a re-enter regenerates it. A job still in
    // flight keeps its self-owned packet alive and is dropped unread.
    void reset() {
        pending_.reset();
        hasRaster_ = false;
        extentValid_ = false;
    }

private:
    const world::WorldspaceForm* exteriorWorldspace(
        const MapContext& ctx) const;
    void pushMarker(const MapContext& ctx);

    // The worker's self-owned packet (Phase-5 idiom): its OWN copy of the
    // TerrainParams (the patches sptr keeps authored terrain alive even
    // across scene teardown) and the output pixels. `done` is the only
    // cross-thread signal.
    struct AsyncRaster {
        render::TerrainParams params;
        MapRasterDesc desc;
        core::Guid worldspace;
        vector<u8> pixels;
        std::atomic<bool> done { false };
    };
    sptr<AsyncRaster> pending_;

    MapRasterDesc desc_ {};       // extent of the current map (mapUv)
    core::Guid rasterWorldspace_; // which exterior the raster shows
    bool hasRaster_ { false };    // pixels pushed to runtime://map
    bool extentValid_ { false };  // desc_ usable for markers
};

} // namespace game
