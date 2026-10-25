#include "data/plugins/BinaryFormat.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "engine/core/BinaryArchive.hpp"
#include "engine/core/Log.hpp"
#include "engine/reflect/BinaryValue.hpp"

namespace data {

namespace {

constexpr u8 kMagic[4] = { 'M', 'D', 'W', 'P' };

} // namespace

vector<u8> writePluginBinary(const Plugin& plugin) {
    vector<u8> out;
    core::ByteWriter w { out };

    w.bytes(kMagic, sizeof(kMagic));
    w.u32_(kPluginBinaryVersion);

    w.guid(plugin.id);
    w.str_(plugin.name);
    w.u32_(static_cast<u32>(plugin.dependencies.size()));
    for (const core::Guid& dep : plugin.dependencies) {
        w.guid(dep);
    }

    w.u32_(static_cast<u32>(plugin.assets.size()));
    for (const AssetEntry& asset : plugin.assets) {
        w.guid(asset.id);
        w.str_(asset.path);
    }

    w.u32_(static_cast<u32>(plugin.records.size()));
    for (const Record& record : plugin.records) {
        w.guid(record.formId);
        w.u32_(record.typeId);
        w.u8_(record.creates ? 1 : 0);

        // Sorted by field id: byte-identical output for identical input.
        vector<std::pair<u32, const reflect::Value*>> sorted;
        sorted.reserve(record.fields.size());
        for (const auto& [id, value] : record.fields) {
            sorted.emplace_back(id, &value);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        w.u32_(static_cast<u32>(sorted.size()));
        for (const auto& [id, value] : sorted) {
            w.u32_(id);
            reflect::writeValue(w, *value);
        }
    }
    return out;
}

std::optional<Plugin> readPluginBinary(std::span<const u8> bytes,
                                       std::string_view sourceName) {
    core::ByteReader r { bytes };

    u8 magic[4] = {};
    if (!r.bytes_(magic, sizeof(magic)) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        LOG_ERROR("{}: not a cooked plugin (bad magic)", sourceName);
        return std::nullopt;
    }
    u32 version = 0;
    if (!r.u32_(version) || version != kPluginBinaryVersion) {
        LOG_ERROR("{}: unsupported cooked version {} (expected {})",
                  sourceName, version, kPluginBinaryVersion);
        return std::nullopt;
    }

    const auto truncated = [&sourceName]() -> std::optional<Plugin> {
        LOG_ERROR("{}: truncated or corrupt cooked plugin", sourceName);
        return std::nullopt;
    };

    Plugin plugin;
    if (!r.guid(plugin.id) || !r.str_(plugin.name)) {
        return truncated();
    }
    u32 dependencyCount = 0;
    if (!r.u32_(dependencyCount)) {
        return truncated();
    }
    for (u32 i = 0; i < dependencyCount; ++i) {
        core::Guid dep;
        if (!r.guid(dep)) {
            return truncated();
        }
        plugin.dependencies.push_back(dep);
    }

    u32 assetCount = 0;
    if (!r.u32_(assetCount)) {
        return truncated();
    }
    for (u32 i = 0; i < assetCount; ++i) {
        AssetEntry asset;
        if (!r.guid(asset.id) || !r.str_(asset.path)) {
            return truncated();
        }
        plugin.assets.push_back(std::move(asset));
    }

    u32 recordCount = 0;
    if (!r.u32_(recordCount)) {
        return truncated();
    }
    for (u32 i = 0; i < recordCount; ++i) {
        Record record;
        u8 creates = 0;
        if (!r.guid(record.formId) || !r.u32_(record.typeId) ||
            !r.u8_(creates)) {
            return truncated();
        }
        record.creates = (creates != 0);

        u32 fieldCount = 0;
        if (!r.u32_(fieldCount)) {
            return truncated();
        }
        for (u32 f = 0; f < fieldCount; ++f) {
            u32 fieldId = 0;
            reflect::Value value;
            if (!r.u32_(fieldId) || !reflect::readValue(r, value)) {
                return truncated();
            }
            record.fields.insert_or_assign(fieldId, std::move(value));
        }
        plugin.records.push_back(std::move(record));
    }

    // Asset paths resolve against the plugin's directory, like the TOML
    // loader sets it; `sourceName` is the file path (or a bare label).
    plugin.baseDir =
        std::filesystem::path { sourceName }.parent_path().string();
    return plugin;
}

} // namespace data
