#include "game/scenes/FxDirector.hpp"

#include <cmath>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Hash.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // MaterialWeights
#include "game/SoundResolver.hpp" // cue sounds

namespace game {

const char* FxDirector::footstepMaterial(
    const render::terrain::MaterialWeights& weights) {
    // Max over the splat weights: the step sounds like what the ground
    // LOOKS covered with (grass wins ties, the dominant ground). Bare
    // cliff sounds like rock.
    const char* material = "Grass";
    f32 best = weights.grass;
    if (weights.rock + weights.cliff > best) {
        best = weights.rock + weights.cliff;
        material = "Rock";
    }
    if (weights.snow > best) { best = weights.snow; material = "Snow"; }
    if (weights.sand > best) { best = weights.sand; material = "Sand"; }
    return material;
}

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
                    core::hashU32(static_cast<u32>(static_cast<i32>(event.position.x * 73.0f)) ^
                                  (static_cast<u32>(static_cast<i32>(event.position.z * 179.0f))
                                   << 8) ^
                                  ++spawnCounter);
                sim->spawn(gameplay::toEmitterParams(*particles),
                           event.position, seed);
            }
        }
        if (cue->cameraShake > 0.0f) {
            // Damage-ish magnitudes scale the punch a little (shakeScale
            // damage = authored strength), clamped so a crit doesn't
            // nauseate. All four knobs are CueForm fields.
            const f32 scale =
                event.magnitude > 0.0f
                    ? glm::clamp(event.magnitude /
                                     glm::max(cue->shakeScale, 0.001f),
                                 cue->shakeScaleMin, cue->shakeScaleMax)
                    : 1.0f;
            addShake(cue->cameraShake * scale, cue->shakeAmplitude,
                     cue->shakeDecay);
        }
        if (cue->sound.isValid() && sounds) {
            // The SoundForm resolver — weighted variant + jitter.
            // Its OWN always-incremented counter (review bug 7a: the
            // spawn counter only moved in the particle branch, freezing
            // sound-only cues on one variant) + the spot hash, mirroring
            // the particle seed; the emitter's scales soften sneaked
            // steps.
            const u32 seed =
                core::hashU32(static_cast<u32>(static_cast<i32>(event.position.x * 73.0f)) ^
                              (static_cast<u32>(static_cast<i32>(event.position.z * 179.0f))
                               << 8) ^
                              ++soundCounter ^ 0xac00571cu);
            sounds->play(cue->sound, event.position, seed,
                         event.volumeScale, event.pitchScale);
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
    // jolt, not a metronome. [cpp-tuning] (the frequencies; amplitude
    // and decay come from the triggering CueForm).
    const f32 amplitude = shake * shakeAmplitude;
    appliedOffset = Vec3 { std::sin(shakeTime * 71.0f),
                           std::cos(shakeTime * 47.0f) * 0.6f, 0.0f } *
                    amplitude;
    camera.camera.position += appliedOffset;
    shake *= std::exp(-shakeDecay * dt); // ~110 ms half-life at 9/s
}

} // namespace game
