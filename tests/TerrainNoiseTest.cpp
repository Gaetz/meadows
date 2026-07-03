#include <doctest/doctest.h>

#include "engine/render/landscape/TerrainNoise.hpp"

using render::TerrainParams;

namespace {
constexpr f32 kProbes[][2] = {
    { 0.0f, 0.0f },       { 12.5f, -7.25f },    { 63.999f, 64.001f },
    { -300.25f, 481.5f }, { 5000.0f, -5000.0f }, { 123456.0f, 7890.5f },
};
} // namespace

TEST_CASE("terrain height is deterministic across instances and calls") {
    TerrainParams a;
    a.seed = 42;
    TerrainParams b;
    b.seed = 42;
    for (const auto& probe : kProbes) {
        const f32 first = render::terrain::height(a, probe[0], probe[1]);
        const f32 second = render::terrain::height(b, probe[0], probe[1]);
        const f32 third = render::terrain::height(a, probe[0], probe[1]);
        // Bit-exact: chunk borders rely on identical re-evaluation.
        CHECK(first == second);
        CHECK(first == third);
    }
}

TEST_CASE("different seeds produce different terrain") {
    TerrainParams a;
    a.seed = 42;
    TerrainParams b;
    b.seed = 43;
    u32 differing = 0;
    for (const auto& probe : kProbes) {
        if (render::terrain::height(a, probe[0], probe[1]) !=
            render::terrain::height(b, probe[0], probe[1])) {
            ++differing;
        }
    }
    CHECK(differing == std::size(kProbes));
}

TEST_CASE("terrain normals are unit length and never inverted") {
    TerrainParams params;
    params.seed = 1337;
    for (const auto& probe : kProbes) {
        const Vec3 n = render::terrain::normal(params, probe[0], probe[1]);
        CHECK(glm::length(n) == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(n.y > 0.0f); // heightmap: the surface never overhangs
    }
}

TEST_CASE("terrain height stays within configured amplitude bounds") {
    TerrainParams params;
    params.seed = 7;
    const f32 maxHeight = params.hillAmplitude + params.mountainAmplitude;
    for (i32 gz = -20; gz <= 20; ++gz) {
        for (i32 gx = -20; gx <= 20; ++gx) {
            const f32 h = render::terrain::height(
                params, static_cast<f32>(gx) * 97.3f,
                static_cast<f32>(gz) * 101.7f);
            CHECK(h >= -maxHeight);
            CHECK(h <= maxHeight);
        }
    }
}
