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

// Chantier P0 C3 — the SoundForm resolver, the data half of the H6 audio
// seam: picks a WEIGHTED child variant (SoundVariantForm — a mod adds
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
    // form, or no variant whose asset the VFS can resolve.
    std::optional<audio::SoundParams> resolve(const core::Guid& sound,
                                              const Vec3& position,
                                              u32 seed) const;

    // Fire-and-forget at `position` (used when the form says 3D).
    bool play(const core::Guid& sound, const Vec3& position, u32 seed);

private:
    const data::FormDatabase* forms { nullptr };
    const assets::AssetDatabase* assets { nullptr };
    audio::AudioSystem* audio { nullptr };
};

} // namespace game
