#pragma once

#include "engine/core/Defines.hpp"
#include "engine/fx/Particles.hpp"
#include "gameplay/cue/GameplayCues.hpp"

namespace data {
class FormDatabase;
}
namespace render {
class FlyCamera;
namespace terrain {
struct MaterialWeights;
}
}

namespace game {

class SoundResolver;

// Chantier P0 C2 — the standard cue handlers (pattern *Director): THE
// place where a sim-side `cues.emit("Cue.Hit.Slash", pos, damage)`
// becomes presentation. The handler resolves the tag through the
// CueTable (data, hierarchical fallback — mods override the specific)
// and fires the CueForm's pieces:
//   particles   -> a ParticleForm burst/emitter on the C1 sim,
//   cameraShake -> a damped impulse on the fly camera,
//   sound       -> the C3 SoundResolver (weighted variants + jitter).
// The sim never includes this: headless = zero handlers = zero work.
class FxDirector {
public:
    // Builds the cue table from the resolved database and installs the
    // standard handler onto the registry the sim-side emitters use.
    // `sounds` may be null (no audio in this scene).
    void create(const data::FormDatabase& forms, fx::ParticleSim& sim,
                SoundResolver* sounds = nullptr);

    gameplay::CueRegistry& cues() { return registry; }

    // Per frame: decays the shake and applies the camera offset. The
    // offset is TRANSIENT — the previous frame's is removed first, so
    // fly-mode cameras (whose position persists) never accumulate it.
    void update(f32 dt, render::FlyCamera& camera);

    // Direct impulse (cue handler uses it; scripts may later).
    void addShake(f32 strength) { shake = glm::min(shake + strength, 1.5f); }

    // P0 C4b (audit R6, out of LandscapeScene): the footstep material —
    // the dominant terrain splat weight names the cue suffix
    // (Cue.Footstep.<Mat>). The SCENE keeps the terrain query (it owns
    // the renderer access); this is the pure weights -> name verdict.
    static const char* footstepMaterial(
        const render::terrain::MaterialWeights& weights);

private:
    const data::FormDatabase* forms { nullptr };
    fx::ParticleSim* sim { nullptr };
    SoundResolver* sounds { nullptr };
    gameplay::CueRegistry registry;
    gameplay::CueTable table;
    f32 shake { 0.0f };      // current amplitude (m), decays per frame
    f32 shakeTime { 0.0f };  // drives the wobble phase
    Vec3 appliedOffset { 0.0f }; // last frame's camera offset, removed first
    u32 spawnCounter { 0 };  // cosmetic seed stream (never gameplay RNG, §8)
    u32 soundCounter { 0 };  // sound-path stream — advances on EVERY play
                             //   (review bug 7a: sound-only cues varied not)
};

} // namespace game
