#version 460 core
#include "common.glsl"
#include "sky.glsl"

in vec3 vNormal;
in vec3 vColor;
in vec3 vWorldPos;
in float vTint;

out vec4 fragColor;

void main() {
    // Per-instance hue roll: some trees lean yellow-green, some deep green.
    vec3 albedo = vColor * mix(vec3(0.85, 1.0, 0.75), vec3(1.1, 1.0, 1.15),
                               vTint);

    vec3 n = normalize(vNormal);
    // Wrap diffuse keeps the shaded side of the canopy readable (soft-GI
    // feel); the flat facets do the stylization.
    float wrap = clamp((dot(n, uSunDirection.xyz) + 0.4) / 1.4, 0.0, 1.0);
    vec3 lit = albedo * (uAmbientColor.rgb + uSunColor.rgb * wrap);

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
