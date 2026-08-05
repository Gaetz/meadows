#version 460 core
#include "common.glsl"

// Half-res screen-space ambient occlusion (Alchemy-style: spiral taps,
// IGN jitter, derivative normals) over the depth snapshot — the
// contact-shadow pattern's sibling pass. Composited by the tonemap as a
// bounded multiplier; SHORT radius only (uSsaoInfo.y, ~0.7 m): the
// contact-scale crevices the RC GI's coarse probes cannot resolve —
// bark grooves, prop feet, pebble gaps. Distance-faded: far occlusion
// belongs to the GI and the mist. Sky is neutral; the toggle is the
// texture (clear-to-white), like the contact pass.
layout(binding = 0) uniform sampler2D uSceneDepth;
#include "view_util.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    float depth = texture(uSceneDepth, vUv).r;
    if (depth < 1e-8) {
        fragColor = vec4(1.0); // sky = the reversed far clear
        return;
    }
    vec3 position = worldFromDepth(vUv, depth);
    float dist = distance(position, uCameraPos.xyz);
    // Far fade first: past it the taps are pure reconstruction noise.
    float fade = 1.0 - smoothstep(80.0, 140.0, dist);
    if (fade <= 0.001) {
        fragColor = vec4(1.0);
        return;
    }
    vec3 nrm = normalize(cross(dFdx(position), dFdy(position)));
    nrm *= sign(dot(nrm, uCameraPos.xyz - position));

    float radius = uSsaoInfo.y;
    // Projected screen radius (symmetric ~60° fov: focal ~1 in uv/2).
    float uvRadius = min(radius / max(dist, 0.5) * 0.9, 0.25);

    const int kTaps = 8;
    float ang = ignJitter(gl_FragCoord.xy) * 6.2831853;
    float occ = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float t = (float(i) + 0.5) / float(kTaps);
        float a = ang + t * 12.566371; // two spiral turns
        vec2 uv = vUv + vec2(cos(a), sin(a)) * (t * uvRadius);
        if (any(lessThan(uv, vec2(0.0))) ||
            any(greaterThan(uv, vec2(1.0)))) {
            continue;
        }
        float d = texture(uSceneDepth, uv).r;
        if (d < 1e-8) {
            continue; // sky never occludes
        }
        vec3 s = worldFromDepth(uv, d);
        vec3 v = s - position;
        float vv = dot(v, v);
        // Horizon term with a slope bias (kills self-shadow acne on the
        // faceted derivative normals) and a smooth range falloff.
        float falloff = max(0.0, 1.0 - vv / (radius * radius));
        occ += max(0.0,
                   dot(v, nrm) * inversesqrt(max(vv, 1.0e-4)) - 0.10) *
               falloff;
    }
    float ao = clamp(
        1.0 - occ * (uSsaoInfo.x * 2.0 / float(kTaps)), 0.0, 1.0);
    fragColor = vec4(mix(1.0, ao, fade));
}
