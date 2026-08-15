#include <cstring>

#include <doctest/doctest.h>

#include "engine/render/landscape/VegetationSystem.hpp"

// The prop scatter's RNG draw ORDER is a contract: every candidate draws
// (position, filters, variant, scale, yaw, tint, phase) in a fixed
// sequence, so reordering or merging draws reseeds every prop in the
// world. These tests freeze that contract so a refactor of scatterProps
// is provably output-preserving.

namespace {

using render::VegetationSystem;

u64 hashBuckets(const VegetationSystem::VariantBuckets& buckets) {
    u64 h = 1469598103934665603ull; // FNV-1a offset basis
    const auto mix = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& bucket : buckets) {
        const u64 count = bucket.size();
        mix(&count, sizeof(count));
        for (const VegetationSystem::Instance& instance : bucket) {
            mix(&instance.positionScale, sizeof(Vec4));
            mix(&instance.params, sizeof(Vec4));
        }
    }
    return h;
}

u64 tierCount(const VegetationSystem::VariantBuckets& buckets, u32 first,
              u32 count) {
    u64 total = 0;
    for (u32 v = first; v < first + count; ++v) {
        total += buckets[v].size();
    }
    return total;
}

} // namespace

TEST_CASE("vegetation scatter: deterministic and every tier populated") {
    render::TerrainParams params; // seed 1337, pure procedural

    // A few chunks spread over the default landscape so every tier
    // (trees, rocks, bushes, debris, plants, mass, pebbles) fires
    // somewhere in the sample.
    const std::pair<i32, i32> chunks[] = { { 0, 0 },   { 3, -2 },
                                           { -5, 7 },  { 9, 4 },
                                           { -11, -8 } };
    u64 trees = 0, rocks = 0, bushes = 0, debris = 0, plants = 0,
        mass = 0, pebbles = 0;
    for (const auto& [cx, cz] : chunks) {
        const auto buckets = render::scatterProps(params, cx, cz);
        const auto again = render::scatterProps(params, cx, cz);
        for (u32 v = 0; v < VegetationSystem::kVariantCount; ++v) {
            REQUIRE(buckets[v].size() == again[v].size());
            if (!buckets[v].empty()) {
                CHECK(std::memcmp(buckets[v].data(), again[v].data(),
                                  buckets[v].size() *
                                      sizeof(VegetationSystem::Instance)) ==
                      0);
            }
        }
        trees += tierCount(buckets, 0, VegetationSystem::kTreeVariants);
        rocks += tierCount(buckets, VegetationSystem::kFirstRock,
                           VegetationSystem::kRockVariants);
        bushes += tierCount(buckets, VegetationSystem::kFirstBush,
                            VegetationSystem::kBushVariants);
        debris += tierCount(buckets, VegetationSystem::kFirstDebris,
                            VegetationSystem::kDebrisVariants);
        plants += tierCount(buckets, VegetationSystem::kFirstPlant,
                            VegetationSystem::kPlantVariants);
        mass += tierCount(buckets, VegetationSystem::kFirstMass,
                          VegetationSystem::kMassVariants);
        pebbles += tierCount(buckets, VegetationSystem::kFirstPebble,
                             VegetationSystem::kPebbleVariants);
    }
    MESSAGE("trees=" << trees << " rocks=" << rocks << " bushes=" << bushes
                     << " debris=" << debris << " plants=" << plants
                     << " mass=" << mass << " pebbles=" << pebbles);
    CHECK(trees > 0);
    CHECK(rocks > 0);
    CHECK(bushes > 0);
    CHECK(debris > 0);
    CHECK(plants > 0);
    CHECK(mass > 0);
    CHECK(pebbles > 0);
}

TEST_CASE("vegetation scatter: instance buffers are frozen (golden)") {
    // Golden hash over the raw instance bytes of a chunk sample. It pins
    // the RNG draw sequence AND the placement math: an INTENTIONAL
    // tuning change re-captures it (print below); an accidental one is
    // exactly what this test exists to catch.
    render::TerrainParams params;
    u64 h = 1469598103934665603ull;
    for (i32 c = -6; c <= 6; c += 3) {
        const u64 chunkHash =
            hashBuckets(render::scatterProps(params, c, -c + 1));
        h ^= chunkHash;
        h *= 1099511628211ull;
    }
    MESSAGE("scatter golden hash: " << h);
    CHECK(h == 17957688382628280143ull); // 0xf9368670a9b7b74f
}
