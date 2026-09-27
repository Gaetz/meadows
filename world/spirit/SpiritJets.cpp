#include "world/spirit/SpiritJets.hpp"

#include <algorithm>

namespace world {

std::optional<JetLanding> jetLanding(const Vec3& origin, const Vec3& velocity,
                                     f32 gravity, const GroundHeightFn& height,
                                     f32 maxSeconds, f32 step) {
    if (step <= 0.0f || maxSeconds <= 0.0f) {
        return std::nullopt;
    }
    auto at = [&](f32 t) {
        return Vec3 { origin.x + velocity.x * t,
                      origin.y + velocity.y * t - 0.5f * gravity * t * t,
                      origin.z + velocity.z * t };
    };
    auto below = [&](const Vec3& p) { return p.y <= height(p.x, p.z); };
    // A nozzle already under the ground (a crouched caster against a
    // slope) lands where it stands.
    if (below(origin)) {
        return JetLanding { Vec3 { origin.x, height(origin.x, origin.z),
                                   origin.z },
                            0.0f };
    }
    f32 prev = 0.0f;
    for (f32 t = step; t <= maxSeconds + 1e-4f; t += step) {
        if (below(at(t))) {
            // One bisection pass: the arc crossed the ground between
            // prev and t.
            f32 lo = prev;
            f32 hi = t;
            for (int i = 0; i < 8; ++i) {
                const f32 mid = 0.5f * (lo + hi);
                (below(at(mid)) ? hi : lo) = mid;
            }
            Vec3 p = at(hi);
            p.y = height(p.x, p.z);
            return JetLanding { p, hi };
        }
        prev = t;
    }
    return std::nullopt;
}

u32 SpiritJetList::start(SpiritJet jet) {
    u32 evicted = 0;
    if (list.size() >= kMaxJets) {
        evicted = list.front().emitter;
        list.erase(list.begin());
    }
    list.push_back(std::move(jet));
    return evicted;
}

bool SpiritJetList::tick(f32 simSeconds, vector<u32>* stopped) {
    bool changed = false;
    for (SpiritJet& jet : list) {
        jet.remaining = glm::max(0.0f, jet.remaining - simSeconds);
    }
    for (auto it = list.begin(); it != list.end();) {
        if (it->remaining <= 0.0f) {
            if (stopped && it->emitter != 0) {
                stopped->push_back(it->emitter);
            }
            it = list.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

void SpiritJetList::aim(const Vec3& origin, const Vec3& forward) {
    const f32 len = glm::length(forward);
    if (len < 1e-4f) {
        return;
    }
    const Vec3 dir = forward / len;
    for (SpiritJet& jet : list) {
        if (!jet.followsCaster) {
            continue;
        }
        const f32 speed = glm::length(jet.velocity);
        jet.origin = origin;
        jet.velocity = dir * speed;
    }
}

void SpiritJetList::resolveLandings(const GroundHeightFn& height) {
    for (SpiritJet& jet : list) {
        jet.landing = jetLanding(jet.origin, jet.velocity, kGravity, height);
    }
}

void SpiritJetList::appendWaterSources(
    vector<render::terraingen::WaterSource>& out) const {
    for (const SpiritJet& jet : list) {
        if (jet.kind != render::terrain::SpiritKind::Water || !jet.landing ||
            jet.rate <= 0.0f) {
            continue;
        }
        out.push_back({ jet.landing->point.x, jet.landing->point.z,
                        jet.rate });
    }
}

vector<u32> SpiritJetList::clear() {
    vector<u32> handles;
    for (const SpiritJet& jet : list) {
        if (jet.emitter != 0) {
            handles.push_back(jet.emitter);
        }
    }
    list.clear();
    return handles;
}

} // namespace world
