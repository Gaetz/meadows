#pragma once

#include <filesystem>
#include <optional>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

namespace assets {

// Layered virtual file system, GUID-keyed (§5): callers add entries in
// plugin load order and the last writer wins per asset id — the same
// mental model as record fields. Knows nothing about plugins; the data
// layer (or game startup) feeds it.
//
// Phase-1 scope: identity + layering + synchronous resolution. Async
// loading, residency, and placeholders are Phase 8 / §7 concerns.
class AssetDatabase {
public:
    // Registers or overrides an asset. `path` may be relative to `baseDir`.
    void add(const core::Guid& id, const std::filesystem::path& baseDir,
             std::string_view path);

    std::optional<std::filesystem::path> resolve(const core::Guid& id) const;

    u32 count() const { return static_cast<u32>(paths.size()); }

private:
    std::unordered_map<core::Guid, std::filesystem::path> paths;
};

} // namespace assets
