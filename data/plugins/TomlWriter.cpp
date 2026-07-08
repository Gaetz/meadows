#include "data/plugins/TomlWriter.hpp"

#include <algorithm>
#include <sstream>

#include <toml++/toml.hpp>

#include "engine/core/Log.hpp"
#include "engine/reflect/Visit.hpp"

namespace data {

namespace {

toml::array floatArray(std::initializer_list<f32> values) {
    toml::array array;
    for (const f32 v : values) {
        array.push_back(static_cast<double>(v));
    }
    return array;
}

void insertValue(toml::table& fields, std::string_view name,
                 const reflect::Value& value) {
    // Exhaustive per kind (see engine/reflect/Visit.hpp). Vec/Quat file order
    // [x, y, z, w], matching the loader.
    reflect::visit(value, reflect::overloaded {
        [&](bool b)              { fields.insert(name, b); },
        [&](i32 x)               { fields.insert(name, static_cast<i64>(x)); },
        [&](u32 x)               { fields.insert(name, static_cast<i64>(x)); },
        [&](f32 x)               { fields.insert(name, static_cast<double>(x)); },
        [&](f64 x)               { fields.insert(name, x); },
        [&](const str& s)        { fields.insert(name, s); },
        [&](const Vec2& v)       { fields.insert(name, floatArray({ v.x, v.y })); },
        [&](const Vec3& v)       { fields.insert(name, floatArray({ v.x, v.y, v.z })); },
        [&](const Vec4& v)       { fields.insert(name, floatArray({ v.x, v.y, v.z, v.w })); },
        [&](const Quat& q)       { fields.insert(name, floatArray({ q.x, q.y, q.z, q.w })); },
        [&](const core::Guid& g) { fields.insert(name, g.toString()); },
    });
}

} // namespace

str writePluginToml(const Plugin& plugin, const FormTypeRegistry& types) {
    toml::table root;

    toml::table header;
    header.insert("id", plugin.id.toString());
    header.insert("name", plugin.name);
    if (!plugin.dependencies.empty()) {
        toml::array deps;
        for (const core::Guid& dep : plugin.dependencies) {
            deps.push_back(dep.toString());
        }
        header.insert("dependencies", std::move(deps));
    }
    root.insert("plugin", std::move(header));

    if (!plugin.assets.empty()) {
        // Sorted by guid string for stable diffs.
        vector<std::pair<str, str>> sorted;
        sorted.reserve(plugin.assets.size());
        for (const AssetEntry& asset : plugin.assets) {
            sorted.emplace_back(asset.id.toString(), asset.path);
        }
        std::sort(sorted.begin(), sorted.end());
        toml::table assets;
        for (const auto& [guid, path] : sorted) {
            assets.insert(guid, path);
        }
        root.insert("assets", std::move(assets));
    }

    toml::array records;
    for (const Record& record : plugin.records) {
        const reflect::TypeInfo* type = types.findType(record.typeId);
        if (!type) {
            LOG_WARN("TomlWriter: skipped record {} with unknown type id "
                     "{:#x}",
                     record.formId.toString(), record.typeId);
            continue;
        }

        toml::table recordTable;
        recordTable.insert("form", record.formId.toString());
        recordTable.insert("type", type->name);
        if (record.creates) {
            recordTable.insert("new", true);
        }

        // Resolve names first, then sort by name for stable diffs.
        vector<std::pair<str, const reflect::Value*>> named;
        named.reserve(record.fields.size());
        for (const auto& [fieldId, value] : record.fields) {
            const reflect::FieldInfo* field = type->findField(fieldId);
            if (!field) {
                LOG_WARN("TomlWriter: record {}: skipped unknown field id "
                         "{:#x} on {}",
                         record.formId.toString(), fieldId, type->name);
                continue;
            }
            named.emplace_back(field->name, &value);
        }
        std::sort(named.begin(), named.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        if (!named.empty()) {
            toml::table fields;
            for (const auto& [name, value] : named) {
                insertValue(fields, name, *value);
            }
            recordTable.insert("fields", std::move(fields));
        }
        records.push_back(std::move(recordTable));
    }
    if (!records.empty()) {
        root.insert("records", std::move(records));
    }

    std::ostringstream out;
    out << root << "\n";
    return out.str();
}

} // namespace data
