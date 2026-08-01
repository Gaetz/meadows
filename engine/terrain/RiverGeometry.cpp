#include "engine/terrain/RiverGeometry.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/TerrainBase.hpp" // terrain::catmullRom

namespace render::terrain {

namespace {

Vec2 tangent(const RiverNode& behind, const RiverNode& ahead) {
    const Vec2 d { ahead.x - behind.x, ahead.z - behind.z };
    const f32 len = glm::length(d);
    return len > 1.0e-5f ? d / len : Vec2 { 1.0f, 0.0f };
}

f32 turnBetween(const Vec2& a, const Vec2& b) {
    return std::abs(
        std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b)));
}

} // namespace

vector<RiverNode> subdivideRiverNodes(const vector<RiverNode>& nodes,
                                      f32 maxAngle, f32 minStep,
                                      u32 maxInserts) {
    if (nodes.size() < 3) {
        return nodes;
    }
    const auto at = [&](size_t i) -> const RiverNode& {
        return nodes[glm::min(i, nodes.size() - 1)];
    };
    vector<RiverNode> out;
    out.reserve(nodes.size() * 2);
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        const RiverNode& p0 = nodes[i > 0 ? i - 1 : 0];
        const RiverNode& p1 = nodes[i];
        const RiverNode& p2 = nodes[i + 1];
        const RiverNode& p3 = at(i + 2);
        out.push_back(p1);
        const f32 turn =
            turnBetween(tangent(p0, p2), tangent(p1, p3));
        if (turn <= maxAngle) {
            continue; // straight enough: emit as-is
        }
        const f32 segLen = std::hypot(p2.x - p1.x, p2.z - p1.z);
        u32 inserts = glm::min(
            static_cast<u32>(std::ceil(turn / glm::max(maxAngle,
                                                       0.01f))) -
                1,
            maxInserts);
        // Never densify below minStep.
        if (segLen / static_cast<f32>(inserts + 1) < minStep) {
            inserts = segLen > minStep
                          ? static_cast<u32>(segLen / minStep) - 1
                          : 0;
        }
        for (u32 k = 1; k <= inserts; ++k) {
            const f32 t =
                static_cast<f32>(k) / static_cast<f32>(inserts + 1);
            RiverNode n;
            n.x = catmullRom(p0.x, p1.x, p2.x, p3.x, t);
            n.z = catmullRom(p0.z, p1.z, p2.z, p3.z, t);
            n.surface = glm::mix(p1.surface, p2.surface, t);
            n.halfWidth = glm::mix(p1.halfWidth, p2.halfWidth, t);
            out.push_back(n);
        }
    }
    out.push_back(nodes.back());
    // Inner-bank fold guard: at a bend of radius r, a ribbon wider than
    // r folds over itself — clamp the half width under the local turn
    // radius.
    for (size_t i = 1; i + 1 < out.size(); ++i) {
        const Vec2 tIn = tangent(out[i - 1], out[i]);
        const Vec2 tOut = tangent(out[i], out[i + 1]);
        const f32 turn = turnBetween(tIn, tOut);
        if (turn < 1.0e-3f) {
            continue;
        }
        const f32 lenIn =
            std::hypot(out[i].x - out[i - 1].x, out[i].z - out[i - 1].z);
        const f32 lenOut =
            std::hypot(out[i + 1].x - out[i].x, out[i + 1].z - out[i].z);
        const f32 radius = 0.5f * (lenIn + lenOut) / turn;
        out[i].halfWidth = glm::min(out[i].halfWidth, 0.9f * radius);
    }
    return out;
}

} // namespace render::terrain
