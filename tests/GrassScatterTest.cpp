#include <doctest/doctest.h>

#include "engine/render/landscape/GrassSystem.hpp"

// The grass scatter's RNG draw order and placement math are a contract:
// revisited meadows must be identical, and a refactor of scatterGrass is
// provably output-preserving only if the raw instance bytes do not move.
// Same harness as the prop scatter golden (VegetationScatterTest).

namespace {

using render::GrassSystem;

u64 hashInstances(const vector<GrassSystem::Instance>& instances) {
    u64 h = 1469598103934665603ull; // FNV-1a offset basis
    const auto mix = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    const u64 count = instances.size();
    mix(&count, sizeof(count));
    for (const GrassSystem::Instance& instance : instances) {
        mix(&instance.positionScale, sizeof(Vec4));
        mix(&instance.params, sizeof(Vec4));
        mix(&instance.groundNormal, sizeof(Vec4));
        mix(&instance.species, sizeof(Vec4));
    }
    return h;
}

} // namespace

TEST_CASE("grass scatter: deterministic and populated") {
    render::TerrainParams params; // seed 1337, pure procedural
    render::GrassScatterTuning tuning;
    u64 total = 0;
    for (i32 c = -4; c <= 4; c += 2) {
        const auto tufts = render::scatterGrass(params, tuning, c, -c + 1);
        const auto again = render::scatterGrass(params, tuning, c, -c + 1);
        CHECK(hashInstances(tufts) == hashInstances(again));
        total += tufts.size();
    }
    CHECK(total > 0);
}

TEST_CASE("grass scatter: instance buffers are frozen (golden)") {
    // Golden hash over the raw instance bytes of a chunk sample. An
    // INTENTIONAL tuning change re-captures it (print below); an
    // accidental one is what this test exists to catch. The raw-float
    // hash is sensitive to compiler FP codegen, so each platform /
    // toolchain keeps its OWN golden.
    render::TerrainParams params;
    render::GrassScatterTuning tuning;
    u64 h = 1469598103934665603ull;
    for (i32 c = -4; c <= 4; c += 2) {
        const u64 chunkHash =
            hashInstances(render::scatterGrass(params, tuning, c, -c + 1));
        h ^= chunkHash;
        h *= 1099511628211ull;
    }
    MESSAGE("grass scatter golden hash: " << h);
    const bool knownGolden = h == 8288326442141430962ull; // Windows x64 / MSVC
    CHECK(knownGolden);
}
