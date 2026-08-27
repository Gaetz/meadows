#include "WaterReplayTool.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

#include <glm/glm.hpp>
#include <stb_image_write.h>

#include "engine/core/Log.hpp"
#include "engine/terrain/WaterSim.hpp"

namespace cooker {

namespace {

using render::terrain::WaterSimParams;
using render::terrain::WaterSimState;
using render::terraingen::GridSpec;

struct SimStats {
    u32 wet { 0 };
    u32 pinned { 0 };
    u32 isolated { 0 };
    f64 volume { 0.0 };
    f32 maxDepth { 0.0f };
    f32 maxSpeed { 0.0f };
};

SimStats stats(const WaterSimState& state, const WaterSimParams& params) {
    SimStats out;
    const GridSpec& spec = state.spec;
    const f32 texel = spec.texelSize;
    const auto wetAt = [&](size_t j) {
        return state.depth[j] > params.dryThreshold;
    };
    for (size_t i = 0; i < spec.cells(); ++i) {
        const f32 d = state.depth[i];
        out.volume += static_cast<f64>(d) * texel * texel;
        out.maxDepth = glm::max(out.maxDepth, d);
        if (state.pinned.size() == spec.cells() &&
            state.pinned[i] > render::terrain::kWaterInfoDry + 1.0f) {
            ++out.pinned;
        }
        if (!wetAt(i)) {
            continue;
        }
        ++out.wet;
        const f32 fx = state.fE[i] - state.fW[i];
        const f32 fz = state.fS[i] - state.fN[i];
        const f32 div = glm::max(d, 0.05f) * texel;
        out.maxSpeed =
            glm::max(out.maxSpeed, std::hypot(fx, fz) / div);
        const size_t col = i % spec.n;
        const size_t row = i / spec.n;
        bool connected = false;
        for (i32 dz = -1; dz <= 1 && !connected; ++dz) {
            for (i32 dx = -1; dx <= 1 && !connected; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const i32 nc = static_cast<i32>(col) + dx;
                const i32 nr = static_cast<i32>(row) + dz;
                if (nc >= 0 && nr >= 0 &&
                    nc < static_cast<i32>(spec.n) &&
                    nr < static_cast<i32>(spec.n) &&
                    wetAt(static_cast<size_t>(nr) * spec.n +
                          static_cast<size_t>(nc))) {
                    connected = true;
                }
            }
        }
        if (!connected) {
            ++out.isolated;
        }
    }
    return out;
}

void renderMap(const WaterSimState& state, const WaterSimParams& params,
               const std::string& path) {
    const GridSpec& spec = state.spec;
    const i32 n = static_cast<i32>(spec.n);
    const auto groundAt = [&](i32 c, i32 r) {
        c = glm::clamp(c, 0, n - 1);
        r = glm::clamp(r, 0, n - 1);
        return state.terrain[static_cast<size_t>(r) * spec.n +
                             static_cast<size_t>(c)];
    };
    vector<u8> pixels(static_cast<size_t>(n) * n * 3);
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n +
                             static_cast<size_t>(col);
            const f32 gx = groundAt(col - 1, row) - groundAt(col + 1, row);
            const f32 gz = groundAt(col, row - 1) - groundAt(col, row + 1);
            const Vec3 normal = glm::normalize(
                Vec3 { gx, 0.6f * spec.texelSize, gz });
            const f32 light = glm::clamp(
                0.25f + 0.70f * glm::dot(normal,
                                         glm::normalize(Vec3 {
                                             -0.45f, 0.8f, -0.4f })),
                0.0f, 1.0f);
            f32 r = light * 0.62f;
            f32 g = light * 0.60f;
            f32 b = light * 0.55f;
            const bool pin =
                state.pinned.size() == spec.cells() &&
                state.pinned[i] > render::terrain::kWaterInfoDry + 1.0f;
            const f32 d = state.depth[i];
            if (d > params.dryThreshold) {
                const f32 t = glm::clamp(std::sqrt(d / 6.0f), 0.0f, 1.0f);
                r = glm::mix(0.15f, 0.02f, t);
                g = glm::mix(0.60f, 0.18f, t);
                b = glm::mix(0.95f, 0.50f, t);
            }
            if (pin) {
                // Pinned reservoirs read MAGENTA — the first thing to
                // check when water appears where it should not.
                r = glm::mix(r, 1.0f, 0.55f);
                g = glm::mix(g, 0.1f, 0.55f);
                b = glm::mix(b, 0.9f, 0.55f);
            }
            const size_t at = (static_cast<size_t>(row) * n + col) * 3;
            pixels[at + 0] = static_cast<u8>(glm::clamp(r, 0.0f, 1.0f) * 255.0f);
            pixels[at + 1] = static_cast<u8>(glm::clamp(g, 0.0f, 1.0f) * 255.0f);
            pixels[at + 2] = static_cast<u8>(glm::clamp(b, 0.0f, 1.0f) * 255.0f);
        }
    }
    if (!stbi_write_png(path.c_str(), n, n, 3, pixels.data(), n * 3)) {
        LOG_ERROR("water-replay: failed to write {}", path);
    } else {
        LOG_INFO("water-replay: wrote {} ({}x{}, {} m/px)", path, n, n,
                 spec.texelSize);
    }
}

void printStats(const char* label, const SimStats& s) {
    LOG_INFO("water-replay [{}]: wet {} | pinned {} | isolated {} | "
             "volume {:.1f} m3 | max depth {:.2f} m | max speed "
             "{:.2f} m/s",
             label, s.wet, s.pinned, s.isolated, s.volume, s.maxDepth,
             s.maxSpeed);
}

} // namespace

int waterReplay(char** argv, int argc) {
    const char* dumpPath = argv[2];
    const std::string prefix = argv[3];
    const u32 substeps =
        argc >= 5 ? static_cast<u32>(std::atoi(argv[4])) : 0;

    WaterSimState state;
    WaterSimParams params;
    vector<render::terraingen::WaterSource> sources;
    if (!render::terrain::loadSimState(dumpPath, state, params,
                                       sources)) {
        return 1;
    }
    LOG_INFO("water-replay: {} — {}x{} at {} m, origin ({:.0f}, "
             "{:.0f}), {} source(s), dt {:.4f}",
             dumpPath, state.spec.n, state.spec.n, state.spec.texelSize,
             state.spec.originX, state.spec.originZ, sources.size(),
             params.dt);
    for (const auto& s : sources) {
        LOG_INFO("water-replay: source ({:.0f}, {:.0f}) {:.2f} m3/s",
                 s.x, s.z, s.discharge);
    }
    printStats("t0", stats(state, params));
    renderMap(state, params, prefix + "-t0.png");
    if (substeps > 0) {
        render::terrain::stepWindow(state, params, sources, substeps);
        const std::string label =
            "t+" + std::to_string(substeps) + " substeps";
        printStats(label.c_str(), stats(state, params));
        renderMap(state, params,
                  prefix + "-t" + std::to_string(substeps) + ".png");
    }
    return 0;
}

} // namespace cooker
