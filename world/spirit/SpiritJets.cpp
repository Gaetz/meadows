#include "world/spirit/SpiritJets.hpp"

#include <algorithm>
#include <cmath>

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

bool SpiritHold::absorb(f32 dt, bool present) {
    if (present && rate > 0.0f) {
        volume = glm::min(maxVolume, volume + rate * glm::max(dt, 0.0f));
    }
    return volume < maxVolume;
}

f32 SpiritHold::dropDischarge(f32 seconds) const {
    return seconds > 0.0f ? volume / seconds : 0.0f;
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
            // The gesture is over: stop the stream (emitter, kernel
            // source — appendWaterSources skips a spent jet) but keep the
            // jet while lumps are still falling; it leaves with the last.
            if (stopped && it->emitter != 0) {
                stopped->push_back(it->emitter);
                it->emitter = 0;
                changed = true;
            }
            if (it->spheres.empty()) {
                it = list.erase(it);
                changed = true;
                continue;
            }
        }
        ++it;
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

void SpiritJetList::advanceSpheres(f32 dt, f32 interval, f32 gravity,
                                   vector<Vec3>* landed) {
    if (interval <= 0.0f) {
        return;
    }
    for (SpiritJet& jet : list) {
        for (JetSphere& sphere : jet.spheres) {
            sphere.age += dt;
        }
        for (auto it = jet.spheres.begin(); it != jet.spheres.end();) {
            if (it->age >= it->flightSeconds) {
                if (landed) {
                    it->age = it->flightSeconds; // the ground, not below it
                    landed->push_back(it->at(gravity));
                }
                it = jet.spheres.erase(it);
            } else {
                ++it;
            }
        }
        if (jet.remaining <= 0.0f) {
            continue; // an expiring jet lets its last lumps fall, launches none
        }
        jet.sphereClock += dt;
        while (jet.sphereClock >= interval - 1e-4f) {
            jet.sphereClock -= interval;
            JetSphere sphere;
            sphere.origin = jet.origin;
            sphere.velocity = jet.velocity;
            sphere.age = jet.sphereClock; // launched mid-frame: already flying
            sphere.flightSeconds =
                jet.landing ? jet.landing->flightSeconds : 2.0f;
            const f32 volume = glm::max(jet.rate, 0.0f) * interval;
            sphere.radius = glm::clamp(
                std::cbrt(3.0f * volume / (4.0f * 3.1415927f)), 0.15f, 2.0f);
            jet.spheres.push_back(sphere);
        }
    }
}

void SpiritJetList::appendWaterSources(
    vector<render::terraingen::WaterSource>& out) const {
    for (const SpiritJet& jet : list) {
        if (jet.kind != render::terrain::SpiritKind::Water || !jet.landing ||
            jet.rate <= 0.0f || jet.remaining <= 0.0f) {
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
