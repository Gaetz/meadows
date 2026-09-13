#include <chrono>
#include <thread>

#include <doctest/doctest.h>

#include "engine/core/Jobs.hpp"
#include "engine/render/landscape/HeightField.hpp"

// The shared height pyramid (chantier économie, E3): worker-filled
// camera-centered grids the coarse visual consumers sample instead of
// pointwise terrain::height(). Contracts pinned here: exact equality at
// texel centers, bounded bilinear error between them, exact fallback
// outside every level, coarse-level selection for marches.

namespace {

// Pump update() until every level has landed (worker fills).
void fillField(render::HeightField& field,
               const render::TerrainParams& params, const Vec3& focus) {
    for (u32 spin = 0; spin < 2000; ++spin) {
        field.update(params, focus);
        const auto snap = field.snapshot();
        if (snap && snap->levels[0] && snap->levels[1] &&
            snap->levels[2]) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FAIL("height field never filled");
}

} // namespace

TEST_CASE("height field: exact at texel centers, bounded between") {
    core::JobSystem jobs { 2 };
    render::HeightField field;
    field.create(jobs);
    render::TerrainParams params; // pure procedural (legacy noise)
    const Vec3 focus { 100.0f, 0.0f, -250.0f };
    fillField(field, params, focus);
    const auto snap = field.snapshot();
    REQUIRE(snap);

    const auto& l0 = *snap->levels[0];
    // Texel centers of L0: the fill sampled the exact function there —
    // bilinear at a lattice point returns the sample untouched.
    for (u32 row = 10; row < l0.n - 10; row += 97) {
        for (u32 col = 10; col < l0.n - 10; col += 89) {
            const f32 x = l0.originX + static_cast<f32>(col) * l0.texel;
            const f32 z = l0.originZ + static_cast<f32>(row) * l0.texel;
            REQUIRE(snap->height(x, z) ==
                    render::terrain::height(params, x, z)); // EXACT
        }
    }
    // Between texels: bilinear over a 4 m grid of a smooth field —
    // bounded error against the exact function.
    f32 worst = 0.0f;
    for (f32 z = -400.0f; z <= 400.0f; z += 13.7f) {
        for (f32 x = -400.0f; x <= 400.0f; x += 17.3f) {
            worst = glm::max(
                worst, std::abs(snap->height(x, z) -
                                render::terrain::height(params, x, z)));
        }
    }
    CHECK(worst < 3.0f); // hills at 500 m wavelength vs 4 m texels
}

TEST_CASE("height field: exact fallback outside, coarse selection") {
    core::JobSystem jobs { 2 };
    render::HeightField field;
    field.create(jobs);
    render::TerrainParams params;
    const Vec3 focus { 0.0f, 0.0f, 0.0f };
    fillField(field, params, focus);
    const auto snap = field.snapshot();
    REQUIRE(snap);

    // 100 km out: outside every level — the exact function answers.
    CHECK(snap->height(100000.0f, 100000.0f) ==
          render::terrain::height(params, 100000.0f, 100000.0f));

    // heightCoarse honours the footprint: a 16 m request inside L0's
    // span must read a texel >= 16 m (L1), not L0.
    const auto& l1 = *snap->levels[1];
    const f32 x = 50.0f, z = 50.0f;
    REQUIRE(l1.covers(x, z));
    CHECK(snap->heightCoarse(x, z, 16.0f) == l1.sample(x, z));
    // A 4 m request reads the finest cover (L0).
    CHECK(snap->heightCoarse(x, z, 4.0f) == snap->levels[0]->sample(x, z));
}

TEST_CASE("height field: content events re-fill the touched level") {
    core::JobSystem jobs { 2 };
    render::HeightField field;
    field.create(jobs);
    render::TerrainParams params;
    const Vec3 focus { 0.0f, 0.0f, 0.0f };
    fillField(field, params, focus);

    // A content event far outside every level: nothing re-fills.
    params.contentEvents.push(1, 1.0e6f, 1.0e6f, 1.001e6f, 1.001e6f);
    params.contentStamp = 1;
    field.update(params, focus);
    CHECK(!field.snapshot()->levels[0]->heights.empty()); // still live
    // (no simple busy probe: assert indirectly — an in-flight refill
    // would land with seenStamp 1; give it a beat and check stability)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto before = field.snapshot();
    field.update(params, focus);
    CHECK(field.snapshot() == before); // no re-publish happened

    // An event inside L0: the level re-fills (snapshot re-publishes).
    params.contentEvents.push(2, -100.0f, -100.0f, 100.0f, 100.0f);
    params.contentStamp = 2;
    for (u32 spin = 0; spin < 2000; ++spin) {
        field.update(params, focus);
        if (field.snapshot() != before) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(field.snapshot() != before);
}
