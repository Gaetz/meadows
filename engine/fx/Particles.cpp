#include "engine/fx/Particles.hpp"

#include <algorithm>

namespace fx {

namespace {

// Same hash family as the landscape scatter: cheap, deterministic.
u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

struct HashRng {
    u32 state;
    f32 next() { // [0, 1)
        state = hashU32(state);
        return static_cast<f32>(state) * (1.0f / 4294967296.0f);
    }
    f32 spread() { return next() * 2.0f - 1.0f; } // [-1, 1)
};

} // namespace

void ParticleSim::spawnBurst(const EmitterParams& params, const Vec3& origin,
                             u32 seed) {
    HashRng rng { hashU32(seed ^ 0x9e3779b9u) };
    for (i32 i = 0; i < params.burst; ++i) {
        Particle particle;
        particle.position = origin;
        particle.velocity =
            params.velocity +
            Vec3 { rng.spread(), rng.spread(), rng.spread() } *
                params.velocityJitter;
        particle.gravity = params.gravity;
        particle.lifetime = glm::max(
            0.01f, params.lifetime + rng.spread() * params.lifetimeJitter);
        particle.sizeStart = params.sizeStart;
        particle.sizeEnd = params.sizeEnd;
        particle.colorStart = params.colorStart;
        particle.colorEnd = params.colorEnd;
        particles.push_back(particle);
    }
}

void ParticleSim::update(f32 dt) {
    for (Particle& p : particles) {
        p.age += dt;
        p.velocity += p.gravity * dt;
        p.position += p.velocity * dt;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(),
                                   [](const Particle& p) {
                                       return p.age >= p.lifetime;
                                   }),
                    particles.end());
}

} // namespace fx
