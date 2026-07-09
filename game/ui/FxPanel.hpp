#pragma once

#include "data/plugins/EditSession.hpp"
#include "engine/fx/Particles.hpp"

namespace game {

// The FX editor with LIVE preview (chantier 8.10): the selected
// ParticleForm drives a fx::ParticleSim member every frame — the same
// headless sim the game runs, drawn as ImDrawList discs (orthographic
// X/Y, ground line). Deliberately approximate where the tool doesn't
// matter: no additive blending in ImDrawList (brightened instead), no
// textures — it tunes SHAPE and TIMING; the final look is judged in
// game. rate/duration/burst are emulated by an accumulator loop (the
// sim itself only knows spawnBurst — its HOW TO FILL). Edits are live:
// the form is re-read every frame, Restart replays from zero.
class FxPanel {
public:
    explicit FxPanel(data::EditSession& session) : session { session } {}

    void drawEditor(const core::Guid& particle);

private:
    void restart(const core::Guid& particle);

    data::EditSession& session;
    fx::ParticleSim sim;
    core::Guid shown;       // resets the loop when the selection changes
    f32 age { 0.0f };       // emitter seconds since restart
    f32 accumulator { 0.0f };
    f32 timeScale { 1.0f };
    f32 zoom { 60.0f };     // pixels per meter
    u32 seed { 1 };         // incremented per spawn: varied but replayable
    bool paused { false };
};

} // namespace game
