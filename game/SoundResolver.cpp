#include "game/SoundResolver.hpp"

#include <glm/glm.hpp>

#include "data/forms/AudioForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp" // data::childrenOf
#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/Hash.hpp"

namespace game {

void SoundResolver::create(const data::FormDatabase& formsIn,
                           const assets::AssetDatabase& assetsIn,
                           audio::AudioSystem* audioIn) {
    forms = &formsIn;
    assets = &assetsIn;
    audio = audioIn;
}

std::optional<audio::SoundParams> SoundResolver::resolve(
    const core::Guid& sound, const Vec3& position, u32 seed) const {
    if (!forms || !assets || !sound.isValid()) {
        return std::nullopt;
    }
    const auto* form = forms->find<data::SoundForm>(sound);
    if (!form) {
        return std::nullopt;
    }
    // Weighted pick over the child variants (skipping unresolvable
    // assets — a mod may reference a file it forgot to ship).
    struct Candidate {
        str file;
        f32 weight;
    };
    vector<Candidate> candidates;
    f32 total = 0.0f;
    data::childrenOf<data::SoundVariantForm>(
        *forms, sound, [&](const data::SoundVariantForm& variant) {
            if (variant.weight <= 0.0f || !variant.asset.isValid()) {
                return;
            }
            const auto path = assets->resolve(variant.asset);
            if (!path) {
                return;
            }
            candidates.push_back({ path->string(), variant.weight });
            total += variant.weight;
        });
    if (candidates.empty()) {
        return std::nullopt;
    }
    core::HashRng rng { core::hashU32(seed ^ 0x50a4d5e3u) };
    f32 roll = rng.next() * total;
    const Candidate* picked = &candidates.back();
    for (const Candidate& candidate : candidates) {
        if (roll < candidate.weight) {
            picked = &candidate;
            break;
        }
        roll -= candidate.weight;
    }
    audio::SoundParams params;
    params.file = picked->file;
    params.bus = form->bus;
    params.volume =
        glm::max(form->volume + rng.spread() * form->volumeJitter, 0.0f);
    params.pitch =
        glm::max(form->pitch + rng.spread() * form->pitchJitter, 0.05f);
    params.is3d = form->is3d;
    params.position = position;
    params.minDistance = form->minDistance;
    params.maxDistance = form->maxDistance;
    params.loop = form->loop;
    return params;
}

bool SoundResolver::play(const core::Guid& sound, const Vec3& position,
                         u32 seed) {
    if (!audio || !audio->ready()) {
        return false;
    }
    const auto params = resolve(sound, position, seed);
    return params && audio->play(*params);
}

} // namespace game
