#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Pre-water scene snapshot (copied between the opaque and water passes —
// sampling a bound attachment would be undefined).
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uSceneDepth;
// Half-res mirrored scene (uTerrainInfo.w = 1 when valid this frame). For
// points on the water plane the mirror camera projects to the SAME screen
// UV, so the fragment's own UV samples its reflection.
layout(binding = 2) uniform sampler2D uReflection;
// CPU-baked pool depth around the camera (pre-dilated neighborhood max) —
// view-independent, unlike screen-space probing.
layout(binding = 3) uniform sampler2D uPoolDepth;
// Main-view volumetric sky clouds (rgb + transmittance a, last frame's
// display buffer) — reprojected into the mirror, see below.
layout(binding = 4) uniform sampler2D uSkyClouds;

#include "view_util.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

// Scrolling multi-octave wave normal, analytic derivatives (no texture).
vec3 waveNormal(vec2 p, float t) {
    vec2 grad = vec2(0.0);
    grad.x += 0.9 * cos(p.x * 0.23 + p.y * 0.11 + t * 0.85);
    grad.y += 0.7 * cos(p.y * 0.19 - p.x * 0.07 + t * 0.63);
    grad.x += 0.45 * cos(p.x * 0.71 - p.y * 0.33 - t * 1.7);
    grad.y += 0.45 * cos(p.y * 0.83 + p.x * 0.29 + t * 1.9);
    grad += 0.22 * vec2(cos(p.x * 2.3 + t * 3.1), cos(p.y * 2.1 - t * 2.7));
    // Weather chop (uWindInfo.z): storm whips the slopes up, calm flattens
    // the surface toward a mirror.
    float slope = 0.045 * uWindInfo.z;
    return normalize(vec3(-grad.x * slope, 1.0, -grad.y * slope));
}

void main() {
    vec2 screenUv = gl_FragCoord.xy * uScreenInfo.zw;
    float t = uWindInfo.x; // wind-scaled phase: waves slow down in a calm
    vec3 n = waveNormal(vWorldPos.xz, t);

    // Underside: the camera is below the surface, looking up through it.
    if (uCameraPos.y < uTerrainInfo.x) {
        vec3 upViewDir = normalize(vWorldPos - uCameraPos.xyz);
        vec3 nDown = -n; // face the submerged viewer
        // The world above shows through, wobbled by the waves; grazing
        // angles bend into the deep tint (cheap total internal reflection).
        vec2 aboveUv = screenUv + n.xz * 0.035;
        vec3 above = texture(uSceneColor, aboveUv).rgb *
                     vec3(0.70, 0.95, 1.00);
        float internal =
            pow(1.0 - max(dot(-upViewDir, nDown), 0.0), 3.0);
        vec3 color = mix(above, vec3(0.006, 0.040, 0.048),
                         clamp(internal * 0.9, 0.0, 1.0));
        fragColor = vec4(color, 1.0);
        return;
    }

    // Water thickness along the view ray, from the scene depth snapshot.
    vec3 floorWorld =
        worldFromDepth(screenUv, texture(uSceneDepth, screenUv).r);
    float thickness = max(distance(uCameraPos.xyz, floorWorld) -
                              distance(uCameraPos.xyz, vWorldPos),
                          0.0);

    // Refraction: scene color behind the surface, wobbled by the waves
    // (distortion grows with depth, none at the waterline so shores stay
    // glued), then absorbed toward the deep tint (red dies first).
    vec2 refractionUv =
        screenUv + n.xz * (0.018 * clamp(thickness * 0.5, 0.0, 1.0));
    vec3 refracted = texture(uSceneColor, refractionUv).rgb;
    vec3 absorption = exp(-thickness * vec3(0.42, 0.16, 0.12));
    vec3 transmitted =
        mix(vec3(0.008, 0.045, 0.055), refracted, absorption);

    // Reflection: the mirrored scene (terrain, trees, sky + sun glints all
    // included), wobbled by the waves; falls back to the analytic sky when
    // the planar pass is off.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    vec3 reflected;
    if (uTerrainInfo.w > 0.5) {
        vec2 reflectionUv = clamp(screenUv + n.xz * 0.035, vec2(0.001),
                                  vec2(0.999));
        reflected = texture(uReflection, reflectionUv).rgb;
    } else {
        vec3 reflectDir = reflect(viewDir, n);
        reflectDir.y = abs(reflectDir.y); // never reflect below the horizon
        reflected = skyWithSun(reflectDir);
    }
    // Volumetric sky clouds live in a main-view screen buffer
    // (composited by the tonemap), so the mirrored scene never contains
    // them. Clouds sit at quasi-infinity, so DIRECTION alone reprojects
    // them: follow the reflected ray from the main camera and composite
    // the cloud buffer where it lands on-screen. Off-screen (or with
    // the volumetric pass off) the 2D dome layer the reflection pass
    // drew remains — same coverage field, so the patterns agree.
    if (uCloudVolInfo.x > 0.5) {
        vec3 reflectDir = reflect(viewDir, n);
        reflectDir.y = abs(reflectDir.y);
        vec4 clip =
            uViewProj * vec4(uCameraPos.xyz + reflectDir * 40000.0, 1.0);
        if (clip.w > 0.0) {
            vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
            vec2 edge = min(uv, 1.0 - uv);
            float onScreen = smoothstep(0.0, 0.06, min(edge.x, edge.y));
            if (onScreen > 0.0) {
                vec4 cloud = texture(uSkyClouds, uv);
                reflected = mix(reflected,
                                reflected * cloud.a + cloud.rgb,
                                onScreen);
            }
        }
    }
    float fresnel =
        0.02 + 0.98 * pow(1.0 - max(dot(-viewDir, n), 0.0), 5.0);
    // Dialed down so the reflection reads as water, not as a second world.
    fresnel *= 0.75;

    vec3 color = mix(transmitted, reflected, fresnel);

    // Shore foam: a solid lapping line right at the waterline plus a wider
    // animated fringe further out.
    float band = 1.0 - smoothstep(0.0, 4.5, thickness);
    float waterline = 1.0 - smoothstep(0.0, 0.8, thickness);
    float pattern = 0.5 + 0.5 * sin(thickness * 2.6 - t * 1.8 +
                                    (n.x + n.z) * 22.0);
    float foam = waterline +
                 band * smoothstep(0.30, 0.75, pattern + band * 0.30) * 0.8;

    // Small-pool suppression: ONE tap into the CPU-baked pool-depth map
    // (already a neighborhood max). Shallow-everywhere puddles lose their
    // foam; real shores — deep water nearby — keep it. View-independent:
    // no camera-distance popping.
    vec2 poolUv =
        (vWorldPos.xz - uWaterMapInfo.xy) * uWaterMapInfo.z + 0.5;
    float poolDepth = texture(uPoolDepth, poolUv).r;
    foam *= smoothstep(2.0, 4.0, poolDepth);

    color = mix(color, vec3(0.75, 0.82, 0.85), clamp(foam, 0.0, 1.0));

    fragColor = vec4(applyFog(color, vWorldPos), 1.0);
}
