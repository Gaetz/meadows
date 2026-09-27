#include "world/terrain/TerrainPatches.hpp"

#include <algorithm>
#include <cstdio>

#include <cstring>
#include <fstream>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {
constexpr char kMagic[4] = { 'T', 'E', 'R', '1' };
}

bool writeTerFile(const std::filesystem::path& path,
                  const render::HeightPatch& patch) {
    if (patch.samples < 2 ||
        patch.deltas.size() !=
            static_cast<size_t>(patch.samples) * patch.samples) {
        LOG_ERROR("writeTerFile: malformed patch for {}", path.string());
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("writeTerFile: cannot open {}", path.string());
        return false;
    }
    file.write(kMagic, 4);
    file.write(reinterpret_cast<const char*>(&patch.samples),
               sizeof(patch.samples));
    file.write(reinterpret_cast<const char*>(patch.deltas.data()),
               static_cast<std::streamsize>(patch.deltas.size() *
                                            sizeof(f32)));
    return static_cast<bool>(file);
}

std::optional<render::HeightPatch> readTerFile(
    const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("readTerFile: cannot open {}", path.string());
        return std::nullopt;
    }
    char magic[4] = {};
    u32 samples = 0;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&samples), sizeof(samples));
    if (!file || std::memcmp(magic, kMagic, 4) != 0 || samples < 2 ||
        samples > 4096) {
        LOG_ERROR("readTerFile: not a TER1 grid: {}", path.string());
        return std::nullopt;
    }
    render::HeightPatch patch;
    patch.samples = samples;
    patch.deltas.resize(static_cast<size_t>(samples) * samples);
    file.read(reinterpret_cast<char*>(patch.deltas.data()),
              static_cast<std::streamsize>(patch.deltas.size() *
                                           sizeof(f32)));
    if (!file) {
        LOG_ERROR("readTerFile: truncated grid: {}", path.string());
        return std::nullopt;
    }
    return patch;
}

sptr<const render::HeightPatches> buildHeightPatches(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets,
    f32 chunkSize, const WorldspaceFilter& filter) {
    auto patches = std::make_shared<render::HeightPatches>();
    patches->chunkSize = chunkSize;
    data::forEach<TerrainPatchForm>(
        forms, [&](const TerrainPatchForm& form) {
            if (!filter.matches(form.worldspace)) {
                return;
            }
            const auto path = assets.resolve(form.asset);
            if (!path) {
                LOG_WARN("TerrainPatch ({}, {}): asset {} not registered",
                         form.chunkX, form.chunkZ, form.asset.toString());
                return;
            }
            auto grid = readTerFile(*path);
            if (!grid) {
                return;
            }
            patches->chunks.emplace(
                render::HeightPatches::keyOf(form.chunkX, form.chunkZ),
                std::move(*grid));
        });
    return patches;
}

namespace {

core::Guid chunkGuid(const char* prefix, u64 chunkKey) {
    char text[40];
    std::snprintf(text, sizeof(text), "%s%012llx", prefix,
                  static_cast<unsigned long long>(chunkKey & 0xFFFFFFFFFFFFull));
    return *core::Guid::fromString(text);
}

} // namespace

core::Guid saveTerrainAssetGuid(u64 chunkKey) {
    return chunkGuid(kSaveTerrainAssetPrefix, chunkKey);
}

bool isSaveTerrainAsset(const core::Guid& asset) {
    return asset.toString().rfind(kSaveTerrainAssetPrefix, 0) == 0;
}

void stageTerrainPatchRecords(const render::HeightPatches& patches,
                              const vector<u64>& chunks,
                              const data::FormDatabase& forms,
                              const core::Guid& worldspace,
                              const std::filesystem::path& dir,
                              const str& assetPrefix,
                              vector<data::Record>& records,
                              vector<data::AssetEntry>& assets) {
    std::error_code errc;
    std::filesystem::create_directories(dir, errc);
    const reflect::TypeInfo& type = TerrainPatchForm::staticTypeInfo();
    const reflect::FieldInfo* assetField = type.findField("asset");
    const reflect::FieldInfo* worldspaceField = type.findField("worldspace");
    const reflect::FieldInfo* chunkXField = type.findField("chunkX");
    const reflect::FieldInfo* chunkZField = type.findField("chunkZ");
    // Deterministic order (§8): sorted chunk keys.
    vector<u64> sorted = chunks;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (const u64 key : sorted) {
        const auto it = patches.chunks.find(key);
        if (it == patches.chunks.end()) {
            continue;
        }
        const i32 cx = static_cast<i32>(key >> 32);
        const i32 cz = static_cast<i32>(key & 0xffffffffu);
        char name[64];
        std::snprintf(name, sizeof(name), "patch_%d_%d.ter", cx, cz);
        if (!writeTerFile(dir / name, it->second)) {
            continue;
        }
        const core::Guid assetGuid = saveTerrainAssetGuid(key);
        assets.push_back({ assetGuid, assetPrefix + "/" + name });
        // The chunk's record: patch the authored one, else create.
        core::Guid existing;
        forEach<TerrainPatchForm>(forms, [&](const TerrainPatchForm& form) {
            if (form.chunkX == cx && form.chunkZ == cz &&
                (form.worldspace == worldspace || !form.worldspace.isValid())) {
                existing = form.id;
            }
        });
        data::Record record;
        record.typeId = type.id;
        if (existing.isValid()) {
            record.formId = existing;
            record.creates = false;
        } else {
            record.formId = chunkGuid(kSaveTerrainRecordPrefix, key);
            record.creates = true;
            record.fields.emplace(worldspaceField->id,
                                  reflect::Value { worldspace });
            record.fields.emplace(chunkXField->id, reflect::Value { cx });
            record.fields.emplace(chunkZField->id, reflect::Value { cz });
        }
        record.fields.emplace(assetField->id, reflect::Value { assetGuid });
        records.push_back(std::move(record));
    }
}

} // namespace world
