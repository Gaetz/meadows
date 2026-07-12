#pragma once

#include <functional>
#include <unordered_map>

#include "engine/anim/Anim.hpp"          // anim::Skeleton
#include "engine/assets/GltfMesh.hpp"    // assets::GltfClip
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"          // ecs::Entity

namespace rhi {
class Device;
}

namespace game {

struct Npc;
struct NpcContext;

// One rig cache entry per glTF asset (skeleton + its clips). Shared by every
// NPC built from that asset.
struct RigData {
    anim::Skeleton skeleton;
    vector<assets::GltfClip> clips;
};

// R4: the Forms->NPC build path, split out of NpcDirector. Owns the rig
// cache; builds newcomers (and frees the per-NPC GPU state). It FILLS the
// director's lists — npcs / entity map / patrol points / character spot —
// passed by reference: the director keeps owning them.
class NpcSpawner {
public:
    // Cell streaming makes NPC entities come and go: prune dead ones (freeing
    // their GPU state) and build newcomers. finalizeActorSpawn adds the stats /
    // saved-state / loadout components (shared with the player — stays in the
    // scene), applied deferred (a table move on the locked iteration).
    void refreshNpcs(
        rhi::Device& device, const NpcContext& ctx,
        const std::function<void(ecs::Entity, const core::Guid&)>&
            finalizeActorSpawn,
        vector<uptr<Npc>>& npcs, std::unordered_map<u64, Npc*>& npcByEntity,
        vector<Vec3>& patrolPoints, Vec3& characterSpot);

    void destroyNpc(rhi::Device& device, Npc& npc);

    // onExit teardown: drop the rig cache so a re-enter starts clean.
    void clearRigs() { rigCache.clear(); }

private:
    const RigData* loadRig(const NpcContext& ctx, const core::Guid& asset);

    std::unordered_map<core::Guid, RigData> rigCache;
};

} // namespace game
