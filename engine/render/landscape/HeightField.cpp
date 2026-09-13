#include "engine/render/landscape/HeightField.hpp"

#include <cmath>

namespace render {

f32 HeightField::Level::sample(f32 x, f32 z) const {
    const f32 u = (x - originX) / texel;
    const f32 v = (z - originZ) / texel;
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const i32 maxC = static_cast<i32>(n) - 2;
    const i32 c0 = glm::clamp(static_cast<i32>(fu), 0, maxC);
    const i32 r0 = glm::clamp(static_cast<i32>(fv), 0, maxC);
    const f32 tu = glm::clamp(u - static_cast<f32>(c0), 0.0f, 1.0f);
    const f32 tv = glm::clamp(v - static_cast<f32>(r0), 0.0f, 1.0f);
    const auto at = [&](i32 row, i32 col) {
        return heights[static_cast<size_t>(row) * n +
                       static_cast<size_t>(col)];
    };
    const f32 a = glm::mix(at(r0, c0), at(r0, c0 + 1), tu);
    const f32 b = glm::mix(at(r0 + 1, c0), at(r0 + 1, c0 + 1), tu);
    return glm::mix(a, b, tv);
}

f32 HeightField::Snapshot::height(f32 x, f32 z) const {
    for (const sptr<const Level>& level : levels) {
        if (level && level->covers(x, z)) {
            return level->sample(x, z);
        }
    }
    return terrain::height(params, x, z); // exact fallback
}

f32 HeightField::Snapshot::heightCoarse(f32 x, f32 z,
                                        f32 minTexel) const {
    for (const sptr<const Level>& level : levels) {
        if (level && level->texel >= minTexel && level->covers(x, z)) {
            return level->sample(x, z);
        }
    }
    // No coarse-enough cover: the finest cover still beats the exact
    // evaluation a march would pay per step.
    return height(x, z);
}

void HeightField::create(core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    // One mailbox per level: independent one-in-flight bakes; the probe
    // names land on the F6 worker table.
    static constexpr const char* kNames[kLevelCount] = {
        "heightField.L0", "heightField.L1", "heightField.L2"
    };
    for (u32 i = 0; i < kLevelCount; ++i) {
        mailboxes[i].create(jobSystem, kNames[i]);
    }
}

void HeightField::update(const TerrainParams& params, const Vec3& focus) {
    bool landed = false;
    for (u32 i = 0; i < kLevelCount; ++i) {
        mailboxes[i].drain([&](Baked& done) {
            levels[i] = std::move(done.level);
            seenStamps[i] = done.seenStamp;
            landed = true;
        });
    }
    if (landed) {
        publish(params);
    }
    // Kick stale levels, coarsest FIRST: the finer fills upsample the
    // captured snapshot where the ground is uncovered — only L2 ever
    // pays the analytic stack there (first boot excepted, where a fine
    // level may fill before any coarser cover exists: correct, once).
    for (i32 i = static_cast<i32>(kLevelCount) - 1; i >= 0; --i) {
        const u32 index = static_cast<u32>(i);
        if (mailboxes[index].busy()) {
            continue;
        }
        const LevelSpec spec = kLevels[index];
        const f32 span = static_cast<f32>(spec.n - 1) * spec.texel;
        // World-lattice snap (the MistMap no-crossfade idiom): overlap
        // texels of consecutive fills land on identical coordinates.
        const f32 wantX =
            std::floor((focus.x - span * 0.5f) / spec.texel) * spec.texel;
        const f32 wantZ =
            std::floor((focus.z - span * 0.5f) / spec.texel) * spec.texel;
        const sptr<const Level>& current = levels[index];
        const bool strayed =
            !current ||
            glm::max(std::abs(focus.x - (current->originX + span * 0.5f)),
                     std::abs(focus.z -
                              (current->originZ + span * 0.5f))) >
                span * 0.25f;
        const bool touched =
            current &&
            terrain::contentTouchedSince(params, seenStamps[index],
                                         current->originX,
                                         current->originZ,
                                         current->originX + span,
                                         current->originZ + span);
        if (!strayed && !touched) {
            continue;
        }
        mailboxes[index].kick([params, spec, wantX, wantZ,
                               coarser = snap](
                                  Baked& baked,
                                  const std::atomic<bool>& stop) {
            auto level = std::make_shared<Level>();
            level->originX = wantX;
            level->originZ = wantZ;
            level->texel = spec.texel;
            level->n = spec.n;
            level->heights.resize(static_cast<size_t>(spec.n) * spec.n);
            for (u32 row = 0; row < spec.n; ++row) {
                if (stop.load(std::memory_order_relaxed)) {
                    return; // shutdown: the mailbox drops the partial
                }
                const f32 z =
                    wantZ + static_cast<f32>(row) * spec.texel;
                for (u32 col = 0; col < spec.n; ++col) {
                    const f32 x =
                        wantX + static_cast<f32>(col) * spec.texel;
                    f32 h;
                    // Covered (or story terrain): the exact function is
                    // already on its cheap baked path. Uncovered
                    // SANDBOX: upsample a coarser covering level from
                    // the captured snapshot instead of the ~1-3 µs
                    // analytic stack; only the coarsest level (no
                    // coarser cover) pays it.
                    if (!params.sandbox ||
                        (params.base && params.base->regionAt(x, z))) {
                        h = terrain::height(params, x, z);
                    } else {
                        h = -1.0e9f;
                        if (coarser) {
                            for (const sptr<const Level>& c :
                                 coarser->levels) {
                                if (c && c->texel > spec.texel &&
                                    c->covers(x, z)) {
                                    h = c->sample(x, z);
                                    break;
                                }
                            }
                        }
                        if (h <= -1.0e9f) {
                            h = terrain::height(params, x, z);
                        }
                    }
                    level->heights[static_cast<size_t>(row) * spec.n +
                                   col] = h;
                }
            }
            baked.level = std::move(level);
            baked.seenStamp = params.contentStamp;
        });
    }
}

void HeightField::publish(const TerrainParams& params) {
    auto next = std::make_shared<Snapshot>();
    next->levels = levels;
    next->params = params;
    snap = next;
}

} // namespace render
