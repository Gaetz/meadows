#pragma once

#include <filesystem>
#include <optional>
#include <unordered_map>

#include "data/plugins/Record.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/save/SaveState.hpp"

// Save-game runtime plumbing (chantier 5) — the world/scene half of the
// save layer: reference diffs, the PENDING in-memory layer (the memory of
// unloaded cells), and (B5) the save files. The gameplay half (actor
// state capture/apply) lives in gameplay/save/SaveState. A save stays an
// ORDINARY plugin (§5).

namespace data {
class FormDatabase;
class FormTypeRegistry;
}
namespace gameplay {
class GameplayTagRegistry;
}

namespace game {

// Diffs a spawned reference's live state against its resolved
// ReferenceForm and returns a field-level PATCH record (or nullopt when
// nothing changed). Diffed fields:
//  - `cell` — always (a null target = persistent: the future follower
//    contract; a different cell = re-homing);
//  - `position`/`rotation` — ACTORS only (their Y is re-derived from the
//    terrain at build; item/static positions never change at runtime, and
//    capturing snapped world Y would double the ground offset on reload).
std::optional<data::Record> captureReference(ecs::Entity entity,
                                             const data::FormDatabase& forms);

// The pending save layer: the RUNTIME memory of what changed, per
// reference, captured when a cell unloads (CellLoader::beforeUnload) or
// immediately (item pickups). Looted crates stay looted when their cell
// reloads — no disk involved; a disk save (B5) just flushes this plus a
// capture of the still-loaded cells into one ordinary plugin.
class PendingSaveLayer {
public:
    // Captures every (InCell cellEntity) entity's deltas; replaces any
    // previous capture of the same references.
    void captureCell(ecs::World& world, const data::FormDatabase& forms,
                     ecs::Entity cellEntity,
                     const gameplay::GameplayTagRegistry& tags);
    // Captures one entity immediately (the player at save time, an NPC
    // about to despawn).
    void captureEntity(ecs::Entity entity, const data::FormDatabase& forms,
                       const gameplay::GameplayTagRegistry& tags);

    // Marks a reference disabled (picked-up item): its spawn is vetoed
    // (CellLoader::spawnFilter + prefab expansion) and the flush emits
    // enabled = false. For a prefab-derived child (no record in any
    // plugin — a patch would be an orphan), pass the still-alive entity:
    // the layer materializes a full disabled `creates` record instead.
    void disableReference(const core::Guid& referenceId,
                          const data::FormDatabase& forms,
                          ecs::Entity entity = {});
    bool isEnabled(const core::Guid& referenceId) const;

    // Applies any captured instance-field overrides (position/rotation) for
    // this reference onto a freshly (re)spawned entity — the within-session
    // equivalent of re-resolving the reference patch over the authored record.
    // Without this a moved/killed actor reloads at its AUTHORED spawn point
    // (the cell loader spawns the resolved record; only the enabled veto and
    // the actor stats are applied elsewhere). No capture for the reference =>
    // no change. Actors only carry Transform patches today (see captureReference).
    void applyReferenceOverrides(ecs::Entity entity,
                                 const core::Guid& referenceId) const;

    // Materialized saved-actor records for finalizeActorSpawn — valid
    // until the next capture of the same reference. Null stats = this
    // layer holds nothing for that actor.
    bool hasActorState(const core::Guid& referenceId) const;
    gameplay::SavedActorRecords actorState(const core::Guid& referenceId);

    // Every record of the layer (reference patches + actor children),
    // deterministically ordered (§8) — the flush the disk save appends.
    vector<data::Record> flush() const;

    void clear();
    u32 trackedCount() const { return static_cast<u32>(entries.size()); }

private:
    struct Entry {
        std::optional<data::Record> referencePatch;
        vector<data::Record> actorRecords; // SavedX children (raw records)
        bool disabled { false };
        // Materialized views (actorState) — rebuilt on demand.
        gameplay::SavedStatsForm stats;
        vector<gameplay::SavedEffectForm> effects;
        vector<gameplay::SavedItemForm> items;
        vector<gameplay::SavedInjuryForm> injuries;
        bool materialized { false };
    };
    Entry& entryFor(const core::Guid& referenceId);

    std::unordered_map<core::Guid, Entry> entries;
};

// --- Save files (B5). A slot = saves/<name>.toml next to the exe — a
// save file IS a plugin file (writePluginToml/parsePluginToml); the
// binary cooked path is a future option (the cooker already knows the
// save form types).
std::filesystem::path savesDirectory();
std::filesystem::path savePath(const str& slot);
struct SaveSlotInfo {
    str name;
    str timestamp; // "YYYY-MM-DD HH:MM" local time, for the list screen
};
vector<SaveSlotInfo> listSaveSlots(); // newest first
bool writeSave(const str& slot, const data::Plugin& plugin,
               const data::FormTypeRegistry& types);
std::optional<data::Plugin> readSave(const str& slot,
                                     const data::FormTypeRegistry& types);

} // namespace game
