#version 460 core
#include "common.glsl"
#include "firemask.glsl"
#include "prop_encoding.glsl"

// Mirrors tree.vert's placement (yaw, scale, distance fade, canopy sway) so
// shadows match the drawn geometry; lighting-only outputs dropped.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;      // x = sway weight
layout(location = 4) in vec4 aPosScale;
layout(location = 5) in vec4 aParams;

layout(std140, binding = 1) uniform ShadowUbo { mat4 uLightViewProj; };

// Leaf-mask uv for cards; kNoCardUv = not a card. The caster must cut the
// same holes as tree.frag or canopy shadows stay full rectangles.
layout(location = 0) out vec2 vCardUv;

void main() {
    float yaw = aParams.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = vec3(aPos.x * c - aPos.z * s, aPos.y,
                      aPos.x * s + aPos.z * c);

    // Billboard leaf card (see tree.vert): expand toward the LIGHT here —
    // every leaf faces the sun, the canopy casts at full density.
    bool leafCard = isLeafCard(aUv);
    float slot = leafCard ? cardSlot(aUv) : 0.0;
    // Textured props: uv is texture coords, not a sway weight — every
    // textured CASTER is rigid (plants never cast).
    float sway = isTextured(aParams) ? 0.0 : (leafCard ? 0.85 : aUv.x);

    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    // The textured plants never reach this caster — drawDepth skips
    // their families — but the mirror keeps the contract.
    float fadeEnd = propFadeEnd(aParams);
    float fade = 1.0 - smoothstep(fadeEnd * 0.86, fadeEnd, dist);
    // The PEBBLE family (kFamilies: castsShadow = false) casts no
    // shadow: drawDepth already skips its variant slots, so this scale
    // guard only reaches explicit (showcase) instances. Expressed as a
    // scale threshold because the instance stream carries no family:
    // pebbles scatter at 0.06-0.25, every casting family at 0.5 and up.
    fade *= step(0.35, aPosScale.w);
    vec3 world = aPosScale.xyz + local * (aPosScale.w * fade);

    float gust = sin(uWindInfo.x * 1.1 + aParams.z +
                     (aPosScale.x + aPosScale.z * 0.7) * 0.05);
    world.xz += uWindDirInfo.xy * 0.96 *
                (gust * 0.07 * uWindInfo.y * sway * aPosScale.w * fade);

    vCardUv = kNoCardUv;
    // Same leaf-fall rule as tree.vert (winter AND the fire's canopy
    // burn): bare crowns cast bare shadows.
    bool dropped = false;
    if (leafCard) {
        float fall = max(uSeasonInfo.y * uLeafSeason[int(slot)].a,
                         fireCanopyAt(aPosScale.xz));
        if (fall > 0.0) {
            float h = fract(sin(dot(aPosScale.xyz + aPos,
                                    vec3(12.9898, 78.233, 45.164))) *
                            43758.5453);
            dropped = h < fall;
        }
    }
    if (leafCard && !dropped) {
        vec3 lightRight = normalize(vec3(uLightViewProj[0][0],
                                         uLightViewProj[1][0],
                                         uLightViewProj[2][0]));
        vec3 lightUp = normalize(vec3(uLightViewProj[0][1],
                                      uLightViewProj[1][1],
                                      uLightViewProj[2][1]));
        vec2 corner = cardCorner(aUv, slot);
        world += (lightRight * corner.x + lightUp * corner.y) *
                 (aPosScale.w * fade);
        vec2 uv01 = sign(corner) * 0.5 + 0.5; // corners are +-halfSize
        vCardUv = vec2((uv01.x + slot) / kPropCardSlotCount, uv01.y);
    }

    gl_Position = uLightViewProj * vec4(world, 1.0);
}
