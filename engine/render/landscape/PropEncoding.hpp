#pragma once

#include <algorithm>

#include "engine/core/Defines.hpp"

// The prop FLAG PROTOCOL shared by the tree generators, the scatter and
// the vegetation shaders. shaders/prop_encoding.glsl mirrors this file
// constant for constant — change both together. Flags ride lanes real
// data never drives negative, so no extra attribute is needed:
//
// Per VERTEX (MeshVertex::uv):
//   wood / lobe        uv.x = sway weight in [0,1], uv.y = height01
//   bark (procedural)  uv.y = kBarkUvY (below kBarkThreshold): tree.frag
//                      samples the slot's bark triplanarly, no mesh uv.
//   leaf card          uv.x = cardSlotBias(slot) + cornerX * halfSize,
//                      uv.y = cornerY * halfSize (a degenerate quad the
//                      vertex shader expands; the bias is below
//                      kCardThreshold and steps kCardSlotStep per
//                      leaf-mask atlas slot).
//   textured prop      uv = real texture coordinates; the INSTANCE
//                      carries the flag (next block).
// Per INSTANCE (VegetationSystem::Instance::params):
//   z = sway phase; rigidPhase(phase) < 0 = never sways (rocks, scans).
//   w = fade end (m); texturedFade(fadeEnd) < 0 = textured prop
//       (|w| stays the fade end).
namespace render::propenc {

inline constexpr f32 kCardFlagBias = -10.0f;
inline constexpr f32 kCardSlotStep = -20.0f;
inline constexpr f32 kCardThreshold = -5.0f;
inline constexpr i32 kCardSlotCount = 8; // leaf-mask atlas tiles
inline constexpr f32 kBarkUvY = -1.0f;
inline constexpr f32 kBarkThreshold = -0.5f;

// uv.x origin of a leaf card in atlas slot `slot` (clamped to the atlas).
constexpr f32 cardSlotBias(i32 slot) {
    return kCardFlagBias +
           kCardSlotStep *
               static_cast<f32>(std::clamp(slot, 0, kCardSlotCount - 1));
}

// Instance sway-phase lane of a rigid prop.
constexpr f32 rigidPhase(f32 phase) { return -(phase + 1.0f); }

// Instance fade lane of a textured prop.
constexpr f32 texturedFade(f32 fadeEnd) { return -fadeEnd; }

} // namespace render::propenc
