#pragma once

#include "engine/core/Defines.hpp"

namespace game {

struct Npc;
struct NpcContext;

// Locomotion helpers shared by combat (strafe / flee / approach) and
// the schedule walker (travel) — they read only ctx and npc, so they live
// as free functions outside any controller.

// Walks npc.path from npc.pathIndex; returns true when the path is done.
bool moveNpcAlongPath(const NpcContext& ctx, Npc& npc, f32 dt,
                      f32 speedScale);

// Pathless steering (strafe orbits, flee) — same stat-driven
// speed as the path walker, explicit facing (a strafer keeps its
// eyes on the target, a runner looks where it runs).
void moveNpcDirect(const NpcContext& ctx, Npc& npc, f32 dt,
                   const Vec3& direction, f32 speedScale, f32 faceYaw);

// Is a pathless steering step about to walk into a wall? Direct
// moves (strafe, cornered flee) bypass the navigator, so probe the way
// with a chest-height ray — buildings are static colliders, the ray
// sees them (actors are NOT in the broadphase; other NPCs don't block).
bool steerBlocked(const NpcContext& ctx, const Vec3& from,
                  const Vec3& direction);

} // namespace game
