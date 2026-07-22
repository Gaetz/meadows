#include "data/plugins/PluginLoader.hpp"

#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "engine/core/Log.hpp"

namespace data {

namespace {

// TOML node -> reflect::Value, driven by the target field's kind. Returns
// nullopt on mismatch (no implicit coercion beyond TOML's own numerics).
std::optional<reflect::Value> convertValue(const toml::node& node,
                                           reflect::FieldKind kind) {
    using reflect::FieldKind;
    using reflect::Value;

    const auto floats =
        [&node](u32 count) -> std::optional<std::array<f32, 4>> {
        const toml::array* array = node.as_array();
        if (!array || array->size() != count) {
            return std::nullopt;
        }
        std::array<f32, 4> out {};
        for (u32 i = 0; i < count; ++i) {
            const auto value = (*array)[i].value<double>();
            if (!value) {
                return std::nullopt;
            }
            out[i] = static_cast<f32>(*value);
        }
        return out;
    };

    switch (kind) {
    case FieldKind::Bool:
        if (const auto value = node.value<bool>()) {
            return Value { *value };
        }
        return std::nullopt;
    case FieldKind::I32:
        if (const auto value = node.value<i64>()) {
            return Value { static_cast<i32>(*value) };
        }
        return std::nullopt;
    case FieldKind::U32:
        if (const auto value = node.value<i64>(); value && *value >= 0) {
            return Value { static_cast<u32>(*value) };
        }
        return std::nullopt;
    case FieldKind::F32:
        if (const auto value = node.value<double>()) {
            return Value { static_cast<f32>(*value) };
        }
        return std::nullopt;
    case FieldKind::F64:
        if (const auto value = node.value<double>()) {
            return Value { *value };
        }
        return std::nullopt;
    case FieldKind::Str:
        if (const auto value = node.value<std::string>()) {
            return Value { *value };
        }
        return std::nullopt;
    case FieldKind::Guid:
        if (const auto text = node.value<std::string>()) {
            if (const auto guid = core::Guid::fromString(*text)) {
                return Value { *guid };
            }
        }
        return std::nullopt;
    case FieldKind::Vec2:
        if (const auto v = floats(2)) {
            return Value { Vec2 { (*v)[0], (*v)[1] } };
        }
        return std::nullopt;
    case FieldKind::Vec3:
        if (const auto v = floats(3)) {
            return Value { Vec3 { (*v)[0], (*v)[1], (*v)[2] } };
        }
        return std::nullopt;
    case FieldKind::Vec4:
        if (const auto v = floats(4)) {
            return Value { Vec4 { (*v)[0], (*v)[1], (*v)[2], (*v)[3] } };
        }
        return std::nullopt;
    case FieldKind::Quat:
        // File order is [x, y, z, w]; glm's constructor takes w first.
        if (const auto v = floats(4)) {
            return Value { Quat { (*v)[3], (*v)[0], (*v)[1], (*v)[2] } };
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<core::Guid> guidOf(const toml::table& table,
                                 std::string_view key) {
    if (const auto text = table[key].value<std::string>()) {
        return core::Guid::fromString(*text);
    }
    return std::nullopt;
}

} // namespace

core::Result<Plugin> parsePluginToml(std::string_view text,
                                     const FormTypeRegistry& types,
                                     std::string_view sourceName) {
    // Fatal failures return their REASON (still logged here so the
    // headless tests keep the context in their output).
    const auto fail = [&](const str& reason) {
        LOG_ERROR("{}: {}", sourceName, reason);
        return core::Error { str { sourceName } + ": " + reason };
    };
    toml::parse_result result = toml::parse(text);
    if (!result) {
        std::ostringstream error;
        error << result.error();
        return fail("TOML parse error: " + error.str());
    }
    const toml::table& root = result.table();

    const toml::table* header = root["plugin"].as_table();
    if (!header) {
        return fail("missing [plugin] header");
    }

    Plugin plugin;
    const auto pluginId = guidOf(*header, "id");
    if (!pluginId) {
        return fail("[plugin] needs a valid 'id' guid");
    }
    plugin.id = *pluginId;
    plugin.name = (*header)["name"].value_or(std::string { sourceName });

    if (const toml::array* deps = (*header)["dependencies"].as_array()) {
        for (const toml::node& dep : *deps) {
            const auto text2 = dep.value<std::string>();
            const auto guid =
                text2 ? core::Guid::fromString(*text2) : std::nullopt;
            if (guid) {
                plugin.dependencies.push_back(*guid);
            } else {
                LOG_WARN("{}: ignored malformed dependency guid", sourceName);
            }
        }
    }

    if (const toml::table* assets = root["assets"].as_table()) {
        for (const auto& [key, node] : *assets) {
            const auto guid = core::Guid::fromString(key.str());
            const auto path = node.value<std::string>();
            if (!guid || !path || path->empty()) {
                LOG_WARN("{}: skipped malformed asset entry '{}'", sourceName,
                         key.str());
                continue;
            }
            plugin.assets.push_back({ *guid, *path });
        }
    }

    const toml::array* records = root["records"].as_array();
    if (!records) {
        return plugin; // a plugin with no records is legal (asset-only mods)
    }

    for (const toml::node& recordNode : *records) {
        const toml::table* recordTable = recordNode.as_table();
        if (!recordTable) {
            LOG_WARN("{}: skipped non-table record entry", sourceName);
            continue;
        }

        const auto formId = guidOf(*recordTable, "form");
        if (!formId) {
            LOG_WARN("{}: skipped record with missing/invalid 'form' guid",
                     sourceName);
            continue;
        }

        const auto typeName = (*recordTable)["type"].value<std::string>();
        const reflect::TypeInfo* type =
            typeName ? types.findType(*typeName) : nullptr;
        if (!type) {
            LOG_WARN("{}: skipped record {} with unknown type '{}'",
                     sourceName, formId->toString(),
                     typeName.value_or("<missing>"));
            continue;
        }

        Record record;
        record.formId = *formId;
        record.typeId = type->id;
        record.creates = (*recordTable)["new"].value_or(false);

        if (const toml::table* fields = (*recordTable)["fields"].as_table()) {
            for (const auto& [key, node] : *fields) {
                const reflect::FieldInfo* field = type->findField(key.str());
                if (!field) {
                    LOG_WARN("{}: record {}: unknown field '{}' on {}",
                             sourceName, formId->toString(), key.str(),
                             type->name);
                    continue;
                }
                if ((field->flags & reflect::Transient) != 0) {
                    LOG_WARN("{}: record {}: field '{}' is transient, not "
                             "patchable",
                             sourceName, formId->toString(), key.str());
                    continue;
                }
                auto value = convertValue(node, field->kind);
                if (!value) {
                    LOG_WARN("{}: record {}: field '{}' has the wrong type",
                             sourceName, formId->toString(), key.str());
                    continue;
                }
                record.fields.insert_or_assign(field->id, std::move(*value));
            }
        }

        plugin.records.push_back(std::move(record));
    }

    return plugin;
}

core::Result<Plugin> loadPluginFile(const std::filesystem::path& path,
                                    const FormTypeRegistry& types) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("Cannot open plugin file: {}", path.string());
        return core::Error { "cannot open " + path.string() };
    }
    std::ostringstream content;
    content << file.rdbuf();
    auto plugin =
        parsePluginToml(content.str(), types, path.filename().string());
    if (plugin) {
        plugin->baseDir = path.parent_path().string();
    }
    return plugin;
}

} // namespace data
