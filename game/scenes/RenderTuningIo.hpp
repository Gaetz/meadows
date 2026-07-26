#pragma once

#include "engine/core/Defines.hpp"

namespace data {
struct LandscapeTuningForm;
struct LobeTreeTuningForm;
struct ColonizedTreeTuningForm;
struct RcTuningForm;
}
namespace render {
class WorldRenderer;
class HeightPatches;
}

namespace game {

// The Forms<->flat-params seam of the world renderer (CLAUDE.md §4: the
// engine never sees a Form). apply* seeds the renderer's live knobs from
// the resolved tuning records at startup (§5: the TOML sets where
// everything starts); capture* is the reverse mapping for the panels'
// "Save render tuning" button — fills the records from the CURRENT live
// values (fields the panels don't own keep `out`'s values; the scene
// overlays its atmosphere-owned fields, then writes the overlay plugin).
// Stateless, friend of the renderer — the seam edits its knobs in place.
class RenderTuningIo {
public:
    static void applyTuning(render::WorldRenderer& r,
                            const data::LandscapeTuningForm& tuning,
                            const sptr<const render::HeightPatches>& patches);
    // Tree builder: the two *TreeTuningForm records mapped onto the
    // generators' flat engine params (the TerrainParams pattern) —
    // startup values; the Trees panel edits them live.
    static void applyTreeTuning(render::WorldRenderer& r,
                                const data::LobeTreeTuningForm& lobes,
                                const data::ColonizedTreeTuningForm& colonized);
    // GI tuning record -> the live RcTuning struct (same contract).
    static void applyRcTuning(render::WorldRenderer& r,
                              const data::RcTuningForm& rc);

    static void captureTuning(const render::WorldRenderer& r,
                              data::LandscapeTuningForm& out);
    static void captureRcTuning(const render::WorldRenderer& r,
                                data::RcTuningForm& out);
    static void captureTreeTuning(const render::WorldRenderer& r,
                                  data::LobeTreeTuningForm& lobes,
                                  data::ColonizedTreeTuningForm& colonized);
};

} // namespace game
