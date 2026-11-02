#include "engine/terrain/generation/ContinentGround.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/Hash.hpp"
#include "engine/core/Log.hpp"
#include "engine/reflect/Fields.hpp"
#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/TileBake.hpp"

namespace render::terraingen {

namespace {

constexpr char kGroundMagic[4] = { 'C', 'G', 'R', '3' };
constexpr char kOverviewMagic[4] = { 'C', 'G', 'O', '1' };
constexpr u32 kFormatVersion = 5; // 5: piercing on pass-start basins, band cut capped
constexpr f32 kOverviewTexel = 64.0f;

struct Memo {
    u64 key;
    sptr<const ContinentGround> ground;
};
std::mutex gMemoMutex;
std::unordered_map<u64, vector<Memo>> gMemo; // (seed, cell) -> entries

u64 cellKey(u32 seed, i32 superX, i32 superZ) {
    return (static_cast<u64>(seed) << 32) ^
           (static_cast<u64>(static_cast<u32>(superX)) << 16) ^
           static_cast<u64>(static_cast<u32>(superZ));
}

sptr<const ContinentGround> memoFind(u32 seed, i32 superX, i32 superZ,
                                     u64 key) {
    std::lock_guard<std::mutex> lock { gMemoMutex };
    const auto it = gMemo.find(cellKey(seed, superX, superZ));
    if (it == gMemo.end()) {
        return nullptr;
    }
    for (const Memo& memo : it->second) {
        if (memo.key == key) {
            return memo.ground;
        }
    }
    return nullptr;
}

void memoStore(u32 seed, i32 superX, i32 superZ, u64 key,
               sptr<const ContinentGround> ground) {
    std::lock_guard<std::mutex> lock { gMemoMutex };
    gMemo[cellKey(seed, superX, superZ)].push_back(
        { key, std::move(ground) });
}

std::filesystem::path groundPath(const std::filesystem::path& dir, u64 key,
                                 i32 superX, i32 superZ) {
    char name[96];
    std::snprintf(name, sizeof(name), "continent_%d_%d_%016llx.bin", superX,
                  superZ, static_cast<unsigned long long>(key));
    return dir / name;
}

std::filesystem::path overviewPath(const std::filesystem::path& dir,
                                   u64 key, i32 superX, i32 superZ) {
    char name[96];
    std::snprintf(name, sizeof(name), "continent_%d_%d_%016llx_ov.bin",
                  superX, superZ, static_cast<unsigned long long>(key));
    return dir / name;
}

bool readHeader(std::ifstream& file, const char magic[4], GridSpec& grid) {
    char got[4] = {};
    file.read(got, 4);
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    read(grid.originX);
    read(grid.originZ);
    read(grid.texelSize);
    read(grid.n);
    return file && std::memcmp(got, magic, 4) == 0 && grid.n >= 2 &&
           grid.n <= 8192;
}

bool readGrid(std::ifstream& file, size_t cells, vector<f32>& out) {
    out.resize(cells);
    file.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(cells * sizeof(f32)));
    return static_cast<bool>(file);
}

bool readGroundFile(const std::filesystem::path& path,
                    ContinentGround& out) {
    std::ifstream file { path, std::ios::binary };
    if (!file || !readHeader(file, kGroundMagic, out.grid)) {
        return false;
    }
    return readGrid(file, out.grid.cells(), out.height) &&
           readGrid(file, out.grid.cells(), out.deposit);
}

bool readOverviewFile(const std::filesystem::path& path,
                      ContinentOverview& out) {
    std::ifstream file { path, std::ios::binary };
    if (!file || !readHeader(file, kOverviewMagic, out.grid)) {
        return false;
    }
    return readGrid(file, out.grid.cells(), out.height);
}

// Atomic publish: written to a sibling, renamed into place (a
// concurrent writer of the same file loses the race harmlessly).
void writeAtomic(const std::filesystem::path& path,
                 const std::function<void(std::ofstream&)>& body) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream file { tmp, std::ios::binary | std::ios::trunc };
        if (!file) {
            return;
        }
        body(file);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
    }
}

void writeHeader(std::ofstream& file, const char magic[4],
                 const GridSpec& grid) {
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(magic, 4);
    write(grid.originX);
    write(grid.originZ);
    write(grid.texelSize);
    write(grid.n);
}

void writeGrid(std::ofstream& file, const vector<f32>& grid) {
    file.write(reinterpret_cast<const char*>(grid.data()),
               static_cast<std::streamsize>(grid.size() * sizeof(f32)));
}

