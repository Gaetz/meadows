#include "engine/fx/Particles.hpp"

#include <algorithm>

#include "engine/core/Hash.hpp"

namespace fx {

namespace {

// hashU32 / HashRng now live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

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
