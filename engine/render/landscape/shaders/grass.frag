#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "locallights.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"

// The Quick_Grass lighting
// model on our frame machinery (shadows, clouds, terrain light map,
// wetness, fog):
//  - WRAP DIFFUSE (wrap 0.5) — soft carpet lighting.
//  - BACKSCATTER — light bleeding THROUGH blades when looking toward the
//    sun (the reference's "backscatter fakery"), high-detail range only.
//  - ROOT AO — a gentle occlusion ramp (easeIn^2) rising along the
//    blade; depth without breaking the blades' uniform ground color.
//  - BLADE MIDDLE SHADING — the width-center darkens slightly, selling
//    the rounded cross-section together with the two blended normals.

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vT;
layout(location = 2) in float vXSide;
layout(location = 3) in float vLodOut;
layout(location = 4) in vec3 vNormal1;
layout(location = 5) in vec3 vNormal2;
layout(location = 6) in vec3 vWorldPos;

layout(location = 0) out vec4 fragColor;

void main() {
    // Wetness: rain darkens and cools the meadow.
    vec3 albedo = vColor * mix(vec3(1.0), vec3(0.66, 0.72, 0.72),
                               clamp(uStormInfo.y, 0.0, 1.0));

    // Blade middle darkening (near only — flattens out with the LOD).
    float acrossEdge = abs(vXSide * 2.0 - 1.0); // 0 center, 1 edges
    float middle = 1.0 - uGrassBladeInfo.z * (1.0 - acrossEdge) *
                             (1.0 - acrossEdge);
    albedo *= mix(middle, 1.0, vLodOut);

    // Root occlusion (density AO, easeIn^2 up) — kept GENTLE so near
    // blades hold the ground's color down to the carpet. Eases out with
    // the LOD: the terrain has no such AO, so the far carpet must light
    // like the bare ground it dissolves into.
    float ao = mix(mix(uGrassShadeInfo.x, 1.0, vT * vT), 1.0, vLodOut);

    // The two rounded normals blend across the width.
    vec3 n = normalize(mix(vNormal1, vNormal2, vXSide));

    // Wrap diffuse; the shared stylized ramp keeps the BotW A/B working.
    float ndl = dot(n, uSunDirection.xyz);
    float wrap = clamp((ndl + 0.5) / 1.5, 0.0, 1.0);
    float diffuse = stylizedDiffuse(ndl, wrap);

    // Backscatter: view toward the sun lights the blade interior.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    float backLight =
        clamp((dot(viewDir, uSunDirection.xyz) + 0.5) / 1.5, 0.0, 1.0);
    float scatter = backLight * uGrassBladeInfo.w * (1.0 - vLodOut);

    // Thin sheen along the blade, strongest at the tips — kept SMOOTH
    // on purpose (the stepped stylizedSpec band lives on characters and
    // props, mesh/skinned.frag; quantized blade sheen read worse).
    vec3 halfDir = normalize(uSunDirection.xyz - viewDir);
    float sheen = pow(max(dot(n, halfDir), 0.0), 32.0) *
                  uGrassShadeInfo.y * vT;

    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) *
                   cloudShadowFactor(vWorldPos);
    // Distant terrain shadow + sky openness.
    vec2 tl = terrainLightFactors(vWorldPos);
    // The ONE GI technique branch (gi.glsl) — Classic stays intact.
    vec3 ambient = giAmbient(vWorldPos, n, uAmbientColor.rgb * tl.y);
    // Direct local lights on the meadow, CLUSTERED PATH ONLY
    // (docs/RENDERING.md §5 B4) — the blade's blended normal feeds the
    // shared shading; root AO keeps the carpet's depth under torchlight.
    vec3 local = uClusterInfo.x > 0.5 ? localLights(vWorldPos, n)
                                      : vec3(0.0);
    vec3 lit = albedo * ao *
                   (ambient + local +
                    uSunColor.rgb *
                        ((diffuse + scatter) * shadow * tl.x)) +
               uSunColor.rgb * sheen * ao * shadow * tl.x;

    // Scene-alpha contract (tonemap.frag): 0 = this pixel does NOT
    // receive screen-space contact shadows. Blade-on-blade contact
    // noise would break the meadow's flat-mass read; the blades still
    // CAST into the march, so the block keeps darkening the ground
    // beside it.
    fragColor = vec4(applyFog(lit, vWorldPos), 0.0);
}
