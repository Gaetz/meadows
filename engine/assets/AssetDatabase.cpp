#include "engine/assets/AssetDatabase.hpp"

#include "engine/core/Log.hpp"

namespace assets {

void AssetDatabase::add(const core::Guid& id,
                        const std::filesystem::path& baseDir,
                        std::string_view path) {
    if (!id.isValid()) {
        LOG_WARN("AssetDatabase: ignored entry with invalid guid");
        return;
    }
    std::filesystem::path full = baseDir / path;
    if (!std::filesystem::exists(full)) {
        LOG_WARN("AssetDatabase: {} -> missing file '{}'", id.toString(),
                 full.string());
        // Registered anyway: the resolve-time consumer decides how to
        // placeholder. Layering stays predictable.
    }
    paths.insert_or_assign(id, std::move(full));
}

std::optional<std::filesystem::path>
AssetDatabase::resolve(const core::Guid& id) const {
    const auto it = paths.find(id);
    if (it == paths.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace assets
