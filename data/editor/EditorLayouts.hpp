#pragma once

#include <map>
#include <optional>

#include <glm/glm.hpp> // Defines.hpp only forward-declares Vec2

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

// The node-position side-store of the graph editors (chantier 8.6).
// Decided with the dev (2026-07-09): node x/y layout is TOOL state, never
// Form fields — moving a node must not pollute a mod's diff. This is the
// one assumed exception to "everything is a plugin": the file (default
// data/editor-layouts.toml) is never loaded by the game runtime and never
// listed in plugins.toml — it is imgui.ini's sibling.
//
// Keys are form guids: the graph form's guid, then one entry per node
// guid. Sorted maps + sorted writes keep the file git-diff friendly.

namespace data {

class EditorLayouts {
public:
    // Missing file is fine (empty store, first run). False = parse error.
    bool load(const str& path);
    // Writes back to the path given to load(). No-op without one.
    bool save() const;

    std::optional<Vec2> positionOf(const core::Guid& graph,
                                   const core::Guid& node) const;
    void setPosition(const core::Guid& graph, const core::Guid& node,
                     const Vec2& position);

    // Serialization split out for headless round-trip tests.
    str writeToml() const;
    bool parseToml(const str& text);

    u32 graphCount() const { return static_cast<u32>(graphs.size()); }

private:
    str path;
    std::map<core::Guid, std::map<core::Guid, Vec2>> graphs;
};

} // namespace data
