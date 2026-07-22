#pragma once

#include "engine/core/Defines.hpp"

namespace game {

struct Npc;
struct NpcContext;

// The peaceful-life half of the NPC frame, split out of NpcDirector —
// schedule evaluation, package execution (travel / wander / useFurniture /
// guard...), the furniture claim/seat/effect logic (D1), and the legacy
// patrol fallback.
class NpcScheduleController {
public:
    // --- Schedule-driven day --- re-evaluates the schedule, then
    // executes the active package. `idleDecay` is the director's per-NPC
    // speed-decay rate (wander softens it).
    void update(f32 dt, const NpcContext& ctx, Npc& npc, f32 hourOfDay,
                f32& idleDecay);

    // --- Legacy patrol fallback --- when the ActorForm
    // carries no schedule and the cell has patrol markers.
    void patrol(f32 dt, const NpcContext& ctx, Npc& npc,
                const vector<Vec3>& patrolPoints);

    // --- The `follow` package (docs/FOLLOWERS.md) --- an ACTIVE follower
    // overrides its schedule with this (NpcDirector dispatches it; combat
    // still wins). Reuses the goTo/moveNpcAlongPath idiom driven by the
    // pure decideFollow intent: repath toward the player every
    // followRepathSeconds, hurry beyond the catchup radius, face the
    // player when idle-near, teleport-near when lost.
    void followPlayer(f32 dt, const NpcContext& ctx, Npc& npc);

    // D1: the ONE way out of furniture — frees the occupancy point,
    // removes the furniture's GAS effect, clears the anim gate. Public:
    // combat and death stand the NPC up too (director / combat side).
    void releaseFurniture(const NpcContext& ctx, Npc& npc);

    // Interruption over (combat/dialogue cleared — the director's
    // updateInterruption falling edge): re-evaluate the schedule NOW
    // (the slot may have changed mid-fight) and repath from wherever the
    // override left the NPC.
    void resume(Npc& npc);

private:
    void updateNpcSchedule(const NpcContext& ctx, Npc& npc, f32 hourOfDay);
};

} // namespace game
