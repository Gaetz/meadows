#pragma once

// Subsystem map: docs/AUDIT/U1-foundations.md

#include <glm/glm.hpp>

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

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
    Disc,   // uniform on the horizontal disc of shapeRadius (ground fires)
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
    // ParticleForm.blend = "flame": drawn by the flame pipeline (an
    // upright tongue shaped by noise, colorStart = core, colorEnd =
    // outer band, size = height) instead of a round sprite.
    bool flame { false };
    // How much the wind carries the particle: 0 = none, 1 = its
    // horizontal velocity relaxes to the wind's in ~1 s (ParticleForm.windDrag).
    f32 windDrag { 0.0f };
    // Flames: the flipbook sheet (null = the procedural tongue).
    core::Guid texture;
    i32 flipbookColumns { 1 };
    i32 flipbookRows { 1 };
    f32 flipbookFps { 30.0f };
    f32 flipbookAspect { 1.0f };
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
    bool flame { false };
    f32 seed { 0.0f }; // 0..1, per particle (cosmetic variety)
    f32 windDrag { 0.0f };
    core::Guid texture; // flames: the flipbook sheet
    i32 flipbookColumns { 1 };
    i32 flipbookRows { 1 };
    f32 flipbookFps { 30.0f };
    f32 flipbookAspect { 1.0f };
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
    // Re-aims a stream (a spirit jet following the caster): the initial
    // velocity and the per-particle lifetime of everything spawned from
    // now on; live particles keep flying. Unknown ids are ignored.
    void steerEmitter(u32 id, const Vec3& velocity, f32 lifetime);
    // Ends the emission early; live particles drain naturally.
    void stopEmitter(u32 id);

    void update(f32 dt);
    // The wind every particle with windDrag drifts with (m/s, world):
    // uniform, or sampled per particle when a sampler is set (the wind
    // field's gusts).
    void setWind(const Vec3& velocity) { wind = velocity; }
    using WindSampler = std::function<Vec3(const Vec3& position)>;
    void setWindSampler(WindSampler sampler) { windSampler = std::move(sampler); }
    void clear();

    u32 count() const { return static_cast<u32>(particles.size()); }
    u32 emitterCount() const { return static_cast<u32>(emitters.size()); }

    // The global budget: spawns beyond it are dropped (default 4096).
    void setBudget(u32 maxParticles) { budget = maxParticles; }
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
    // The raw particle plus its life fraction — the extract needs the
    // flame kind, both colours and the seed.
    template<typename Fn>
    void forEachRaw(Fn&& fn) const {
        for (const Particle& p : particles) {
            fn(p, glm::clamp(p.age / p.lifetime, 0.0f, 1.0f));
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
    Vec3 wind { 0.0f };
    WindSampler windSampler;

    vector<Particle> particles;
    vector<Emitter> emitters;
    u32 nextEmitterId { 1 };
    u32 budget { 4096 };
};

} // namespace fx
