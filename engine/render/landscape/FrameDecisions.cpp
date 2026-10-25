#include "engine/render/landscape/FrameDecisions.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace render {

vector<KeyShadowPick> pickKeyShadows(const vector<SceneLight>& shadowLights,
                                     const Vec3& cameraPosition,
                                     const Vec3& shadowSunDirection,
                                     u32 maxSlots) {
    struct KeyCandidate {
        f32 score;
        Vec3 anchor;
        Vec3 position;
        Vec3 direction;
        f32 fov;
        f32 radius;
    };
    vector<KeyCandidate> keyCandidates;
    const f32 keySunGate =
        glm::smoothstep(0.05f, 0.20f, shadowSunDirection.y);
    for (const SceneLight& light : shadowLights) {
        Vec3 position = light.position;
        Vec3 direction = light.direction;
        if (light.sunLinked) {
            // Same anchor model as the lights UBO: the shadow camera
            // sits OUTSIDE, films through the window along the live
            // beam — the aperture clips the pool for real. A dead
            // beam (night, sun behind the wall) frees its slot.
            direction = -shadowSunDirection;
            const f32 facing =
                glm::dot(direction, glm::normalize(light.direction));
            if (keySunGate * glm::smoothstep(0.15f, 0.40f, facing) <=
                0.05f) {
                continue;
            }
            position += shadowSunDirection * 3.5f;
        } else if (light.spotAngle <= 0.0f) {
            // POINT lights film straight DOWN: one cone cannot
            // cover the sphere, and the ground disc under the lamp
            // is where cast shadows read (an unrotated light's
            // default +Z cone missed everything beside it — "the
            // meshes by the door cast nothing"). Wall shadows above
            // the lamp's own height are the accepted loss.
            direction = Vec3 { 0.0f, -1.0f, 0.0f };
        }
        const Vec3 d = light.position - cameraPosition;
        keyCandidates.push_back(
            { light.intensity / (1.0f + glm::dot(d, d)), light.position,
              position, direction,
              light.spotAngle > 0.0f
                  ? glm::min(light.spotAngle * 1.3f, 150.0f)
                  : 150.0f,
              light.radius });
    }
    std::stable_sort(keyCandidates.begin(), keyCandidates.end(),
                     [](const KeyCandidate& a, const KeyCandidate& b) {
                         return a.score > b.score;
                     });
    vector<KeyShadowPick> picks;
    const size_t slots = glm::min<size_t>(keyCandidates.size(), maxSlots);
    for (size_t slot = 0; slot < slots; ++slot) {
        const KeyCandidate& pick = keyCandidates[slot];
        const Vec3 up = std::abs(pick.direction.y) > 0.95f
                            ? Vec3 { 1.0f, 0.0f, 0.0f }
                            : Vec3 { 0.0f, 1.0f, 0.0f };
        const Mat4 lightView =
            glm::lookAt(pick.position, pick.position + pick.direction, up);
        const Mat4 proj =
            glm::perspective(glm::radians(pick.fov), 1.0f, 0.05f, pick.radius);
        picks.push_back({ pick.anchor, proj * lightView });
    }
    return picks;
}

LightsUniforms fillLightsUbo(const LightsInputs& in) {
    LightsUniforms lights;
    // H2 (docs/RENDERING.md): sun-linked sources take the SUN's
    // live color — hour and weather included — and die below the
    // horizon. WINDOW projectors (docs/RENDERING.md) keep the anchor
    // and carry the window's NORMAL + extents; the facing gate
    // lives in the shader (beam · normal). Plain sun-linked spots
    // keep the pushed-cone model. rcOnly lights skip the direct
    // path entirely — their GI blob carries them.
    const f32 sunGate = glm::smoothstep(0.05f, 0.20f, in.shadowSunDirection.y);
    u32 slot = 0;
    for (u32 i = 0; i < in.lights.size() && slot < in.budget && !in.rcOnly;
         ++i) {
        const SceneLight& light = in.lights[i];
        if (light.rcOnly) {
            continue;
        }
        const bool window = light.windowHalfWidth > 0.0f;
        f32 intensity = light.intensity;
        Vec3 color = light.color;
        Vec3 position = light.position;
        if (light.sunLinked) {
            color = in.sunColor;
            intensity *=
                sunGate * aboveBuried(light.position.y, in.buriedBelowY);
            if (!window) {
                const f32 facing = glm::dot(-in.shadowSunDirection,
                                            glm::normalize(light.direction));
                intensity *= glm::smoothstep(0.15f, 0.40f, facing);
                position += in.shadowSunDirection * 3.5f;
            }
        }
        if (light.flicker > 0.0f) {
            const f32 phase = static_cast<f32>(i) * 1.7f;
            intensity *=
                1.0f + light.flicker *
                           (0.55f * std::sin(in.timeSeconds * 9.0f + phase) +
                            0.45f * std::sin(in.timeSeconds * 23.0f +
                                             phase * 3.1f));
        }
        lights.positionRadius[slot] = { position, light.radius };
        lights.colorIntensity[slot] = { color * intensity, 0.0f };
        const bool spot = light.spotAngle > 0.0f;
        lights.directionAngle[slot] = {
            glm::normalize(window ? light.direction
                           : light.sunLinked ? -in.shadowSunDirection
                                             : light.direction),
            window ? -3.0f
            : spot ? std::cos(glm::radians(light.spotAngle * 0.5f))
                   : -2.0f
        };
        // The key-shadow atlas slot (windowInfo.z, 0 = unshadowed):
        // matched against the pick's anchor — both come from the
        // same Transform, so the positions are bit-equal.
        f32 shadowSlot = 0.0f;
        for (size_t k = 0; k < in.keyShadowPicks.size(); ++k) {
            if (glm::distance(in.keyShadowPicks[k].anchor, light.position) <
                0.05f) {
                shadowSlot = static_cast<f32>(k + 1);
                break;
            }
        }
        lights.windowInfo[slot] = { light.windowHalfWidth,
                                    light.windowHalfHeight, shadowSlot,
                                    0.0f };
        ++slot;
    }
    lights.count.x = static_cast<f32>(slot);
    return lights;
}

bool seaVisibleInFrustum(const vector<TerrainSystem::ChunkAabb>& chunks,
                         f32 seaLevel, const Vec3& cameraPosition,
                         const Frustum& frustum) {
    for (const TerrainSystem::ChunkAabb& aabb : chunks) {
        if (aabb.lo.y >= seaLevel) {
            continue; // fully above the water table
        }
        // Beyond ~600 m the mirror sits deep in the fog (fogStart
        // 450): not worth a full scene re-render (~7 ms measured
        // while no readable water was in sight).
        const Vec2 toChunk { (aabb.lo.x + aabb.hi.x) * 0.5f -
                                 cameraPosition.x,
                             (aabb.lo.z + aabb.hi.z) * 0.5f -
                                 cameraPosition.z };
        if (glm::dot(toChunk, toChunk) > 600.0f * 600.0f) {
            continue;
        }
        // Test the chunk's water RECTANGLE (the sea plane spans it).
        if (frustum.intersectsAabb(
                { aabb.lo.x, seaLevel - 1.0f, aabb.lo.z },
                { aabb.hi.x, seaLevel + 1.0f, aabb.hi.z })) {
            return true;
        }
    }
    return false;
}

} // namespace render
