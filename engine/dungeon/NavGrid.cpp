#include "engine/dungeon/NavGrid.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "engine/core/Log.hpp"

namespace dungeon {

namespace {

constexpr char kMagic[4] = { 'N', 'V', 'G', '1' };
constexpr u32 kMaxDim = 16u * 1024u;
// Walkable levels stack per column; far more than any real mine needs —
// a sanity bound for the reader, not a design limit.
constexpr u64 kMaxLevelsPerColumn = 64;

} // namespace

u32 NavGrid::columnOf(f32 x, f32 z) const {
    const i32 ix = static_cast<i32>(std::floor((x - originX) / cellSize));
    const i32 iz = static_cast<i32>(std::floor((z - originZ) / cellSize));
    if (ix < 0 || iz < 0 || ix >= static_cast<i32>(width) ||
        iz >= static_cast<i32>(depth)) {
        return ~0u;
    }
    return static_cast<u32>(iz) * width + static_cast<u32>(ix);
}

void NavGrid::columnLevels(u32 column, u32& begin, u32& end) const {
    begin = firstLevel[column];
    end = firstLevel[column + 1];
}

bool NavGrid::airAt(i32 ix, i32 iz, f32 y) const {
    if (ix < 0 || iz < 0 || ix >= static_cast<i32>(width) ||
        iz >= static_cast<i32>(depth)) {
        return false;
    }
    u32 begin = 0;
    u32 end = 0;
    columnLevels(static_cast<u32>(iz) * width + static_cast<u32>(ix), begin,
                 end);
    for (u32 l = begin; l < end; ++l) {
        if (levels[l].floorY <= y + 0.6f &&
            levels[l].floorY + levels[l].clearance >= y + 1.5f) {
            return true;
        }
    }
    return false;
}

bool NavGrid::wallAdjacent(i32 ix, i32 iz, f32 y) const {
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if ((dx != 0 || dz != 0) && !airAt(ix + dx, iz + dz, y)) {
                return true;
            }
        }
    }
    return false;
}

NavGrid bakeNavGrid(const DensityFn& density, const Vec3& min, const Vec3& max,
                    f32 cellSize, f32 minClearance) {
    NavGrid grid;
    grid.cellSize = cellSize;
    grid.originX = min.x;
    grid.originZ = min.z;
    grid.width = static_cast<u32>(std::ceil((max.x - min.x) / cellSize));
    grid.depth = static_cast<u32>(std::ceil((max.z - min.z) / cellSize));
    grid.firstLevel.reserve(static_cast<size_t>(grid.width) * grid.depth + 1);
    grid.firstLevel.push_back(0);

    const f32 step = 0.25f;
    // Sphere-traced column march: the density lower-bounds the distance to
    // the nearest surface (minus the wall noise, hence the 1 m margin), so
    // deep rock and tall air both leap in one step; only the last meter
    // around each surface walks at `step`. Same crossings, ~10x fewer
    // samples than a fixed march.
    const f32 leapMargin = 1.0f;
    for (u32 iz = 0; iz < grid.depth; ++iz) {
        for (u32 ix = 0; ix < grid.width; ++ix) {
            const f32 x = min.x + (static_cast<f32>(ix) + 0.5f) * cellSize;
            const f32 z = min.z + (static_cast<f32>(iz) + 0.5f) * cellSize;
            f32 below = density({ x, min.y - step, z });
            f32 y = min.y;
            while (y <= max.y) {
                const f32 here = density({ x, y, z });
                if (std::abs(here) > leapMargin + step) {
                    below = here;
                    y += std::abs(here) - leapMargin;
                    continue;
                }
                if (below >= 0.0f && here < 0.0f) {
                    // Solid -> air going up: a floor. Refine the crossing,
                    // then measure headroom to the next ceiling.
                    f32 lo = y - step;
                    f32 hi = y;
                    for (i32 i = 0; i < 4; ++i) {
                        const f32 mid = 0.5f * (lo + hi);
                        (density({ x, mid, z }) < 0.0f ? hi : lo) = mid;
                    }
                    const f32 floorY = 0.5f * (lo + hi);
                    f32 ceiling = floorY;
                    f32 probe;
                    while (ceiling <= max.y &&
                           (probe = density({ x, ceiling + step, z })) <
                               0.0f) {
                        ceiling += std::max(step, -probe - leapMargin);
                    }
                    const f32 clearance = ceiling - floorY;
                    if (clearance >= minClearance) {
                        grid.levels.push_back({ floorY, clearance });
                    }
                    y = ceiling; // resume above this air pocket
                }
                below = here;
                y += step;
            }
            grid.firstLevel.push_back(static_cast<u32>(grid.levels.size()));
        }
    }
    return grid;
}

