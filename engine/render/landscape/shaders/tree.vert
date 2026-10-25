#version 460 core
#include "common.glsl"
#include "firemask.glsl"
#include "prop_encoding.glsl"

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
// Leaf-mask uv for cards; kNoCardUv = not a card (wood/lobes/rocks).
layout(location = 4) out vec2 vCardUv;
// Textured-prop uv (the instance flags the variant, its mesh uv is real
// texture coordinates); kNoPropUv = untextured.
layout(location = 5) out vec2 vPropUv;
// Bark (procedural trees): xyz = object-space position x scale (the
// triplanar domain — yaw-stable per instance), w = the bark lane
// (prop_encoding.glsl; flagged wood on non-card, non-textured vertices).
layout(location = 7) out vec4 vObjPos;
layout(location = 8) out vec3 vObjNormal;
// Height above the instance base — the ground-anchor blend (tree.frag).
layout(location = 6) out float vGroundDelta;
layout(location = 9) out float vBurn; // the fire's canopy burn at this tree

void main() {
    float yaw = aParams.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = vec3(aPos.x * c - aPos.z * s, aPos.y,
                      aPos.x * s + aPos.z * c);
    vec3 normal = vec3(aNormal.x * c - aNormal.z * s, aNormal.y,
                       aNormal.x * s + aNormal.z * c);

    // Billboard leaf card (space-colonization trees): the mesh stores a
    // DEGENERATE quad at the clump center; the uv flag carries the
    // corner and the atlas slot (prop_encoding.glsl, appendBillboardCard).
    // The card expands toward the CAMERA here; its lighting normal stays
    // the SDF gradient (rotated with the instance like everything else).
    bool leafCard = isLeafCard(aUv);
    float slot = leafCard ? cardSlot(aUv) : 0.0;
    // Textured prop: uv carries texture coords, so the sway weight comes
    // from the local height instead — base planted, tips ride the gust.
    // A RIGID prop (rocks, stumps: photogrammetry never waves) stays put.
    bool texturedProp = isTextured(aParams);
    float sway = texturedProp
                     ? (isRigid(aParams) ? 0.0
                                         : clamp(aPos.y * 1.2, 0.0, 0.8))
                 : leafCard ? 0.85
                            : aUv.x;

    // Distance fade, per category: trees carry to the fog line, rocks
    // and bushes bow out earlier.
    float fadeEnd = propFadeEnd(aParams);
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
    world.xz += uWindDirInfo.xy * 0.96 *
                (gust * 0.07 * uWindInfo.y * sway * aPosScale.w * fade);

    vCardUv = kNoCardUv;
    // Winter leaf fall: a per-card hash against (global fall x the
    // slot's seasonality) collapses the quad — deciduous crowns thin to
    // bare branches, evergreens (seasonality 0) keep every needle. The
    // shadow caster runs the SAME rule (shadow_prop.vert).
    // The fire's own leaf fall (chantier ESPRITS E3.e): a burnt tree's
    // canopy goes progressively, evergreen or not, and returns as the
    // cell regrows — the same collapse rule as winter.
    float burn = fireCanopyAt(aPosScale.xz);
    bool dropped = false;
    if (leafCard) {
        float fall = max(uSeasonInfo.y * uLeafSeason[int(slot)].a, burn);
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
        vec2 corner = cardCorner(aUv, slot);
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
        vCardUv = vec2((uv01.x + slot) / kPropCardSlotCount, uv01.y);
    }

    vPropUv = texturedProp ? aUv : kNoPropUv;
    // The bark lane packs flag + yaw — the frag needs the instance yaw
    // to move between world and object space for the triplanar
    // normal/parallax.
    bool bark = !leafCard && !texturedProp && isBark(aUv);
    vObjPos = vec4(aPos * aPosScale.w, barkLane(bark, yaw));
    vObjNormal = aNormal;
    vNormal = normal;
    vColor = aColor;
    vTint = aParams.y;
    vWorldPos = world;
    vGroundDelta = world.y - aPosScale.y;
    vBurn = burn;
    gl_Position = uViewProj * vec4(world, 1.0);
}
