#include "data/plugins/TomlWriter.hpp"

#include <algorithm>
#include <sstream>

#include <toml++/toml.hpp>

#include "engine/core/Log.hpp"

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
    using reflect::FieldKind;
    switch (reflect::valueKind(value)) {
    case FieldKind::Bool:
        fields.insert(name, std::get<bool>(value));
        break;
    case FieldKind::I32:
        fields.insert(name, static_cast<i64>(std::get<i32>(value)));
        break;
    case FieldKind::U32:
        fields.insert(name, static_cast<i64>(std::get<u32>(value)));
        break;
    case FieldKind::F32:
        fields.insert(name, static_cast<double>(std::get<f32>(value)));
        break;
    case FieldKind::F64:
        fields.insert(name, std::get<f64>(value));
        break;
    case FieldKind::Str:
        fields.insert(name, std::get<str>(value));
        break;
    case FieldKind::Vec2: {
        const Vec2& v = std::get<Vec2>(value);
        fields.insert(name, floatArray({ v.x, v.y }));
        break;
    }
    case FieldKind::Vec3: {
        const Vec3& v = std::get<Vec3>(value);
        fields.insert(name, floatArray({ v.x, v.y, v.z }));
        break;
    }
    case FieldKind::Vec4: {
        const Vec4& v = std::get<Vec4>(value);
        fields.insert(name, floatArray({ v.x, v.y, v.z, v.w }));
        break;
    }
    case FieldKind::Quat: {
        // File order [x, y, z, w], matching the loader.
        const Quat& q = std::get<Quat>(value);
        fields.insert(name, floatArray({ q.x, q.y, q.z, q.w }));
        break;
    }
    case FieldKind::Guid:
        fields.insert(name, std::get<core::Guid>(value).toString());
        break;
    }
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
