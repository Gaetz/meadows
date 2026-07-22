#pragma once

// Subsystem map: docs/AUDIT/U1-foundations.md

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// CPU particle simulation (docs/HORIZONTAL-PASS.md):
// pure, headless, deterministic per seed — the FX seam's compute half.
// Rendering is the caller's business: 2D scenes draw each live particle
// as a sprite (painter order); the 3D landscape draws camera-facing
// quads (FxRenderer) from the extract's POD copies. EmitterParams
// mirrors ParticleForm; the runtime layer maps one onto the other (the
// engine never sees data::).
//
// Features: continuous emitters (rate over duration, the streaming
// accumulator pattern), spawn shapes (sphere/cone/box), a global budget
// (spawns beyond it are DROPPED — FX degrade, never grow the frame),
// and the per-particle blend flag the renderer batches by. A GPU path
// (compute) waits for counts that demand it.

namespace fx {

enum class EmitterShape : u8 {
    Point,  // all particles at the origin
    Sphere, // uniform inside a shapeRadius ball
    Cone,   // spawn at origin, velocity fanned around `velocity` by
            // shapeRadius RADIANS of half-angle
    Box     // uniform inside a shapeRadius half-extent cube
};

struct EmitterParams {
    EmitterShape shape { EmitterShape::Point };
    f32 shapeRadius { 0.1f }; // meters (Cone: half-angle in radians)
    i32 burst { 12 };         // particles on spawn
    f32 rate { 0.0f };        // particles/second while the emitter lives
    f32 duration { 0.0f };    // emitter seconds; 0 = the burst only
    f32 lifetime { 1.0f };
    f32 lifetimeJitter { 0.2f };
    Vec3 velocity { 0.0f, 1.0f, 0.0f };
    f32 velocityJitter { 0.5f };
    Vec3 gravity { 0.0f, -3.0f, 0.0f };
    f32 sizeStart { 0.2f };
    f32 sizeEnd { 0.05f };
    Vec4 colorStart { 1.0f, 1.0f, 1.0f, 1.0f };
    Vec4 colorEnd { 1.0f, 1.0f, 1.0f, 0.0f };
    bool additive { false }; // ParticleForm.blend — the render batch key
};

struct Particle {
    Vec3 position {};
    Vec3 velocity {};
    Vec3 gravity {};
    f32 age { 0.0f };
    f32 lifetime { 1.0f };
    f32 sizeStart { 0.1f };
    f32 sizeEnd { 0.0f };
    Vec4 colorStart { 1.0f };
    Vec4 colorEnd { 1.0f };
    bool additive { false };
};

class ParticleSim {
public:
    // Spawns `params.burst` particles at `origin` and, when the params
    // carry a rate + duration, registers a continuous emitter that keeps
    // spawning until its duration runs out (or stopEmitter). Returns the
    // emitter id (0 = burst-only, nothing to steer). Same seed =
    // identical stream (cosmetic seeds may come from position hashes;
    // anything gameplay-relevant must route core::Rng, §8 — particles
    // never are).
    u32 spawn(const EmitterParams& params, const Vec3& origin, u32 seed);

    // Follows a moving source (a torch bearer); unknown ids are ignored.
    void moveEmitter(u32 id, const Vec3& origin);
    // Ends the emission early; live particles drain naturally.
    void stopEmitter(u32 id);

    void update(f32 dt);
    void clear();

    u32 count() const { return static_cast<u32>(particles.size()); }
    u32 emitterCount() const { return static_cast<u32>(emitters.size()); }

    // The global budget: spawns beyond it are dropped (default 4096).
    void setBudget(u32 particles) { budget = particles; }
    u32 budgetLeft() const {
        return budget > count() ? budget - count() : 0;
    }

    // Visits every live particle with its CURRENT derived state.
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (const Particle& p : particles) {
            const f32 t = glm::clamp(p.age / p.lifetime, 0.0f, 1.0f);
            fn(p.position, glm::mix(p.sizeStart, p.sizeEnd, t),
               glm::mix(p.colorStart, p.colorEnd, t), p.additive);
        }
    }

private:
    struct Emitter {
        u32 id { 0 };
        EmitterParams params;
        Vec3 origin { 0.0f };
        f32 remaining { 0.0f };   // emitter seconds left
        f32 accumulator { 0.0f }; // fractional spawns carried over
        u32 seed { 0 };
        u32 spawned { 0 }; // feeds per-particle seeds (deterministic)
    };

    void spawnOne(const EmitterParams& params, const Vec3& origin,
                  u32 seed);

    vector<Particle> particles;
    vector<Emitter> emitters;
    u32 nextEmitterId { 1 };
    u32 budget { 4096 };
};

} // namespace fx
