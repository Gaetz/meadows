#include "data/editor/EditorLayouts.hpp"

#include <fstream>
#include <sstream>

#include <glm/glm.hpp> // Defines.hpp only forward-declares Vec2
#include <toml++/toml.hpp>

#include "engine/core/Log.hpp"

namespace data {

bool EditorLayouts::load(const str& filePath) {
    path = filePath;
    graphs.clear();
    std::ifstream in(filePath, std::ios::binary);
    if (!in) {
        return true; // no file yet — first run, empty store
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parseToml(buffer.str());
}

bool EditorLayouts::save() const {
    if (path.empty()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        LOG_WARN("EditorLayouts: cannot write '{}'", path);
        return false;
    }
    out << writeToml();
    return true;
}

std::optional<Vec2> EditorLayouts::positionOf(const core::Guid& graph,
                                              const core::Guid& node) const {
    const auto graphIt = graphs.find(graph);
    if (graphIt == graphs.end()) {
        return std::nullopt;
    }
    const auto nodeIt = graphIt->second.find(node);
    if (nodeIt == graphIt->second.end()) {
        return std::nullopt;
    }
    return nodeIt->second;
}

void EditorLayouts::setPosition(const core::Guid& graph,
                                const core::Guid& node,
                                const Vec2& position) {
    graphs[graph][node] = position;
}

str EditorLayouts::writeToml() const {
    // Sorted maps + toml++'s sorted tables => byte-stable output.
    toml::table graphsTable;
    for (const auto& [graph, nodes] : graphs) {
        toml::table nodeTable;
        for (const auto& [node, position] : nodes) {
            toml::array xy;
            xy.push_back(static_cast<double>(position.x));
            xy.push_back(static_cast<double>(position.y));
            nodeTable.insert(node.toString(), std::move(xy));
        }
        graphsTable.insert(graph.toString(), std::move(nodeTable));
    }
    toml::table root;
    root.insert("graphs", std::move(graphsTable));

    std::ostringstream out;
    out << "# Node positions of the graph editors.\n"
           "# TOOL state only: never loaded by the game, not a plugin.\n\n";
    out << root << "\n";
    return out.str();
}

bool EditorLayouts::parseToml(const str& text) {
    graphs.clear();
    const toml::parse_result result = toml::parse(text);
    if (!result) {
        LOG_WARN("EditorLayouts: parse error in '{}': {}", path,
                 str(result.error().description()));
        return false;
    }
    const toml::table* graphsTable = result.table()["graphs"].as_table();
    if (!graphsTable) {
        return true; // empty store
    }
    for (const auto& [graphKey, graphNode] : *graphsTable) {
        const auto graphId = core::Guid::fromString(graphKey.str());
        const toml::table* nodes = graphNode.as_table();
        if (!graphId || !nodes) {
            continue; // tool state: tolerate junk, never fail hard
        }
        auto& entry = graphs[*graphId];
        for (const auto& [nodeKey, value] : *nodes) {
            const auto nodeId = core::Guid::fromString(nodeKey.str());
            const toml::array* xy = value.as_array();
            if (!nodeId || !xy || xy->size() < 2) {
                continue;
            }
            entry[*nodeId] =
                Vec2 { static_cast<f32>(xy->get(0)->value_or(0.0)),
                       static_cast<f32>(xy->get(1)->value_or(0.0)) };
        }
    }
    return true;
}

} // namespace data
