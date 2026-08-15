#include <doctest/doctest.h>

#include "engine/audio/Audio.hpp"

// The audio seam runs fully headless on miniaudio's null backend —
// same code paths as the real device, no hardware, CI-safe.

TEST_CASE("audio system lifecycle on the null backend") {
    audio::AudioSystem system;
    REQUIRE(system.create(/*nullBackend=*/true));
    CHECK(system.ready());

    system.setBusVolume("sfx", 0.5f);
    system.setBusVolume("nonexistent", 1.0f); // silently ignored
    system.setListener({ 0.0f, 1.7f, 0.0f }, { 0.0f, 0.0f, -1.0f });

    // Missing file: graceful false, no crash.
    CHECK_FALSE(system.play({ .file = "does-not-exist.wav" }));

    // Generated tone: plays (null backend consumes it) and reaps on time.
    system.playTestTone(0.05f);
    for (int i = 0; i < 10; ++i) {
        system.update(0.016f);
    }

    system.destroy();
    CHECK_FALSE(system.ready());

    // Re-creatable after destroy.
    REQUIRE(system.create(/*nullBackend=*/true));
    system.destroy();
}