bool writeNvgFile(const std::filesystem::path& path, const NavGrid& grid,
                  u32 contentVersion) {
    if (grid.width == 0 || grid.depth == 0 ||
        grid.firstLevel.size() !=
            static_cast<size_t>(grid.width) * grid.depth + 1) {
        LOG_ERROR("writeNvgFile: malformed grid for {}", path.string());
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("writeNvgFile: cannot open {}", path.string());
        return false;
    }
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(kMagic, 4);
    write(contentVersion);
    write(grid.originX);
    write(grid.originZ);
    write(grid.cellSize);
    write(grid.width);
    write(grid.depth);
    write(static_cast<u32>(grid.levels.size()));
    file.write(reinterpret_cast<const char*>(grid.firstLevel.data()),
               static_cast<std::streamsize>(grid.firstLevel.size() *
                                            sizeof(u32)));
    file.write(reinterpret_cast<const char*>(grid.levels.data()),
               static_cast<std::streamsize>(grid.levels.size() *
                                            sizeof(NavGrid::Level)));
    return static_cast<bool>(file);
}

std::optional<NavGrid> readNvgFile(const std::filesystem::path& path,
                                   u32 expectedContentVersion) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("readNvgFile: cannot open {}", path.string());
        return std::nullopt;
    }
    char magic[4] = {};
    u32 contentVersion = 0;
    u32 levelCount = 0;
    NavGrid grid;
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    file.read(magic, 4);
    read(contentVersion);
    read(grid.originX);
    read(grid.originZ);
    read(grid.cellSize);
    read(grid.width);
    read(grid.depth);
    read(levelCount);
    if (!file || std::memcmp(magic, kMagic, 4) != 0) {
        LOG_ERROR("readNvgFile: not an NVG1 file: {}", path.string());
        return std::nullopt;
    }
    if (contentVersion != expectedContentVersion) {
        LOG_ERROR("readNvgFile: stale content v{} (want v{}): {}",
                  contentVersion, expectedContentVersion, path.string());
        return std::nullopt;
    }
    if (grid.width == 0 || grid.depth == 0 || grid.width > kMaxDim ||
        grid.depth > kMaxDim || grid.cellSize <= 0.0f) {
        LOG_ERROR("readNvgFile: bad dimensions in {}", path.string());
        return std::nullopt;
    }
    // Bound levelCount before the resize: a corrupt header must fail as
    // nullopt, not as a multi-GB allocation.
    if (levelCount > static_cast<u64>(grid.width) * grid.depth *
                         kMaxLevelsPerColumn) {
        LOG_ERROR("readNvgFile: implausible level count in {}",
                  path.string());
        return std::nullopt;
    }
    grid.firstLevel.resize(static_cast<size_t>(grid.width) * grid.depth + 1);
    grid.levels.resize(levelCount);
    file.read(reinterpret_cast<char*>(grid.firstLevel.data()),
              static_cast<std::streamsize>(grid.firstLevel.size() *
                                           sizeof(u32)));
    file.read(reinterpret_cast<char*>(grid.levels.data()),
              static_cast<std::streamsize>(grid.levels.size() *
                                           sizeof(NavGrid::Level)));
    if (!file || grid.firstLevel.front() != 0 ||
        grid.firstLevel.back() != levelCount) {
        LOG_ERROR("readNvgFile: truncated or inconsistent file: {}",
                  path.string());
        return std::nullopt;
    }
    return grid;
}

} // namespace dungeon
