#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "data/forms/AudioForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "game/SoundResolver.hpp"

// Chantier P0 C3 — the SoundForm resolver on the NULL audio backend:
// weighted variant picks (deterministic per seed), volume/pitch jitter
// within bounds, VFS-resolved paths, graceful nothing on bad data.

namespace {

constexpr const char* kToml = R"toml(
[plugin]
id = "50a4d5e3-0000-4000-8000-000000000000"
name = "sound-test"

[[records]]
form = "50a4d5e3-0000-4000-8000-000000000001"
type = "SoundForm"
new = true
[records.fields]
editorId = "Clang"
bus = "sfx"
volume = 0.8
volumeJitter = 0.1
pitch = 1.0
pitchJitter = 0.2
is3d = true
minDistance = 2.0
maxDistance = 25.0

[[records]]
form = "50a4d5e3-0000-4000-8000-000000000002"
type = "SoundVariantForm"
new = true
[records.fields]
parent = "50a4d5e3-0000-4000-8000-000000000001"
asset = "50a4d5e3-0000-4000-8000-0000000000a1"
weight = 1.0

[[records]]
form = "50a4d5e3-0000-4000-8000-000000000003"
type = "SoundVariantForm"
new = true
[records.fields]
parent = "50a4d5e3-0000-4000-8000-000000000001"
asset = "50a4d5e3-0000-4000-8000-0000000000a2"
weight = 3.0

[[records]]
form = "50a4d5e3-0000-4000-8000-000000000004"
type = "SoundForm"
new = true
[records.fields]
editorId = "NoVariants"
)toml";

const core::Guid kClang =
    *core::Guid::fromString("50a4d5e3-0000-4000-8000-000000000001");
const core::Guid kNoVariants =
    *core::Guid::fromString("50a4d5e3-0000-4000-8000-000000000004");
const core::Guid kAssetA =
    *core::Guid::fromString("50a4d5e3-0000-4000-8000-0000000000a1");
const core::Guid kAssetB =
    *core::Guid::fromString("50a4d5e3-0000-4000-8000-0000000000a2");

} // namespace

TEST_CASE("the sound resolver picks weighted variants and jitters "
          "within bounds") {
    data::FormTypeRegistry types;
    data::registerAudioFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "sound-test");
    REQUIRE(plugin.has_value());
    data::FormDatabase forms;
    data::resolve({ &*plugin }, types, forms);

    // Two fake files the VFS can actually resolve.
    const auto dir = std::filesystem::temp_directory_path() / "meadows-c3";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "clang_a.wav") << "x";
    std::ofstream(dir / "clang_b.wav") << "x";
    assets::AssetDatabase assetDb;
    assetDb.add(kAssetA, dir, "clang_a.wav");
    assetDb.add(kAssetB, dir, "clang_b.wav");

    game::SoundResolver resolver;
    audio::AudioSystem nullAudio;
    nullAudio.create(/*nullBackend=*/true);
    resolver.create(forms, assetDb, &nullAudio);

    // Deterministic: the same seed resolves the same way, twice.
    const Vec3 at { 1.0f, 2.0f, 3.0f };
    const auto first = resolver.resolve(kClang, at, 7);
    const auto again = resolver.resolve(kClang, at, 7);
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    CHECK(first->file == again->file);
    CHECK(first->volume == doctest::Approx(again->volume));
    CHECK(first->pitch == doctest::Approx(again->pitch));

    // The form's fields map through; jitter stays inside its bounds.
    CHECK(first->bus == "sfx");
    CHECK(first->is3d);
    CHECK(first->position.x == doctest::Approx(1.0f));
    CHECK(first->minDistance == doctest::Approx(2.0f));
    CHECK(first->maxDistance == doctest::Approx(25.0f));
    u32 pickedB = 0;
    for (u32 seed = 0; seed < 64; ++seed) {
        const auto params = resolver.resolve(kClang, at, seed);
        REQUIRE(params.has_value());
        CHECK(params->volume >= 0.7f - 1e-4f);
        CHECK(params->volume <= 0.9f + 1e-4f);
        CHECK(params->pitch >= 0.8f - 1e-4f);
        CHECK(params->pitch <= 1.2f + 1e-4f);
        if (params->file.find("clang_b") != str::npos) {
            ++pickedB;
        }
    }
    // Weight 3 vs 1: B dominates (loose statistical bound).
    CHECK(pickedB > 32);
    CHECK(pickedB < 64);

    // play() runs the full pipeline; the fake one-byte "wav" fails to
    // DECODE (even the null backend parses files) and the failure comes
    // back as a clean false — never a throw. Real assets are the dev's
    // audible validation.
    CHECK(!resolver.play(kClang, at, 3));

    // Graceful nothing: a form without resolvable variants, an unknown
    // guid, an invalid guid.
    CHECK(!resolver.resolve(kNoVariants, at, 1).has_value());
    CHECK(!resolver.resolve(core::Guid {}, at, 1).has_value());
    CHECK(!resolver.play(kNoVariants, at, 1));
}
