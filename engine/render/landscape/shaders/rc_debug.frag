#version 460 core
#include "common.glsl"

// Radiance cascades debug view — fullscreen raymarch of one clipmap
// volume, alpha-blended over the tonemapped frame (drawn inside the
// composite pass). This view IS the validation of the injection pass:
// terrain surfaces should appear as a lit voxel shell hugging the ground,
// solid black below, empty air transparent.

layout(std140, binding = 2) uniform RcUbo {
    vec4 uRcClipInfo[2]; // xyz = min-corner origin, w = voxel size
    vec4 uRcTileInfo;    // w = clip resolution
    vec4 uRcMisc;        // x = debug clip index
};

layout(binding = 5) uniform sampler3D uRcClipFine;
layout(binding = 6) uniform sampler3D uRcClipCoarse;
layout(binding = 10) uniform sampler3D uRcCascade0; // merged (rc_merge), dir-major

// Merged cascade-0 irradiance at a fine-clip point: average of the 8
// direction slabs (hardware trilinear per slab, z clamped inside it).
vec3 cascadeIrradiance(vec3 uvw, float res) {
    float zProbe = clamp(uvw.z * res, 0.5, res - 0.5);
    vec3 sum = vec3(0.0);
    for (int d = 0; d < 8; ++d) {
        float z = (zProbe + float(d) * res) / (res * 8.0);
        sum += texture(uRcCascade0, vec3(uvw.xy, z)).rgb;
    }
    return sum / 8.0;
}

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    // View 1 = fine clip, 2 = coarse clip, 3 = merged cascade-0
    // irradiance (marched on the FINE clip's occupancy).
    const int view = int(uRcMisc.x + 0.5);
    const int clip = view == 2 ? 1 : 0;
    const vec4 info = uRcClipInfo[clip];
    const float res = uRcTileInfo.w;
    const float span = res * info.w;

    // Camera ray through this pixel (the god-ray/volumetric pattern).
    vec4 nearW = uInvViewProj * vec4(vUv * 2.0 - 1.0, 1.0, 1.0); // reversed near
    vec4 farW = uInvViewProj * vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
    vec3 origin = nearW.xyz / nearW.w;
    vec3 dir = normalize(farW.xyz / farW.w - origin);

    // Clip the ray to the volume's AABB.
    vec3 boxMin = info.xyz;
    vec3 boxMax = info.xyz + vec3(span);
    vec3 inv = 1.0 / dir;
    vec3 t0 = (boxMin - origin) * inv;
    vec3 t1 = (boxMax - origin) * inv;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tExit = min(min(tbig.x, tbig.y), tbig.z);
    if (tExit <= max(tEnter, 0.0)) {
        discard; // volume not on this ray
    }

    // March at half-voxel steps; show the first occupied voxel.
    float t = max(tEnter, 0.0) + info.w * 0.25;
    const float step = info.w * 0.5;
    for (int i = 0; i < 512 && t < tExit; ++i, t += step) {
        vec3 p = origin + dir * t;
        vec3 uvw = (p - info.xyz) / span;
        vec4 texel = clip == 0 ? texture(uRcClipFine, uvw)
                               : texture(uRcClipCoarse, uvw);
        if (texel.a > 0.5) {
            vec3 c;
            if (view == 3) {
                // The merged GI arriving just above the hit surface.
                vec3 above = uvw + vec3(0.0, info.w * 1.5 / span, 0.0);
                c = cascadeIrradiance(above, res);
            } else {
                c = texel.rgb; // the injected direct radiance
            }
            // Radiance is HDR — rough-tonemap for the debug overlay.
            c = c / (c + vec3(0.6));
            fragColor = vec4(c, 0.85);
            return;
        }
    }
    discard;
}
