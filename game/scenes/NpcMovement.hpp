#pragma once

#include "engine/core/Defines.hpp"

namespace render {
struct TerrainParams;
}
namespace phys {
class PhysicsWorld;
}

namespace game {

struct Npc;
struct NpcContext;

// Locomotion helpers shared by combat (strafe / flee / approach) and
// the schedule walker (travel) — they read only ctx and npc, so they live
// as free functions outside any controller.

// Grounds a position after a horizontal step: terrain height outdoors, a
// downward probe of the collision mesh in an interior (the terrain field
// would teleport the actor out of the dungeon — terrain::height must stay
// behind interiorMode, docs/DUNGEON-GEN.md). Returns false when the
// interior probe finds no floor underfoot — a ledge over a tall room's
// air; the movers treat that as a wall and refuse the step (a kinematic
// NPC neither falls nor walks on air). groundAt is the raw form for
// callers without an NpcContext (FollowerController::teleportNear).
bool groundAt(const render::TerrainParams& terrain, bool interiorMode,
              phys::PhysicsWorld* physics, Vec3& position);
bool groundNpc(const NpcContext& ctx, Vec3& position);

// Wraps an angle into [-pi, pi].
f32 wrapAngle(f32 angle);
// Exponentially eases `yaw` toward `goalYaw` (`rate` in 1/s), wrap-aware —
// the shared facing smoother of the walkers, guards and the mount.
void smoothYawToward(f32& yaw, f32 goalYaw, f32 rate, f32 dt);

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
