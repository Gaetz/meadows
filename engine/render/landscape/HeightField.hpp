#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/BakeMailbox.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

namespace render {

// The SHARED height sampling layer (chantier économie, docs/CPU-PERF.md):
// a camera-centered three-level pyramid of height grids the coarse
// visual consumers (light/shade/mist/pool maps, occlusion, far terrain,
// minimap) sample instead of each re-evaluating terrain::height()
// pointwise over the same square. Levels fill on workers from the exact
// function (covered ground pays the cheap baked path; only the far
// level ever pays the analytic stack for uncovered sandbox — the finer
// levels upsample it there), publish as immutable snapshots (the
// TerrainBase contract: sptr swap on main, lock-free reads from any
// worker), and invalidate through the rect-scoped content events.
//
// The snapshot's height() falls back to the exact function outside
// every level, so a consumer is never wrong — at worst slow. NOT for
// the golden-hash consumers (scatter, grass, collision, meshing, nav,
// water sim): those keep terrain::height() exactly (docs/CPU-PERF.md,
// contraintes dures).
class HeightField {
public:
    struct LevelSpec {
        f32 texel;
        u32 n; // samples per side; span = (n - 1) * texel
    };
    static constexpr u32 kLevelCount = 3;
    // L0 4 m/4 km (near maps), L1 16 m/8 km (sun-march mid range),
    // L2 64 m/32 km (far terrain, minimap L, march tails).
    static constexpr LevelSpec kLevels[kLevelCount] = { { 4.0f, 1025 },
                                                       { 16.0f, 513 },
                                                       { 64.0f, 513 } };

    struct Level {
        f32 originX { 0.0f };
        f32 originZ { 0.0f };
        f32 texel { 0.0f };
        u32 n { 0 };
        vector<f32> heights; // n*n, row-major (+Z rows)

        f32 spanMeters() const { return static_cast<f32>(n - 1) * texel; }
        bool covers(f32 x, f32 z) const {
            return n >= 2 && x >= originX && z >= originZ &&
                   x <= originX + spanMeters() &&
                   z <= originZ + spanMeters();
        }
        // Bilinear, edge-clamped. Only meaningful where covers().
        f32 sample(f32 x, f32 z) const;
    };

    // Immutable once published; workers capture it by shared ownership.
    struct Snapshot {
        array<sptr<const Level>, kLevelCount> levels {};
        TerrainParams params; // exact-fallback source (shared layers)

        // Finest covering level, bilinear; exact terrain::height()
        // outside every level. Never wrong, at worst slow.
        f32 height(f32 x, f32 z) const;
        // Ray marches pick their footprint: the finest covering level
        // whose texel >= minTexel (coarser is fine, finer wastes
        // cache); exact fallback outside.
        f32 heightCoarse(f32 x, f32 z, f32 minTexel) const;
    };

    void create(core::JobSystem& jobSystem);

    // Main thread, once per frame: land finished fills, re-kick stale
    // levels (stray past the inner quarter, or content touched in the
    // level rect). Coarse levels kick first so the finer fills can
    // upsample them where the ground is uncovered.
    void update(const TerrainParams& params, const Vec3& focus);

    // Null until the first level lands.
    sptr<const Snapshot> snapshot() const { return snap; }

private:
    struct Baked {
        sptr<Level> level;
        u64 seenStamp { 0 };
        u64 gen { 0 };
    };

    void publish(const TerrainParams& params);

    array<BakeMailbox<Baked>, kLevelCount> mailboxes;
    array<sptr<const Level>, kLevelCount> levels {};
    array<u64, kLevelCount> seenStamps {};
    sptr<const Snapshot> snap;
    core::JobSystem* jobs { nullptr };
};

} // namespace render
