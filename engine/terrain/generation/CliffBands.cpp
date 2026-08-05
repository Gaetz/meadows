#include "engine/terrain/generation/CliffBands.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace render::terraingen {

namespace {

// Bilinear height on the fine grid, clamped to the border.
f32 sampleH(const GridSpec& spec, const vector<f32>& heights, f32 x,
            f32 z) {
    const f32 fx = glm::clamp((x - spec.originX) / spec.texelSize, 0.0f,
                              static_cast<f32>(spec.n - 1));
    const f32 fz = glm::clamp((z - spec.originZ) / spec.texelSize, 0.0f,
                              static_cast<f32>(spec.n - 1));
    const u32 ix = glm::min(static_cast<u32>(fx), spec.n - 2);
    const u32 iz = glm::min(static_cast<u32>(fz), spec.n - 2);
    const f32 tx = fx - static_cast<f32>(ix);
    const f32 tz = fz - static_cast<f32>(iz);
    const size_t row = static_cast<size_t>(iz) * spec.n + ix;
    const f32 a = glm::mix(heights[row], heights[row + 1], tx);
    const f32 b =
        glm::mix(heights[row + spec.n], heights[row + spec.n + 1], tx);
    return glm::mix(a, b, tz);
}

// Height gradient (central differences over the detect stride).
Vec2 gradAt(const GridSpec& spec, const vector<f32>& heights, f32 x,
            f32 z, f32 stride) {
    const f32 gx = (sampleH(spec, heights, x + stride, z) -
                    sampleH(spec, heights, x - stride, z)) /
                   (2.0f * stride);
    const f32 gz = (sampleH(spec, heights, x, z + stride) -
                    sampleH(spec, heights, x, z - stride)) /
                   (2.0f * stride);
    return { gx, gz };
}

f32 wetnessAt(const GridSpec& spec, const vector<u8>& wetness, f32 x,
              f32 z) {
    if (wetness.empty() || spec.n < 2) {
        return 0.0f;
    }
    const f32 fx = glm::clamp((x - spec.originX) / spec.texelSize, 0.0f,
                              static_cast<f32>(spec.n - 1));
    const f32 fz = glm::clamp((z - spec.originZ) / spec.texelSize, 0.0f,
                              static_cast<f32>(spec.n - 1));
    const u32 ix = glm::min(static_cast<u32>(fx), spec.n - 2);
    const u32 iz = glm::min(static_cast<u32>(fz), spec.n - 2);
    const size_t row = static_cast<size_t>(iz) * spec.n + ix;
    return static_cast<f32>(wetness[row]) / 255.0f;
}

// Fall-line probe from a point: walk down and up the gradient while the
// ground stays steep; fills the local foot/head of the wall this point
// sits on. Steps at half the detect stride, bounded run.
struct WallProbe {
    f32 footX, footZ, footH;
    f32 headX, headZ, headH;
};
WallProbe probeWall(const GridSpec& spec, const vector<f32>& heights,
                    f32 x, f32 z, f32 stride, f32 slopeGrad) {
    const f32 step = stride * 0.5f;
    const f32 keepGrad = slopeGrad * 0.65f;
    const f32 h = sampleH(spec, heights, x, z);
    WallProbe probe { x, z, h, x, z, h };
    f32 cx = x;
    f32 cz = z;
    for (u32 i = 0; i < 16; ++i) {
        const Vec2 g = gradAt(spec, heights, cx, cz, stride);
        const f32 len = glm::length(g);
        if (len < keepGrad) {
            break;
        }
        cx -= g.x / len * step; // downhill = against the gradient
        cz -= g.y / len * step;
        probe.footX = cx;
        probe.footZ = cz;
        probe.footH = sampleH(spec, heights, cx, cz);
    }
    cx = x;
    cz = z;
    for (u32 i = 0; i < 16; ++i) {
        const Vec2 g = gradAt(spec, heights, cx, cz, stride);
        const f32 len = glm::length(g);
        if (len < keepGrad) {
            break;
        }
        cx += g.x / len * step;
        cz += g.y / len * step;
        probe.headX = cx;
        probe.headZ = cz;
        probe.headH = sampleH(spec, heights, cx, cz);
    }
    return probe;
}

} // namespace

