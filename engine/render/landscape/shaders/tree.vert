#version 460 core
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;      // x = sway weight, y = height01
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec4 aPosScale; // xyz = terrain point, w = scale
layout(location = 5) in vec4 aParams;   // x = yaw, y = tint, z = sway phase,
                                        // w = fade-end distance (m)

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vTint;
// Leaf-mask uv for cards; x < 0 = not a card (wood/lobes/rocks).
layout(location = 4) out vec2 vCardUv;
// Textured-prop uv (plants: aParams.w < 0 flags the variant, its mesh uv
// is real texture coordinates); x < -5 = untextured.
layout(location = 5) out vec2 vPropUv;
// Bark (procedural trees): xyz = object-space position x scale (the
// triplanar domain — yaw-stable per instance), w = wood flag
// (uv.y < -0.5 on non-card, non-textured vertices).
layout(location = 7) out vec4 vObjPos;
layout(location = 8) out vec3 vObjNormal;
// Height above the instance base — the ground-anchor blend (tree.frag).
layout(location = 6) out float vGroundDelta;

void main() {
    float yaw = aParams.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = vec3(aPos.x * c - aPos.z * s, aPos.y,
                      aPos.x * s + aPos.z * c);
    vec3 normal = vec3(aNormal.x * c - aNormal.z * s, aNormal.y,
                       aNormal.x * s + aNormal.z * c);

    // Billboard leaf card (space-colonization trees): the mesh stores a
    // DEGENERATE quad at the clump center; uv.x < -5 flags it and carries
    // the corner (see appendBillboardCard). The card expands toward the
    // CAMERA here; its lighting normal stays the SDF gradient (rotated
    // with the instance like everything else).
    bool leafCard = aUv.x < -5.0;
    // Atlas slot from the flag bias (-10 - 20*slot, slots 0..7).
    float slot = leafCard ? floor((-aUv.x - 5.0) / 20.0) : 0.0;
    // Textured prop (negative fade lane): uv carries texture coords, so
    // the sway weight comes from the local height instead — base
    // planted, tips ride the gust. A negative sway-phase lane on top
    // marks it RIGID (rocks, stumps: photogrammetry never waves).
    bool texturedProp = aParams.w < 0.0;
    float sway = texturedProp
                     ? (aParams.z < 0.0 ? 0.0
                                        : clamp(aPos.y * 1.2, 0.0, 0.8))
                 : leafCard ? 0.85
                            : aUv.x;

    // Distance fade, per category (|aParams.w|): trees carry to the fog
    // line, rocks and bushes bow out earlier.
    float fadeEnd = abs(aParams.w);
    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    float fade = 1.0 - smoothstep(fadeEnd * 0.86, fadeEnd, dist);
    // Density ramp for the SHORT-REACH textured clutter (the grass
    // prefix idea, per instance): a stable hash key thins the field with
    // distance — near shows everything, far a deterministic subset. This
    // is what makes high near-field densities affordable. Long-reach
    // props (boulders, debris) are structural and never thin.
    if (texturedProp && fadeEnd < 150.0) {
        float thin = smoothstep(fadeEnd * 0.45, fadeEnd * 0.95, dist);
        if (fract(aParams.y * 61.7013) < thin) {
            fade = 0.0;
        }
    }
    vec3 world = aPosScale.xyz + local * (aPosScale.w * fade);

    // Gentle canopy sway: same gust field as the grass, scaled by the
    // per-vertex sway weight (trunk base stays planted).
    float gust = sin(uWindInfo.x * 1.1 + aParams.z +
                     (aPosScale.x + aPosScale.z * 0.7) * 0.05);
    world.xz += vec2(0.9, 0.35) *
                (gust * 0.07 * uWindInfo.y * sway * aPosScale.w * fade);

    vCardUv = vec2(-1.0);
    // Winter leaf fall: a per-card hash against (global fall x the
    // slot's seasonality) collapses the quad — deciduous crowns thin to
    // bare branches, evergreens (seasonality 0) keep every needle. The
    // shadow caster runs the SAME rule (shadow_prop.vert).
    bool dropped = false;
    if (leafCard) {
        float fall = uSeasonInfo.y * uLeafSeason[int(slot)].a;
        if (fall > 0.0) {
            float h = fract(sin(dot(aPosScale.xyz + aPos,
                                    vec3(12.9898, 78.233, 45.164))) *
                            43758.5453);
            dropped = h < fall;
        }
    }
    if (leafCard && !dropped) {
        // Screen-aligned expansion: viewProj rows 0/1 are the camera
        // right/up directions (projection only scales them).
        vec3 camRight = normalize(
            vec3(uViewProj[0][0], uViewProj[1][0], uViewProj[2][0]));
        vec3 camUp = normalize(
            vec3(uViewProj[0][1], uViewProj[1][1], uViewProj[2][1]));
        vec2 corner = vec2(aUv.x + 10.0 + slot * 20.0, aUv.y);
        // Mirror pass: the card re-aims at the mirrored camera, so its
        // winding does not flip like static geometry's under the pass's
        // inverted front face — flip the corners to match (the mirrored
        // mask uv is invisible on a leaf-cluster cutout).
        if (uLeafLodInfo.z > 0.5) {
            corner.x = -corner.x;
        }
        world += (camRight * corner.x + camUp * corner.y) *
                 (aPosScale.w * fade);
        // Corners are exactly +-halfSize: the sign recovers the mask
        // uv; u lands in the card's atlas slot.
        vec2 uv01 = sign(corner) * 0.5 + 0.5;
        vCardUv = vec2((uv01.x + slot) / 8.0, uv01.y);
    }

    vPropUv = texturedProp ? aUv : vec2(-10.0);
    bool bark = !leafCard && !texturedProp && aUv.y < -0.5;
    vObjPos = vec4(aPos * aPosScale.w, bark ? 1.0 : 0.0);
    vObjNormal = aNormal;
    vNormal = normal;
    vColor = aColor;
    vTint = aParams.y;
    vWorldPos = world;
    vGroundDelta = world.y - aPosScale.y;
    gl_Position = uViewProj * vec4(world, 1.0);
}
