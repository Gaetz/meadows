#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

namespace ecs {
class World;
}

namespace gameplay {

// Persistent per-entity script state (§2.8): a map of named values, each a
// reflect::Value (the §5 serialization currency — NOT loose Lua tables). The
// script layer's `self.x` proxy reads/writes these; the struct itself is
// Lua-free so the save layer can persist it without a script dependency.
//
// Persistence: one SavedScriptVarForm child record per entry (SaveState
// captureActor / applySavedState), so a script variable serializes and
// diffs through the §5 patch layer like every other saved row. The row
// carries the three kinds the Lua bridge stores — f32 number, str text,
// bool flag; a value of another kind is not script-authored state and is
// not persisted.
struct ScriptVars {
    std::unordered_map<str, reflect::Value> vars;
};

// Registers the component in flecs (runtime storage only — the map is not
// flat-reflectable; persistence is the child-record path above).
void registerScriptComponents(ecs::World& world);

} // namespace gameplay