vector<render::CliffBand> extractCliffBands(
    const GridSpec& fineSpec, vector<f32>& heights,
    const GridSpec& coarseSpec, const vector<u8>& wetness,
    const CliffBandParams& params, f32 clipMinX, f32 clipMinZ,
    f32 clipSpan) {
    vector<render::CliffBand> bands;
    if (fineSpec.n < 4 || heights.size() != fineSpec.cells()) {
        return bands;
    }
    const f32 stride = glm::max(params.detectStride, fineSpec.texelSize);
    const f32 span =
        static_cast<f32>(fineSpec.n - 1) * fineSpec.texelSize;
    const i32 detectN = static_cast<i32>(span / stride) - 1;
    if (detectN < 4) {
        return bands;
    }
    // One inset detect ring: gradients never read past the border.
    const auto detectPos = [&](i32 cx, i32 cz) {
        return Vec2 { fineSpec.originX +
                          (static_cast<f32>(cx) + 1.0f) * stride,
                      fineSpec.originZ +
                          (static_cast<f32>(cz) + 1.0f) * stride };
    };

    // 1) Steep mask at the detect stride (water-gated).
    vector<u8> steep(static_cast<size_t>(detectN) * detectN, 0);
    for (i32 cz = 0; cz < detectN; ++cz) {
        for (i32 cx = 0; cx < detectN; ++cx) {
            const Vec2 p = detectPos(cx, cz);
            if (glm::length(gradAt(fineSpec, heights, p.x, p.y,
                                   stride)) < params.slopeGrad) {
                continue;
            }
            if (wetnessAt(coarseSpec, wetness, p.x, p.y) >
                params.wetnessGate) {
                continue;
            }
            steep[static_cast<size_t>(cz) * detectN + cx] = 1;
        }
    }

    // 2) Connected components (8-way BFS), area-gated.
    vector<i32> label(steep.size(), -1);
    vector<vector<u32>> components;
    vector<u32> stack;
    for (size_t seed = 0; seed < steep.size(); ++seed) {
        if (steep[seed] == 0 || label[seed] >= 0) {
            continue;
        }
        const i32 id = static_cast<i32>(components.size());
        components.emplace_back();
        stack.assign(1, static_cast<u32>(seed));
        label[seed] = id;
        while (!stack.empty()) {
            const u32 cell = stack.back();
            stack.pop_back();
            components[static_cast<size_t>(id)].push_back(cell);
            const i32 cx = static_cast<i32>(cell) % detectN;
            const i32 cz = static_cast<i32>(cell) / detectN;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 nx = cx + dx;
                    const i32 nz = cz + dz;
                    if (nx < 0 || nz < 0 || nx >= detectN ||
                        nz >= detectN) {
                        continue;
                    }
                    const size_t idx =
                        static_cast<size_t>(nz) * detectN + nx;
                    if (steep[idx] != 0 && label[idx] < 0) {
                        label[idx] = id;
                        stack.push_back(static_cast<u32>(idx));
                    }
                }
            }
        }
        if (components.back().size() < params.minCells) {
            components.pop_back(); // labels of dropped comps stay set:
                                   // they only gate re-visits
        }
    }

    // 3) Sharpen: per steep cell of a KEPT component, remap the height
    // between the local foot/head (logistic curve), write the delta at
    // detect resolution, then add it back bilinearly on the fine grid.
    // Detect-resolution deltas keep the fine relief riding on top.
    if (params.sharpen > 0.0f && !components.empty()) {
        vector<f32> delta(steep.size(), 0.0f);
        vector<u8> inKept(steep.size(), 0);
        for (const auto& component : components) {
            for (const u32 cell : component) {
                inKept[cell] = 1;
            }
        }
        const f32 k = params.sharpenExponent;
        for (size_t cell = 0; cell < steep.size(); ++cell) {
            if (inKept[cell] == 0) {
                continue;
            }
            const i32 cx = static_cast<i32>(cell) % detectN;
            const i32 cz = static_cast<i32>(cell) / detectN;
            const Vec2 p = detectPos(cx, cz);
            const WallProbe probe = probeWall(
                fineSpec, heights, p.x, p.y, stride, params.slopeGrad);
            const f32 drop = probe.headH - probe.footH;
            if (drop < params.minHeight) {
                continue;
            }
            const f32 h = sampleH(fineSpec, heights, p.x, p.y);
            const f32 t = glm::clamp((h - probe.footH) / drop, 0.0f,
                                     1.0f);
            const f32 tk = std::pow(t, k);
            const f32 tRemap =
                tk / glm::max(tk + std::pow(1.0f - t, k), 1.0e-6f);
            delta[cell] = (probe.footH + tRemap * drop - h) *
                          params.sharpen;
        }
        // Feather: average-of-neighbours smoothing so band edges blend
        // (one pass at detect resolution ≈ an 8 m blur).
        vector<f32> smoothed(delta.size(), 0.0f);
        for (i32 cz = 0; cz < detectN; ++cz) {
            for (i32 cx = 0; cx < detectN; ++cx) {
                f32 sum = 0.0f;
                f32 weight = 0.0f;
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = cx + dx;
                        const i32 nz = cz + dz;
                        if (nx < 0 || nz < 0 || nx >= detectN ||
                            nz >= detectN) {
                            continue;
                        }
                        const f32 w =
                            (dx == 0 && dz == 0) ? 2.0f : 1.0f;
                        sum += delta[static_cast<size_t>(nz) * detectN +
                                     nx] *
                               w;
                        weight += w;
                    }
                }
                smoothed[static_cast<size_t>(cz) * detectN + cx] =
                    sum / weight;
            }
        }
        for (u32 row = 0; row < fineSpec.n; ++row) {
            const f32 wz = fineSpec.z(row);
            const f32 fz = (wz - fineSpec.originZ) / stride - 1.0f;
            if (fz < 0.0f || fz > static_cast<f32>(detectN - 1)) {
                continue;
            }
            const u32 iz =
                glm::min(static_cast<u32>(fz),
                         static_cast<u32>(detectN - 2));
            const f32 tz = fz - static_cast<f32>(iz);
            for (u32 col = 0; col < fineSpec.n; ++col) {
                const f32 wx = fineSpec.x(col);
                const f32 fx = (wx - fineSpec.originX) / stride - 1.0f;
                if (fx < 0.0f || fx > static_cast<f32>(detectN - 1)) {
                    continue;
                }
                const u32 ix =
                    glm::min(static_cast<u32>(fx),
                             static_cast<u32>(detectN - 2));
                const f32 tx = fx - static_cast<f32>(ix);
                const size_t base =
                    static_cast<size_t>(iz) * detectN + ix;
                const f32 a = glm::mix(smoothed[base],
                                       smoothed[base + 1], tx);
                const f32 b = glm::mix(
                    smoothed[base + detectN],
                    smoothed[base + detectN + 1], tx);
                heights[static_cast<size_t>(row) * fineSpec.n + col] +=
                    glm::mix(a, b, tz);
            }
        }
    }

    // 4) Foot polylines on the SHARPENED grid: per kept component,
    // collect foot nodes (steep cell whose downhill neighbour left the
    // component), refine each by the fall-line probe, chain them by
    // nearest-neighbour walks. Clipped to the owning tile rect.
    const f32 clipMaxX = clipMinX + clipSpan;
    const f32 clipMaxZ = clipMinZ + clipSpan;
    for (const auto& component : components) {
        vector<render::CliffNode> nodes;
        for (const u32 cell : component) {
            const i32 cx = static_cast<i32>(cell) % detectN;
            const i32 cz = static_cast<i32>(cell) / detectN;
            const Vec2 p = detectPos(cx, cz);
            const Vec2 g = gradAt(fineSpec, heights, p.x, p.y, stride);
            const f32 len = glm::length(g);
            if (len < 1.0e-4f) {
                continue;
            }
            // Foot cell: at least one 4-neighbour is BOTH flatter than
            // the threshold and lower — the wall's talus side. (The
            // gradient-direction test alone flags interior zigzags.)
            const f32 hCell = sampleH(fineSpec, heights, p.x, p.y);
            bool foot = false;
            const i32 offs[4][2] = {
                { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
            };
            for (const auto& off : offs) {
                const i32 nx = cx + off[0];
                const i32 nz = cz + off[1];
                if (nx < 0 || nz < 0 || nx >= detectN || nz >= detectN) {
                    continue;
                }
                if (steep[static_cast<size_t>(nz) * detectN + nx] != 0) {
                    continue;
                }
                const Vec2 np = detectPos(nx, nz);
                if (sampleH(fineSpec, heights, np.x, np.y) <
                    hCell - 0.5f) {
                    foot = true;
                    break;
                }
            }
            if (!foot) {
                continue;
            }
            const WallProbe probe = probeWall(
                fineSpec, heights, p.x, p.y, stride, params.slopeGrad);
            if (probe.headH - probe.footH < params.minHeight) {
                continue;
            }
            if (probe.footX < clipMinX || probe.footX >= clipMaxX ||
                probe.footZ < clipMinZ || probe.footZ >= clipMaxZ) {
                continue;
            }
            // Spatial thinning: probes converge — a node under 0.6
            // strides from a kept one is the same foot point.
            bool duplicate = false;
            for (const render::CliffNode& kept : nodes) {
                const f32 dx = kept.x - probe.footX;
                const f32 dz = kept.z - probe.footZ;
                if (dx * dx + dz * dz <
                    stride * stride * 0.36f) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            nodes.push_back({ probe.footX, probe.footZ, probe.footH,
                              probe.headX, probe.headZ, probe.headH,
                              -g.x / len, -g.y / len });
        }
        // Chain into polylines: greedy nearest-neighbour from the
        // lowest-(x+z) unused node; a gap over 2.5 strides starts a new
        // band (branchy components split naturally).
        vector<u8> used(nodes.size(), 0);
        const f32 maxLink = stride * 3.5f;
        for (;;) {
            size_t start = nodes.size();
            f32 best = 0.0f;
            for (size_t i = 0; i < nodes.size(); ++i) {
                const f32 score = nodes[i].x + nodes[i].z;
                if (used[i] == 0 &&
                    (start == nodes.size() || score < best)) {
                    start = i;
                    best = score;
                }
            }
            if (start == nodes.size()) {
                break;
            }
            render::CliffBand band;
            band.nodes.push_back(nodes[start]);
            used[start] = 1;
            for (;;) {
                const render::CliffNode& tail = band.nodes.back();
                size_t next = nodes.size();
                f32 bestDist = maxLink * maxLink;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (used[i] != 0) {
                        continue;
                    }
                    const f32 dx = nodes[i].x - tail.x;
                    const f32 dz = nodes[i].z - tail.z;
                    const f32 d2 = dx * dx + dz * dz;
                    if (d2 < bestDist) {
                        bestDist = d2;
                        next = i;
                    }
                }
                if (next == nodes.size()) {
                    break;
                }
                band.nodes.push_back(nodes[next]);
                used[next] = 1;
            }
            if (band.nodes.size() >= 2) {
                bands.push_back(std::move(band));
            }
        }
    }
    return bands;
}

} // namespace render::terraingen