ContinentOverview decimate(const ContinentGround& ground) {
    ContinentOverview ov;
    const u32 step = glm::max(
        1u, static_cast<u32>(std::lround(kOverviewTexel /
                                         ground.grid.texelSize)));
    ov.grid.originX = ground.grid.originX;
    ov.grid.originZ = ground.grid.originZ;
    ov.grid.texelSize = ground.grid.texelSize * static_cast<f32>(step);
    ov.grid.n = (ground.grid.n - 1) / step + 1;
    ov.height.resize(ov.grid.cells());
    for (u32 row = 0; row < ov.grid.n; ++row) {
        for (u32 col = 0; col < ov.grid.n; ++col) {
            ov.height[static_cast<size_t>(row) * ov.grid.n + col] =
                ground.height[static_cast<size_t>(row) * step *
                                  ground.grid.n +
                              static_cast<size_t>(col) * step];
        }
    }
    return ov;
}

// The params of the continent's own stage-1: the maps' recipe and
// texel, the border lattice shaped as the maps shape it (the same pure
// function of the seed and the map size), no continent of its own.
TileBakeParams continentParams(const TileBakeParams& params, f32 mapSize) {
    TileBakeParams p = params;
    p.continent.enabled = false;
    if (p.mapGrid.valid) {
        p.mapGrid.seed = p.worldSeed;
        p.mapGrid.mapSize = mapSize;
        p.mapGrid.seaLevel = p.macro.seaLevel;
    }
    return p;
}

GridSpec continentSpec(const TileBakeParams& p, i32 superX, i32 superZ) {
    const MasterNetworkParams& net = p.network;
    GridSpec sim;
    sim.texelSize = p.macroTexel;
    sim.originX = net.superOrigin +
                  static_cast<f32>(superX) * net.superRegionSize -
                  net.apron;
    sim.originZ = net.superOrigin +
                  static_cast<f32>(superZ) * net.superRegionSize -
                  net.apron;
    const f32 span = net.superRegionSize + 2.0f * net.apron;
    sim.n = static_cast<u32>(std::lround(span / sim.texelSize)) + 1;
    return sim;
}

} // namespace

u64 hashKey(const ContinentGroundParams& p) {
    core::Fnv64 h;
    reflect::hashReflected(ContinentGroundParams::staticTypeInfo(), &p, h);
    return h.value;
}

bool ContinentGround::heightAt(f32 x, f32 z, f32& out) const {
    if (!valid()) {
        return false;
    }
    const f32 u = (x - grid.originX) / grid.texelSize;
    const f32 v = (z - grid.originZ) / grid.texelSize;
    if (u < 0.0f || v < 0.0f || u > static_cast<f32>(grid.n - 1) ||
        v > static_cast<f32>(grid.n - 1)) {
        return false;
    }
    const u32 c0 = glm::min(static_cast<u32>(u), grid.n - 2);
    const u32 r0 = glm::min(static_cast<u32>(v), grid.n - 2);
    const f32 tu = u - static_cast<f32>(c0);
    const f32 tv = v - static_cast<f32>(r0);
    const auto at = [&](u32 c, u32 r) {
        return height[static_cast<size_t>(r) * grid.n + c];
    };
    const f32 a = glm::mix(at(c0, r0), at(c0 + 1, r0), tu);
    const f32 b = glm::mix(at(c0, r0 + 1), at(c0 + 1, r0 + 1), tu);
    out = glm::mix(a, b, tv);
    return true;
}

i32 continentSuperIndex(const MasterNetworkParams& network, f32 v) {
    return static_cast<i32>(
        std::floor((v - network.superOrigin) / network.superRegionSize));
}

u64 continentGroundKey(const TileBakeParams& params, f32 mapSize) {
    // The stage-1 and piercing inputs ONLY — a finalize or water-solve
    // knob must not re-bake minutes of continent. Keep in step with
    // bakeTileStage1's reads of TileBakeParams and with
    // pierceLakeOutlets (hydrology).
    core::Fnv64 h;
    h.mix(params.worldSeed);
    h.mix(params.macroTexel);
    h.mix(params.calmCut);
    h.mix(params.roughCut);
    h.mix(params.dimpleFillMax);
    h.mix(params.keepCrestFade);
    h.mix(hashKey(params.controls));
    h.mix(hashKey(params.macro));
    h.mix(hashKey(params.fluvial));
    h.mix(hashKey(params.thermal));
    h.mix(hashKey(params.rounding));
    h.mix(hashKey(params.hydrology));
    h.mix(hashKey(params.network));
    h.mix(hashKey(params.imprint));
    h.mix(hashKey(params.mapGrid));
    h.mix(static_cast<u32>(params.biomeErosion.size()));
    for (const BiomeErosion& b : params.biomeErosion) {
        reflect::hashReflected(BiomeErosion::staticTypeInfo(), &b, h);
    }
    h.mix(mapSize);
    h.mix(kFormatVersion);
    return h.value;
}

