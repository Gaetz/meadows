#pragma once

#include <filesystem>
#include <optional>

#include "engine/assets/MeshData.hpp"
#include "engine/core/Defines.hpp"

// Cooked-mesh IO: `.cmesh` carries one render::MeshData verbatim (generator
// output — dungeon cavern chunks). Distinct from glTF on purpose: MeshCache's
// glTF decode path recenters to the ground plane and bakes disk AO, both
// wrong for pre-baked, cell-aligned chunks; `.cmesh` loads as authored.
// Sibling of CookedTexture (.mtex) and TerrainRegions (.trg).
//
// File: magic "CMSH", u32 formatVersion, u32 contentVersion (the producer's
// bake version, e.g. kDungeonBakeVersion — the loader refuses a mismatch so
// stale assets fail loudly instead of rendering garbage), u32 vertexCount,
// u32 indexCount, Vec3 boundsMin, Vec3 boundsMax, then the interleaved
// MeshVertex array and the u32 index array. Little-endian.

namespace assets {

constexpr u32 kCookedMeshFormatVersion = 1;

bool saveCookedMesh(const std::filesystem::path& path,
                    const render::MeshData& mesh, u32 contentVersion);

std::optional<render::MeshData> loadCookedMesh(
    const std::filesystem::path& path, u32 expectedContentVersion);

// Version-blind load, for generic consumers (MeshCache): a shipped mod's
// mesh is just a mesh — only the PRODUCER (re-bake, caches) must insist on
// its own content version.
std::optional<render::MeshData> loadCookedMesh(
    const std::filesystem::path& path);

} // namespace assets
