#include "engine/fx/Particles.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/Hash.hpp"

namespace fx {

namespace {

// hashU32 / HashRng live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

} // namespace

void ParticleSim::spawnOne(const EmitterParams& params, const Vec3& origin,
                           u32 seed) {
    if (count() >= budget) {
        return; // over budget: FX degrade, the frame never grows
    }
    HashRng rng { hashU32(seed ^ 0x9e3779b9u) };
    Particle particle;
    particle.position = origin;
    particle.velocity =
        params.velocity +
        Vec3 { rng.spread(), rng.spread(), rng.spread() } *
            params.velocityJitter;
    switch (params.shape) {
    case EmitterShape::Point:
        break;
    case EmitterShape::Sphere: {
        // A spread cube clamped to the ball — visually indistinguishable
        // at FX scales, cheaper than rejection sampling.
        const Vec3 offset { rng.spread(), rng.spread(), rng.spread() };
        const f32 lengthSq = glm::dot(offset, offset);
        particle.position +=
            (lengthSq > 1.0f ? offset / std::sqrt(lengthSq) : offset) *
            params.shapeRadius;
        break;
    }
    case EmitterShape::Cone: {
        // Fan the velocity around its own axis by up to shapeRadius
        // radians of half-angle (the spawn point stays at the origin).
        const f32 speed = glm::length(params.velocity);
        if (speed > 1e-4f) {
            const Vec3 axis = params.velocity / speed;
            const Vec3 helper = std::abs(axis.y) > 0.95f
                                    ? Vec3 { 1.0f, 0.0f, 0.0f }
                                    : Vec3 { 0.0f, 1.0f, 0.0f };
            const Vec3 side = glm::normalize(glm::cross(axis, helper));
            const f32 tilt = rng.next() * params.shapeRadius;
            const f32 spin = rng.next() * glm::two_pi<f32>();
            const Vec3 dir = glm::angleAxis(spin, axis) *
                             (glm::angleAxis(tilt, side) * axis);
            particle.velocity =
                dir * speed +
                Vec3 { rng.spread(), rng.spread(), rng.spread() } *
                    params.velocityJitter;
        }
        break;
    }
    case EmitterShape::Box:
        particle.position += Vec3 { rng.spread(), rng.spread(),
                                    rng.spread() } *
                             params.shapeRadius;
        break;
    }
    particle.gravity = params.gravity;
    particle.lifetime = glm::max(
        0.01f, params.lifetime + rng.spread() * params.lifetimeJitter);
    particle.sizeStart = params.sizeStart;
    particle.sizeEnd = params.sizeEnd;
    particle.colorStart = params.colorStart;
    particle.colorEnd = params.colorEnd;
    particle.additive = params.additive;
    particles.push_back(particle);
}

u32 ParticleSim::spawn(const EmitterParams& params, const Vec3& origin,
                       u32 seed) {
    for (i32 i = 0; i < params.burst; ++i) {
        spawnOne(params, origin, hashU32(seed + static_cast<u32>(i)));
    }
    if (params.rate <= 0.0f || params.duration <= 0.0f) {
        return 0; // the burst was the whole show
    }
    Emitter emitter;
    emitter.id = nextEmitterId++;
    emitter.params = params;
    emitter.origin = origin;
    emitter.remaining = params.duration;
    emitter.seed = seed;
    emitter.spawned = static_cast<u32>(glm::max(params.burst, 0));
    emitters.push_back(emitter);
    return emitter.id;
}

void ParticleSim::moveEmitter(u32 id, const Vec3& origin) {
    for (Emitter& emitter : emitters) {
        if (emitter.id == id) {
            emitter.origin = origin;
            return;
        }
    }
}

void ParticleSim::stopEmitter(u32 id) {
    emitters.erase(std::remove_if(emitters.begin(), emitters.end(),
                                  [&](const Emitter& emitter) {
                                      return emitter.id == id;
                                  }),
                   emitters.end());
}

void ParticleSim::update(f32 dt) {
    // Continuous emitters first: the streaming accumulator pattern —
    // rate x dt carried across frames so low rates still trickle.
    for (Emitter& emitter : emitters) {
        const f32 slice = glm::min(dt, glm::max(emitter.remaining, 0.0f));
        emitter.remaining -= dt;
        emitter.accumulator += emitter.params.rate * slice;
        while (emitter.accumulator >= 1.0f) {
            emitter.accumulator -= 1.0f;
            spawnOne(emitter.params, emitter.origin,
                     hashU32(emitter.seed + 0x51ed270bu +
                             emitter.spawned++));
        }
    }
    emitters.erase(std::remove_if(emitters.begin(), emitters.end(),
                                  [](const Emitter& emitter) {
                                      return emitter.remaining <= 0.0f;
                                  }),
                   emitters.end());

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

void ParticleSim::clear() {
    particles.clear();
    emitters.clear();
}

} // namespace fx
