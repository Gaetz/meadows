#version 460 core
#include "common.glsl"

// Mirrors tree.vert's placement (yaw, scale, distance fade, canopy sway) so
// shadows match the drawn geometry; lighting-only outputs dropped.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;      // x = sway weight
layout(location = 4) in vec4 aPosScale;
layout(location = 5) in vec4 aParams;

layout(std140, binding = 1) uniform ShadowUbo { mat4 uLightViewProj; };

// Leaf-mask uv for cards; x < 0 = not a card. The caster must cut the
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
    bool leafCard = aUv.x < -5.0;
    float slot = leafCard ? floor((-aUv.x - 5.0) / 20.0) : 0.0;
    // Textured props (aParams.w < 0): uv is texture coords, not a sway
    // weight — every textured CASTER is rigid (plants never cast).
    float sway = aParams.w < 0.0 ? 0.0 : (leafCard ? 0.85 : aUv.x);

    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    // |w|: a negative lane flags textured plants (they never reach this
    // caster — drawDepth skips them — but the mirror keeps the contract).
    float fadeEnd = abs(aParams.w);
    float fade = 1.0 - smoothstep(fadeEnd * 0.86, fadeEnd, dist);
    // Pebble-scale clutter (shares the rock variants) casts no shadow:
    // collapse it here — its shadow is invisible but its 700+ tri scan
    // mesh is not free across the cascades. Same threshold as the
    // collision skip (VegetationCollision).
    fade *= step(0.35, aPosScale.w);
    vec3 world = aPosScale.xyz + local * (aPosScale.w * fade);

    float gust = sin(uWindInfo.x * 1.1 + aParams.z +
                     (aPosScale.x + aPosScale.z * 0.7) * 0.05);
    world.xz += vec2(0.9, 0.35) *
                (gust * 0.07 * uWindInfo.y * sway * aPosScale.w * fade);

    vCardUv = vec2(-1.0);
    // Same leaf-fall rule as tree.vert: bare crowns cast bare shadows.
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
        vec3 lightRight = normalize(vec3(uLightViewProj[0][0],
                                         uLightViewProj[1][0],
                                         uLightViewProj[2][0]));
        vec3 lightUp = normalize(vec3(uLightViewProj[0][1],
                                      uLightViewProj[1][1],
                                      uLightViewProj[2][1]));
        vec2 corner = vec2(aUv.x + 10.0 + slot * 20.0, aUv.y);
        world += (lightRight * corner.x + lightUp * corner.y) *
                 (aPosScale.w * fade);
        vec2 uv01 = sign(corner) * 0.5 + 0.5; // corners are +-halfSize
        vCardUv = vec2((uv01.x + slot) / 8.0, uv01.y);
    }

    gl_Position = uLightViewProj * vec4(world, 1.0);
}
