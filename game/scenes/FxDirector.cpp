#include "game/scenes/FxDirector.hpp"

#include <cmath>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Hash.hpp"
#include "engine/render/FlyCamera.hpp"
#include "game/SoundResolver.hpp" // C3: cue sounds

namespace game {

void FxDirector::create(const data::FormDatabase& formsIn,
                        fx::ParticleSim& simIn, SoundResolver* soundsIn) {
    forms = &formsIn;
    sim = &simIn;
    sounds = soundsIn;
    registry = {}; // a scene re-enter must not stack handlers
    table.build(formsIn);
    registry.addHandler([this](const gameplay::CueEvent& event) {
        const data::CueForm* cue = table.find(event.tag);
        if (!cue) {
            return; // no authored look for this tag (or its parents)
        }
        if (cue->particles.isValid() && sim) {
            if (const auto* particles =
                    forms->find<data::ParticleForm>(cue->particles)) {
                // Cosmetic seed: spot hash + a running counter (§8:
                // presentation never touches the gameplay RNG).
                const u32 seed =
                    core::hashU32(static_cast<u32>(event.position.x * 73.0f) ^
                                  (static_cast<u32>(event.position.z * 179.0f)
                                   << 8) ^
                                  ++spawnCounter);
                sim->spawn(gameplay::toEmitterParams(*particles),
                           event.position, seed);
            }
        }
        if (cue->cameraShake > 0.0f) {
            // Damage-ish magnitudes scale the punch a little (10 damage
            // = authored strength), clamped so a crit doesn't nauseate.
            const f32 scale =
                event.magnitude > 0.0f
                    ? glm::clamp(event.magnitude / 10.0f, 0.5f, 2.0f)
                    : 1.0f;
            addShake(cue->cameraShake * scale);
        }
        if (cue->sound.isValid() && sounds) {
            // C3: the SoundForm resolver — weighted variant + jitter,
            // seeded from the same cosmetic stream as the particles.
            sounds->play(cue->sound, event.position,
                         core::hashU32(spawnCounter ^ 0xac00571cu));
        }
    });
}

void FxDirector::update(f32 dt, render::FlyCamera& camera) {
    // Remove last frame's offset FIRST: fly cameras own their position
    // across frames, a persistent add would random-walk them.
    camera.camera.position -= appliedOffset;
    appliedOffset = Vec3 { 0.0f };
    if (shake <= 0.001f) {
        shake = 0.0f;
        return;
    }
    shakeTime += dt;
    // Damped wobble: two incommensurate frequencies so it reads as a
    // jolt, not a metronome. [cpp-tuning]
    const f32 amplitude = shake * 0.05f;
    appliedOffset = Vec3 { std::sin(shakeTime * 71.0f),
                           std::cos(shakeTime * 47.0f) * 0.6f, 0.0f } *
                    amplitude;
    camera.camera.position += appliedOffset;
    shake *= std::exp(-9.0f * dt); // ~110 ms half-life
}

} // namespace game
