#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include "engine/audio/Audio.hpp"

#include <unordered_map>

#include "engine/core/Log.hpp"

namespace audio {

namespace {
constexpr const char* kBuses[] = { "sfx", "music", "voice", "ambient",
                                   "ui" };
}

struct AudioSystem::Impl {
    ma_context context {};
    bool contextInitialized { false };
    ma_engine engine {};
    bool engineInitialized { false };
    // Groups are nodes of the engine's graph: their addresses must be
    // stable for the engine's lifetime — heap-allocate them.
    std::unordered_map<str, uptr<ma_sound_group>> buses;

    struct ActiveSound {
        u64 id { 0 };
        uptr<ma_sound> sound;
        uptr<ma_waveform> waveform; // test tones only
        f32 secondsLeft { 0.0f };   // waveform sounds have no natural end
        bool timed { false };
    };
    vector<ActiveSound> active;
    u64 nextSoundId { 1 }; // 0 = the failed-play sentinel

    // Two music slots for the crossfade.
    uptr<ma_sound> music[2];
    u32 musicFront { 0 };

    ma_sound_group* busOf(std::string_view name) {
        const auto it = buses.find(str { name });
        return it != buses.end() ? it->second.get() : nullptr;
    }
};

AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() {
    destroy();
}

bool AudioSystem::create(bool nullBackend) {
    pimpl = std::make_unique<Impl>();
    auto& impl = *pimpl;

    if (nullBackend) {
        ma_backend backends[] = { ma_backend_null };
        if (ma_context_init(backends, 1, nullptr, &impl.context) !=
            MA_SUCCESS) {
            LOG_ERROR("AudioSystem: null context init failed");
            return false;
        }
        impl.contextInitialized = true;
    }
    ma_engine_config config = ma_engine_config_init();
    if (impl.contextInitialized) {
        config.pContext = &impl.context;
    }
    if (ma_engine_init(&config, &impl.engine) != MA_SUCCESS) {
        LOG_ERROR("AudioSystem: engine init failed");
        return false;
    }
    impl.engineInitialized = true;

    for (const char* bus : kBuses) {
        auto group = std::make_unique<ma_sound_group>();
        if (ma_sound_group_init(&impl.engine, 0, nullptr, group.get()) ==
            MA_SUCCESS) {
            impl.buses.emplace(bus, std::move(group));
        }
    }
    created = true;
    return true;
}

void AudioSystem::destroy() {
    if (!pimpl) {
        return;
    }
    auto& impl = *pimpl;
    for (auto& active : impl.active) {
        ma_sound_uninit(active.sound.get());
        if (active.waveform) {
            ma_waveform_uninit(active.waveform.get());
        }
    }
    impl.active.clear();
    for (auto& slot : impl.music) {
        if (slot) {
            ma_sound_uninit(slot.get());
            slot.reset();
        }
    }
    for (auto& [name, group] : impl.buses) {
        ma_sound_group_uninit(group.get());
    }
    impl.buses.clear();
    if (impl.engineInitialized) {
        ma_engine_uninit(&impl.engine);
    }
    if (impl.contextInitialized) {
        ma_context_uninit(&impl.context);
    }
    pimpl.reset();
    created = false;
}

void AudioSystem::update(f32 dt) {
    if (!pimpl) {
        return;
    }
    auto& impl = *pimpl;
    for (auto it = impl.active.begin(); it != impl.active.end();) {
        bool finished = false;
        if (it->timed) {
            it->secondsLeft -= dt;
            finished = it->secondsLeft <= 0.0f;
        } else {
            finished = ma_sound_at_end(it->sound.get()) != 0;
        }
        if (finished) {
            ma_sound_uninit(it->sound.get());
            if (it->waveform) {
                ma_waveform_uninit(it->waveform.get());
            }
            it = impl.active.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::setListener(const Vec3& position, const Vec3& forward) {
    if (!pimpl || !pimpl->engineInitialized) {
        return;
    }
    ma_engine_listener_set_position(&pimpl->engine, 0, position.x,
                                    position.y, position.z);
    ma_engine_listener_set_direction(&pimpl->engine, 0, forward.x,
                                     forward.y, forward.z);
}

void AudioSystem::setBusVolume(std::string_view bus, f32 volume) {
    if (!pimpl) {
        return;
    }
    if (ma_sound_group* group = pimpl->busOf(bus)) {
        ma_sound_group_set_volume(group, volume);
    } // unknown bus: silently ignored (mods can't crash the mixer)
}

AudioSystem::SoundId AudioSystem::play(const SoundParams& params) {
    if (!pimpl || !pimpl->engineInitialized) {
        return 0;
    }
    auto& impl = *pimpl;
    Impl::ActiveSound active;
    active.id = impl.nextSoundId++;
    active.sound = std::make_unique<ma_sound>();
    const ma_uint32 flags = params.is3d ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(&impl.engine, params.file.c_str(), flags,
                                impl.busOf(params.bus), nullptr,
                                active.sound.get()) != MA_SUCCESS) {
        LOG_WARN("AudioSystem: cannot play '{}'", params.file);
        return 0;
    }
    ma_sound_set_volume(active.sound.get(), params.volume);
    ma_sound_set_pitch(active.sound.get(), params.pitch);
    ma_sound_set_looping(active.sound.get(),
                         params.loop ? MA_TRUE : MA_FALSE);
    if (params.is3d) {
        ma_sound_set_position(active.sound.get(), params.position.x,
                              params.position.y, params.position.z);
        ma_sound_set_min_distance(active.sound.get(), params.minDistance);
        ma_sound_set_max_distance(active.sound.get(), params.maxDistance);
        ma_sound_set_attenuation_model(active.sound.get(),
                                       ma_attenuation_model_linear);
    }
    ma_sound_start(active.sound.get());
    const SoundId id = active.id;
    impl.active.push_back(std::move(active));
    return id;
}

void AudioSystem::stop(SoundId id, f32 fadeSeconds) {
    if (!pimpl || id == 0) {
        return;
    }
    for (Impl::ActiveSound& active : pimpl->active) {
        if (active.id != id) {
            continue;
        }
        // Fade to silence, then let update()'s timed reap uninit it —
        // the same path the test tones already take.
        ma_sound_set_fade_in_milliseconds(
            active.sound.get(), -1.0f, 0.0f,
            static_cast<ma_uint64>(fadeSeconds * 1000.0f));
        active.timed = true;
        active.secondsLeft = fadeSeconds;
        return;
    }
}

bool AudioSystem::playMusic(const str& file, f32 fadeSeconds) {
    if (!pimpl || !pimpl->engineInitialized) {
        return false;
    }
    auto& impl = *pimpl;
    const ma_uint64 fadeMs =
        static_cast<ma_uint64>(fadeSeconds * 1000.0f);
    // Fade the current track out.
    if (impl.music[impl.musicFront]) {
        ma_sound_set_fade_in_milliseconds(
            impl.music[impl.musicFront].get(), -1.0f, 0.0f, fadeMs);
    }
    const u32 slot = 1 - impl.musicFront;
    if (impl.music[slot]) {
        ma_sound_uninit(impl.music[slot].get());
        impl.music[slot].reset();
    }
    auto sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_file(&impl.engine, file.c_str(),
                                MA_SOUND_FLAG_NO_SPATIALIZATION |
                                    MA_SOUND_FLAG_STREAM,
                                impl.busOf("music"), nullptr,
                                sound.get()) != MA_SUCCESS) {
        LOG_WARN("AudioSystem: cannot stream '{}'", file);
        return false;
    }
    ma_sound_set_looping(sound.get(), MA_TRUE);
    ma_sound_set_fade_in_milliseconds(sound.get(), 0.0f, 1.0f, fadeMs);
    ma_sound_start(sound.get());
    impl.music[slot] = std::move(sound);
    impl.musicFront = slot;
    return true;
}

void AudioSystem::stopMusic(f32 fadeSeconds) {
    if (!pimpl) {
        return;
    }
    if (pimpl->music[pimpl->musicFront]) {
        ma_sound_set_fade_in_milliseconds(
            pimpl->music[pimpl->musicFront].get(), -1.0f, 0.0f,
            static_cast<ma_uint64>(fadeSeconds * 1000.0f));
    }
}

void AudioSystem::playTestTone(f32 seconds, f32 frequency) {
    if (!pimpl || !pimpl->engineInitialized) {
        return;
    }
    auto& impl = *pimpl;
    Impl::ActiveSound active;
    active.waveform = std::make_unique<ma_waveform>();
    const ma_waveform_config config = ma_waveform_config_init(
        ma_format_f32, 2, ma_engine_get_sample_rate(&impl.engine),
        ma_waveform_type_sine, 0.2, frequency);
    if (ma_waveform_init(&config, active.waveform.get()) != MA_SUCCESS) {
        return;
    }
    active.sound = std::make_unique<ma_sound>();
    if (ma_sound_init_from_data_source(
            &impl.engine, active.waveform.get(),
            MA_SOUND_FLAG_NO_SPATIALIZATION, impl.busOf("sfx"),
            active.sound.get()) != MA_SUCCESS) {
        ma_waveform_uninit(active.waveform.get());
        return;
    }
    active.timed = true;
    active.secondsLeft = seconds;
    ma_sound_start(active.sound.get());
    impl.active.push_back(std::move(active));
}

} // namespace audio