sptr<const ContinentGround> continentGroundFor(
    const TileBakeParams& params, f32 mapSize, i32 superX, i32 superZ,
    const std::atomic<bool>* cancel) {
    const u64 key = continentGroundKey(params, mapSize);
    if (auto hit = memoFind(params.worldSeed, superX, superZ, key)) {
        return hit;
    }
    const std::filesystem::path dir = masterNetworkCacheDir();
    auto ground = std::make_shared<ContinentGround>();
    bool fromDisk = false;
    if (!dir.empty()) {
        fromDisk = readGroundFile(groundPath(dir, key, superX, superZ),
                                  *ground);
    }
    if (!fromDisk) {
        const auto start = std::chrono::steady_clock::now();
        const TileBakeParams p = continentParams(params, mapSize);
        const GridSpec sim = continentSpec(p, superX, superZ);
        TileStage1 s1 = bakeTileStage1(p, sim, nullptr, cancel);
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            return nullptr; // partial: never cached
        }
        ground->grid = sim;
        ground->height = std::move(s1.eroded);
        ground->deposit = std::move(s1.deposit);
        s1 = TileStage1 {}; // the other stage-1 grids: gigabytes, done
        // The outlet piercing at continent scale: the basins the storey
        // walls dam are drained ONCE for the whole continent, their
        // gorges shared by every map (the per-map pierce then meets
        // drained valleys and only notches what the finalize's own
        // detail dams).
        HydrologyParams hydrology = p.hydrology;
        hydrology.seaLevel = p.macro.seaLevel;
        const vector<f32> prePierce = ground->height;
        pierceLakeOutlets(sim, ground->height, hydrology);
        f64 pierceSum = 0.0;
        f32 deepest = 0.0f;
        u32 cut10 = 0;
        for (size_t i = 0; i < prePierce.size(); ++i) {
            const f32 d = ground->height[i] - prePierce[i];
            pierceSum += d;
            deepest = glm::min(deepest, d);
            cut10 += d < -10.0f;
        }
        LOG_INFO("continent ground ({}, {}): {}x{} at {:.0f} m in {:.1f} s "
                 "— pierce mean {:+.2f} m, deepest {:.1f} m, {:.1f} % texels "
                 "cut > 10 m",
                 superX, superZ, sim.n, sim.n, sim.texelSize,
                 std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                            start)
                     .count(),
                 pierceSum / static_cast<f64>(prePierce.size()), deepest,
                 100.0 * cut10 / static_cast<f64>(prePierce.size()));
        if (!dir.empty()) {
            writeAtomic(groundPath(dir, key, superX, superZ),
                        [&](std::ofstream& file) {
                            writeHeader(file, kGroundMagic, ground->grid);
                            writeGrid(file, ground->height);
                            writeGrid(file, ground->deposit);
                        });
            const ContinentOverview ov = decimate(*ground);
            writeAtomic(overviewPath(dir, key, superX, superZ),
                        [&](std::ofstream& file) {
                            writeHeader(file, kOverviewMagic, ov.grid);
                            writeGrid(file, ov.height);
                        });
        }
    }
    memoStore(params.worldSeed, superX, superZ, key, ground);
    return ground;
}

std::optional<ContinentGround> loadContinentGround(
    const std::filesystem::path& dir, const TileBakeParams& params,
    f32 mapSize, i32 superX, i32 superZ) {
    const u64 key = continentGroundKey(params, mapSize);
    if (auto hit = memoFind(params.worldSeed, superX, superZ, key)) {
        return *hit;
    }
    ContinentGround ground;
    if (dir.empty() ||
        !readGroundFile(groundPath(dir, key, superX, superZ), ground)) {
        return std::nullopt;
    }
    return ground;
}

std::optional<ContinentOverview> loadContinentOverview(
    const std::filesystem::path& dir, const TileBakeParams& params,
    f32 mapSize, i32 superX, i32 superZ) {
    const u64 key = continentGroundKey(params, mapSize);
    if (auto hit = memoFind(params.worldSeed, superX, superZ, key)) {
        return decimate(*hit);
    }
    ContinentOverview ov;
    if (dir.empty() ||
        !readOverviewFile(overviewPath(dir, key, superX, superZ), ov)) {
        return std::nullopt;
    }
    return ov;
}

} // namespace render::terraingen
