#include "world/spirit/WaterReading.hpp"

#include <cmath>
#include <deque>

#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterSim.hpp"

namespace world {

namespace {

using render::terrain::kWaterInfoDry;
using render::terrain::WaterSimSnapshot;

// Squared distance from p to the segment ab, and where along it.
f32 segmentDistance2(const Vec2& p, const Vec2& a, const Vec2& b, f32* t) {
    const Vec2 ab = b - a;
    const f32 len2 = glm::dot(ab, ab);
    f32 u = len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f)
                         : 0.0f;
    if (t) {
        *t = u;
    }
    const Vec2 q = a + ab * u;
    return glm::dot(p - q, p - q);
}

// The river covering (x, z), with its local half width; nullptr if none.
const render::RiverSurface* riverAt(const render::WaterBodies& bodies, f32 x,
                                    f32 z, f32* halfWidth) {
    const Vec2 p { x, z };
    for (const render::RiverSurface& river : bodies.rivers) {
        if (x < river.minX || x > river.maxX || z < river.minZ ||
            z > river.maxZ || river.nodes.size() < 2) {
            continue;
        }
        for (size_t i = 0; i + 1 < river.nodes.size(); ++i) {
            const render::RiverNode& a = river.nodes[i];
            const render::RiverNode& b = river.nodes[i + 1];
            f32 t = 0.0f;
            const f32 d2 =
                segmentDistance2(p, { a.x, a.z }, { b.x, b.z }, &t);
            const f32 hw = glm::mix(a.halfWidth, b.halfWidth, t);
            if (d2 <= hw * hw) {
                if (halfWidth) {
                    *halfWidth = hw;
                }
                return &river;
            }
        }
    }
    return nullptr;
}

// Flood the sim's wet cells connected to (col, row); volume in m³ and
// whether the flood stayed inside the trusted window.
void floodVolume(const WaterSimSnapshot& snap, i32 col, i32 row, f32* volume,
                 bool* exact) {
    const i32 n = static_cast<i32>(snap.spec.n);
    const i32 margin = static_cast<i32>(snap.marginCells);
    const f32 cellArea = snap.spec.texelSize * snap.spec.texelSize;
    vector<u8> seen(snap.spec.cells(), 0);
    std::deque<std::pair<i32, i32>> open;
    open.emplace_back(col, row);
    seen[static_cast<size_t>(row) * n + col] = 1;
    f64 sum = 0.0;
    bool inside = true;
    while (!open.empty()) {
        const auto [c, r] = open.front();
        open.pop_front();
        sum += snap.depth[static_cast<size_t>(r) * n + c];
        if (c <= margin || r <= margin || c >= n - 1 - margin ||
            r >= n - 1 - margin) {
            inside = false;
        }
        const i32 next[4][2] = { { c + 1, r }, { c - 1, r }, { c, r + 1 },
                                 { c, r - 1 } };
        for (const auto& [nc, nr] : next) {
            if (nc < 0 || nr < 0 || nc >= n || nr >= n) {
                continue;
            }
            const size_t i = static_cast<size_t>(nr) * n + nc;
            if (seen[i] || snap.depth[i] <= 0.0f) {
                continue;
            }
            seen[i] = 1;
            open.emplace_back(nc, nr);
        }
    }
    *volume = static_cast<f32>(sum) * cellArea;
    *exact = inside;
}

// Lake level over the baked basin, subsampled so a 5 km lake costs a
// few thousand height taps.
f32 lakeVolumeEstimate(const render::LakeSurface& lake,
                       const GroundHeightFn& ground) {
    if (!ground) {
        return 0.0f;
    }
    const f32 w = lake.maxX - lake.minX;
    const f32 h = lake.maxZ - lake.minZ;
    if (w <= 0.0f || h <= 0.0f) {
        return 0.0f;
    }
    // A grid of taps tiling the bounds exactly (ceil, then the tap pitch
    // is bounds / count), thinned so the whole lake costs <= ~4096 taps.
    const f32 texel = glm::max(lake.maskTexel, 1.0f);
    const u32 nx = glm::max(1u, static_cast<u32>(std::ceil(w / texel)));
    const u32 nz = glm::max(1u, static_cast<u32>(std::ceil(h / texel)));
    const u32 stride = glm::max(
        1u, static_cast<u32>(std::ceil(std::sqrt(
                static_cast<f32>(nx) * static_cast<f32>(nz) / 4096.0f))));
    const f32 pitchX = w / static_cast<f32>(nx);
    const f32 pitchZ = h / static_cast<f32>(nz);
    f64 sum = 0.0;
    for (u32 iz = 0; iz < nz; iz += stride) {
        for (u32 ix = 0; ix < nx; ix += stride) {
            const f32 x = lake.minX + (static_cast<f32>(ix) + 0.5f) * pitchX;
            const f32 z = lake.minZ + (static_cast<f32>(iz) + 0.5f) * pitchZ;
            if (!lake.covers(x, z)) {
                continue;
            }
            sum += glm::max(0.0f, lake.level - ground(x, z));
        }
    }
    const f32 sampleArea = pitchX * pitchZ * static_cast<f32>(stride * stride);
    return static_cast<f32>(sum) * sampleArea;
}

} // namespace

