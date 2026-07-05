#pragma once

#include "data/forms/Form.hpp"

// Audio data Forms (horizontal pass H1). The audio runtime (engine/audio,
// miniaudio) consumes flat SoundParams; the runtime layer maps these Forms.
// Random variation picks use the seeded engine RNG only when the outcome
// affects gameplay — cosmetic picks may use a free RNG (§8).

namespace data {

class FormTypeRegistry;

// A playable sound event. Variations are CHILD records (SoundVariantForm)
// — a mod adds a variant without touching the parent.
struct SoundForm : Form {
    str bus { "sfx" };      // "sfx" | "music" | "voice" | "ambient" | "ui"
    f32 volume { 1.0f };
    f32 volumeJitter { 0.0f };
    f32 pitch { 1.0f };
    f32 pitchJitter { 0.0f };
    bool is3d { false };
    f32 minDistance { 1.0f };  // full volume inside
    f32 maxDistance { 30.0f }; // inaudible beyond
    bool loop { false };

    REFLECT_BEGIN(SoundForm, Form)
        REFLECT_FIELD(bus)
        REFLECT_FIELD(volume)
        REFLECT_FIELD(volumeJitter)
        REFLECT_FIELD(pitch)
        REFLECT_FIELD(pitchJitter)
        REFLECT_FIELD(is3d)
        REFLECT_FIELD(minDistance)
        REFLECT_FIELD(maxDistance)
        REFLECT_FIELD(loop)
    REFLECT_END()
};

struct SoundVariantForm : Form {
    core::Guid parent; // SoundForm
    core::Guid asset;  // audio file (VFS guid)
    f32 weight { 1.0f };

    REFLECT_BEGIN(SoundVariantForm, Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(asset)
        REFLECT_FIELD(weight)
    REFLECT_END()
};

void registerAudioFormTypes(FormTypeRegistry& registry);

} // namespace data
