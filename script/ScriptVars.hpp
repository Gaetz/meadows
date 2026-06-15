#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

namespace ecs {
class World;
}

namespace script {

// Persistent per-entity script state (§2.8): a map of named values, each a
// reflect::Value (the §5 serialization currency — NOT loose Lua tables). The
// `self.x` proxy reads/writes these. Runtime component; serialization is Phase 8
// (the map is a container — same deferred story as inventories/active effects).
struct ScriptVars {
    std::unordered_map<str, reflect::Value> vars;
};

void registerScriptComponents(ecs::World& world);

} // namespace script