WaterReading readWater(const WaterReadingInputs& in, f32 x, f32 z,
                       f32 groundY) {
    WaterReading out;
    if (!in.query) {
        return out;
    }
    const render::terrain::WaterQuery& q = *in.query;
    const std::optional<f32> surface =
        render::terrain::waterSurfaceQuery(q, x, z, groundY);
    out.water = surface && *surface - groundY > 0.02f;
    if (out.water) {
        out.depth = *surface - groundY;
        out.flow = render::terrain::waterFlowQuery(q, x, z, groundY);
    }

    // What body: a spirit's spring first (it explains the water), then
    // the baked bodies, then the sea, then plain standing sim water.
    if (in.sources) {
        for (const auto& e : in.sources->entries()) {
            if (e.source.kind != render::terrain::SpiritKind::Water ||
                (in.worldspace.isValid() && e.worldspace != in.worldspace)) {
                continue;
            }
            const f32 reach = glm::max(e.source.radius, 4.0f);
            const f32 dx = e.source.x - x;
            const f32 dz = e.source.z - z;
            if (dx * dx + dz * dz <= reach * reach) {
                out.body = WaterReading::Body::Spirit;
                out.spiritRemaining = e.source.remaining;
                break;
            }
        }
    }
    const render::LakeSurface* lake = nullptr;
    if (out.body == WaterReading::Body::None && q.bodies) {
        for (const render::LakeSurface& candidate : q.bodies->lakes) {
            if (candidate.covers(x, z) && candidate.level > groundY) {
                lake = &candidate;
                out.body = WaterReading::Body::Lake;
                out.lakeLevel = candidate.level;
                out.lakeArea = (candidate.maxX - candidate.minX) *
                               (candidate.maxZ - candidate.minZ);
                if (!candidate.mask.empty()) {
                    u32 wet = 0;
                    for (const u8 m : candidate.mask) {
                        wet += m != 0 ? 1u : 0u;
                    }
                    out.lakeArea = static_cast<f32>(wet) *
                                   candidate.maskTexel * candidate.maskTexel;
                }
                break;
            }
        }
        if (out.body == WaterReading::Body::None) {
            f32 halfWidth = 0.0f;
            if (const render::RiverSurface* river =
                    riverAt(*q.bodies, x, z, &halfWidth)) {
                out.body = WaterReading::Body::River;
                out.riverWidth = halfWidth * 2.0f;
                const f32 speed = glm::length(out.flow) > 0.01f
                                      ? glm::length(out.flow)
                                      : river->flowSpeed;
                out.riverDischarge = out.riverWidth * out.depth * speed;
            }
        }
    }
    if (out.body == WaterReading::Body::None && out.water &&
        groundY < q.seaLevel && surface &&
        std::abs(*surface - q.seaLevel) < 0.5f) {
        out.body = WaterReading::Body::Sea;
    }
    if (out.body == WaterReading::Body::None && out.water) {
        out.body = WaterReading::Body::Pool;
    }

    // Volume: the sim's connected wet region when the aim sits in it.
    if (out.water && q.sim && !q.sim->depth.empty()) {
        const WaterSimSnapshot& snap = *q.sim;
        const f32 texel = snap.spec.texelSize;
        const i32 col = static_cast<i32>(std::lround((x - snap.spec.originX) / texel));
        const i32 row = static_cast<i32>(std::lround((z - snap.spec.originZ) / texel));
        const i32 n = static_cast<i32>(snap.spec.n);
        if (col >= 0 && row >= 0 && col < n && row < n &&
            snap.depth[static_cast<size_t>(row) * n + col] > 0.0f) {
            floodVolume(snap, col, row, &out.volume, &out.volumeExact);
            out.volumeKnown = true;
        }
    }
    if (lake && !(out.volumeKnown && out.volumeExact)) {
        const f32 estimate = lakeVolumeEstimate(*lake, in.ground);
        if (estimate > 0.0f) {
            out.volume = estimate;
            out.volumeKnown = true;
            out.volumeExact = false;
        }
    }
    if (out.body == WaterReading::Body::Sea) {
        out.volumeKnown = false; // the sea has no volume worth a number
    }

    // A dry aim: the nearest water within the search radius — the sim's
    // wet cells, the baked lakes (by bounds) and rivers (by node).
    if (!out.water) {
        f32 best2 = in.searchRadius * in.searchRadius;
        Vec2 bestAt { 0.0f };
        bool found = false;
        const auto consider = [&](f32 px, f32 pz) {
            const f32 dx = px - x;
            const f32 dz = pz - z;
            const f32 d2 = dx * dx + dz * dz;
            if (d2 < best2 && d2 > 1e-4f) {
                best2 = d2;
                bestAt = { px, pz };
                found = true;
            }
        };
        if (q.sim && !q.sim->depth.empty()) {
            const WaterSimSnapshot& snap = *q.sim;
            const i32 n = static_cast<i32>(snap.spec.n);
            for (i32 r = 0; r < n; ++r) {
                for (i32 c = 0; c < n; ++c) {
                    if (snap.depth[static_cast<size_t>(r) * n + c] > 0.02f) {
                        consider(snap.spec.originX +
                                     static_cast<f32>(c) * snap.spec.texelSize,
                                 snap.spec.originZ +
                                     static_cast<f32>(r) * snap.spec.texelSize);
                    }
                }
            }
        }
        if (q.bodies) {
            for (const render::LakeSurface& l : q.bodies->lakes) {
                consider(glm::clamp(x, l.minX, l.maxX),
                         glm::clamp(z, l.minZ, l.maxZ));
            }
            for (const render::RiverSurface& river : q.bodies->rivers) {
                for (const render::RiverNode& node : river.nodes) {
                    consider(node.x, node.z);
                }
            }
        }
        if (found) {
            out.nearestFound = true;
            out.nearestDistance = std::sqrt(best2);
            out.nearestDir = glm::normalize(bestAt - Vec2 { x, z });
        }
    }
    return out;
}

const char* compassCode(const Vec2& dir) {
    if (glm::dot(dir, dir) < 1e-8f) {
        return "N";
    }
    // x east, -z north: angle from east, counter-clockwise seen from
    // above, in eighths.
    const f32 angle = std::atan2(-dir.y, dir.x); // -pi..pi, 0 = east
    const i32 eighth =
        (static_cast<i32>(std::lround(angle / (3.1415927f / 4.0f))) + 8) % 8;
    static const char* const codes[8] = { "E",  "NE", "N", "NW",
                                          "W",  "SW", "S", "SE" };
    return codes[eighth];
}

} // namespace world
