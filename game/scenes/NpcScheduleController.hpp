#pragma once

#include "engine/core/Defines.hpp"

namespace game {

struct Npc;
struct NpcContext;

// R4: the peaceful-life half of the NPC frame, split out of NpcDirector —
// schedule evaluation, package execution (travel / wander / useFurniture /
// guard...), the furniture claim/seat/effect logic (D1), and the legacy
// patrol fallback.
class NpcScheduleController {
public:
    // --- Schedule-driven day (B3) --- re-evaluates the schedule, then
    // executes the active package. `idleDecay` is the director's per-NPC
    // speed-decay rate (wander softens it).
    void update(f32 dt, const NpcContext& ctx, Npc& npc, f32 hourOfDay,
                f32& idleDecay);

    // --- Legacy patrol fallback (chantier 1 B6) --- when the ActorForm
    // carries no schedule and the cell has patrol markers.
    void patrol(f32 dt, const NpcContext& ctx, Npc& npc,
                const vector<Vec3>& patrolPoints);

    // D1: the ONE way out of furniture — frees the occupancy point,
    // removes the furniture's GAS effect, clears the anim gate. Public:
    // combat and death stand the NPC up too (director / combat side).
    void releaseFurniture(const NpcContext& ctx, Npc& npc);

private:
    void updateNpcSchedule(const NpcContext& ctx, Npc& npc, f32 hourOfDay);
};

} // namespace game
