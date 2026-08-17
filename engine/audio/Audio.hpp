#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// The audio seam (docs/HORIZONTAL-PASS.md): miniaudio behind a narrow facade —
// no ma_* type crosses this header. Plain params in (the
// runtime layer maps SoundForm + a picked SoundVariantForm onto
// SoundParams; cosmetic random picks may use a free RNG, gameplay-
// affecting ones go through core::Rng, §8).
//
// Buses are a fixed set (sfx/music/voice/ambient/ui), each a mixer group
// with its own volume — the options screen binds to setBusVolume.
//
// Planned extension points:
//  - SoundForm resolver: variants by weight, volume/pitch jitter, asset
//    path through the plugin VFS -> play();
//  - ambient beds: crossfade loops per cell/weather/time through
//    playMusic-style slots on the "ambient" bus;
//  - 3D: call setListener from the camera each frame; emitters pass
//    is3d + position (attenuation min/max already plumbed);
//  - footsteps/cues: GameplayCues call play() from cue handlers.

namespace audio {

struct SoundParams {
    str file;             // absolute path (VFS-resolved by the caller)
    str bus { "sfx" };    // sfx | music | voice | ambient | ui
    f32 volume { 1.0f };
    f32 pitch { 1.0f };
    bool is3d { false };
    Vec3 position { 0.0f };
    f32 minDistance { 1.0f };
    f32 maxDistance { 30.0f };
    bool loop { false };
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // `nullBackend` = headless (tests/CI): full API, no device.
    bool create(bool nullBackend = false);
    void destroy();

    // Reaps finished one-shots; call once per frame.
    void update(f32 dt);

    void setListener(const Vec3& position, const Vec3& forward);
    void setBusVolume(std::string_view bus, f32 volume);

    // Fire-and-forget (or looping) sound. 0 if the file failed; the
    // nonzero id is only needed to stop() a LOOP (one-shots reap
    // themselves at end).
    using SoundId = u64;
    SoundId play(const SoundParams& params);
    // Stops a playing sound (loops included) with a short fade; unknown
    // or already-finished ids are ignored.
    void stop(SoundId id, f32 fadeSeconds = 0.05f);

    // Music slot with crossfade: the previous track fades out while the
    // new one fades in over `fadeSeconds`.
    bool playMusic(const str& file, f32 fadeSeconds = 2.0f);
    void stopMusic(f32 fadeSeconds = 2.0f);

    // A generated sine beep — the no-asset audible proof (dev/tools).
    void playTestTone(f32 seconds = 0.3f, f32 frequency = 440.0f);

    bool ready() const { return created; }

    struct Impl;

private:
    uptr<Impl> pimpl;
    bool created { false };
};

} // namespace audio
