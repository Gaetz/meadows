#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// Chantier ESPRITS: the transient WATER BODIES of a gesture, as geometry
// — a jet's arc as a tube, a carried volume as a blob. World-space
// triangle soups (3 vertices per triangle) rebuilt every frame by the
// scene and drawn with the placed-volume water look
// (RenderSnapshot::WaterMeshInstance). Headless: pure geometry.

namespace world {

// The cross-section radius of a stream carrying `intensity` m³/s at
// `speed` m/s (area = flow / speed), clamped to what reads on screen.
f32 jetRadius(f32 intensity, f32 speed);

// The radius of a sphere holding `volume` m³, clamped to what reads.
f32 blobRadius(f32 volume);

// A tube of `sides` facets along the ballistic arc from `origin` at
// `velocity` for `seconds`, `segments` rings, `radius` metres, widening
// by `flare` toward its end (the stream breaks up as it falls).
void appendJetTube(vector<Vec3>& out, const Vec3& origin,
                   const Vec3& velocity, f32 gravity, f32 seconds,
                   f32 radius, u32 segments = 20, u32 sides = 6,
                   f32 flare = 1.4f);

// A UV sphere (`rings` x `sectors`) of `radius` at `center`, squashed
// vertically by `squash` (1 = round).
void appendBlob(vector<Vec3>& out, const Vec3& center, f32 radius,
                u32 rings = 7, u32 sectors = 12, f32 squash = 0.85f);

} // namespace world
