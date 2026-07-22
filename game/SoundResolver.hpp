#pragma once

#include <optional>

#include "engine/audio/Audio.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

namespace assets {
class AssetDatabase;
}
namespace data {
class FormDatabase;
}

namespace game {

// The SoundForm resolver, the data half of the audio seam
// (docs/HORIZONTAL-PASS.md): picks a WEIGHTED child variant
// (SoundVariantForm — a mod adds
// one without touching the parent), applies volume/pitch jitter,
// resolves the asset through the plugin VFS and hands flat SoundParams
// to the audio facade. Randomness is a seed-hash stream — cosmetic,
// never the gameplay RNG (§8) — so the same seed always resolves the
// same way (doctested on the null backend).
class SoundResolver {
public:
    void create(const data::FormDatabase& forms,
                const assets::AssetDatabase& assets,
                audio::AudioSystem* audio);

    // Resolve without playing (tests / inspection). nullopt = unknown
    // form, or no variant whose asset the VFS can resolve. The scales
    // multiply the authored volume/pitch AFTER the jitter (sneaked
    // steps: softer and lower).
    std::optional<audio::SoundParams> resolve(const core::Guid& sound,
                                              const Vec3& position,
                                              u32 seed,
                                              f32 volumeScale = 1.0f,
                                              f32 pitchScale = 1.0f) const;

    // Fire-and-forget at `position` (used when the form says 3D).
    bool play(const core::Guid& sound, const Vec3& position, u32 seed,
              f32 volumeScale = 1.0f, f32 pitchScale = 1.0f);

private:
    const data::FormDatabase* forms { nullptr };
    const assets::AssetDatabase* assets { nullptr };
    audio::AudioSystem* audio { nullptr };
};

} // namespace game
