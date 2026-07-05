#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// CPU particle simulation (horizontal pass H7): pure, headless,
// deterministic per seed — the FX seam's compute half. Rendering is the
// caller's business: 2D scenes draw each live particle as a sprite
// (painter order), the 3D landscape will draw camera-facing quads.
// EmitterParams mirrors ParticleForm; the runtime layer maps one onto the
// other (rule n°2: engine never sees data::).
//
// HOW TO FILL (post-7/07, FX vertical):
//  - continuous emitters (rate over duration) on top of bursts: keep the
//    accumulator pattern of the streaming systems;
//  - shapes (sphere/cone/box) in spawnBurst's position/velocity rolls;
//  - soft particles / additive blending / textures at the render site
//    (ParticleForm already carries texture + blend);
//  - GPU path (compute) only when counts demand it — the RHI extension
//    exists, but CPU covers a Skyrim-like comfortably.

namespace fx {

struct EmitterParams {
    i32 burst { 12 };
    f32 lifetime { 1.0f };
    f32 lifetimeJitter { 0.2f };
    Vec3 velocity { 0.0f, 1.0f, 0.0f };
    f32 velocityJitter { 0.5f };
    Vec3 gravity { 0.0f, -3.0f, 0.0f };
    f32 sizeStart { 0.2f };
    f32 sizeEnd { 0.05f };
    Vec4 colorStart { 1.0f, 1.0f, 1.0f, 1.0f };
    Vec4 colorEnd { 1.0f, 1.0f, 1.0f, 0.0f };
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
};

class ParticleSim {
public:
    // Spawns `params.burst` particles at `origin`. Same seed = identical
    // burst (cosmetic seeds may come from position hashes; anything
    // gameplay-relevant must route core::Rng, §8 — particles never are).
    void spawnBurst(const EmitterParams& params, const Vec3& origin,
                    u32 seed);

    void update(f32 dt);
    void clear() { particles.clear(); }

    u32 count() const { return static_cast<u32>(particles.size()); }

    // Visits every live particle with its CURRENT derived state.
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (const Particle& p : particles) {
            const f32 t = glm::clamp(p.age / p.lifetime, 0.0f, 1.0f);
            fn(p.position, glm::mix(p.sizeStart, p.sizeEnd, t),
               glm::mix(p.colorStart, p.colorEnd, t));
        }
    }

private:
    vector<Particle> particles;
};

} // namespace fx
