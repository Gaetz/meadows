#pragma once

#include <optional>

#include "data/plugins/Record.hpp"
#include "engine/ecs/World.hpp"

// Save-game runtime plumbing (chantier 5) — the world/scene half of the
// save layer: reference diffs and (B4+) the pending in-memory layer +
// save files. The gameplay half (actor state capture/apply) lives in
// gameplay/save/SaveState. A save stays an ORDINARY plugin (§5).

namespace data {
class FormDatabase;
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

} // namespace game
