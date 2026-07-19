// Cascaded shadow sampling (requires common.glsl). Receivers declare:
//   layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
// then call shadowFactor(worldPos, normal): 1 = fully lit, 0 = in shadow.

// Cascade debug tint (uPostInfo.z): multiply albedo by this to visualize
// cascade boundaries.
vec3 cascadeDebugTint(vec3 worldPos) {
    if (uPostInfo.z < 0.5) {
        return vec3(1.0);
    }
    float viewDist = distance(worldPos, uCameraPos.xyz);
    if (viewDist < uCascadeSplits.x) { return vec3(1.0, 0.6, 0.6); }
    if (viewDist < uCascadeSplits.y) { return vec3(0.6, 1.0, 0.6); }
    if (viewDist < uCascadeSplits.z) { return vec3(0.6, 0.6, 1.0); }
    return vec3(1.0);
}

float shadowFactor(vec3 worldPos, vec3 normal) {
    if (uShadowInfo.w <= 0.0) {
        return 1.0; // shadows disabled or sun below the horizon
    }
    float viewDist = distance(worldPos, uCameraPos.xyz);
    if (viewDist >= uCascadeSplits.z) {
        return 1.0;
    }
    int cascade = viewDist < uCascadeSplits.x ? 0
                  : viewDist < uCascadeSplits.y ? 1 : 2;

    // Normal-offset bias scaled by the cascade's texel footprint: pushes the
    // sample point off the surface just enough to kill acne without visible
    // peter-panning (polygon offset on the casters does the rest).
    float texelWorld = uShadowInfo[cascade];
    vec3 offsetPos = worldPos + normal * (texelWorld * 1.5);

    vec4 lightClip = uSunViewProj[cascade] * vec4(offsetPos, 1.0);
    vec3 proj = lightClip.xyz / lightClip.w;
    proj.xy = proj.xy * 0.5 + 0.5; // 0..1 clip: only xy needs NDC->UV
    if (proj.z >= 1.0) {
        return 1.0;
    }

    // 3x3 PCF over the hardware-compared fetches (each is already 2x2).
    float texel = 1.0 / 2048.0;
    float sum = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            sum += texture(uShadowMap,
                           vec4(proj.xy + vec2(dx, dy) * texel,
                                float(cascade), proj.z));
        }
    }
    float lit = sum / 9.0;

    // Fade shadows out at the last cascade's edge, and honor the global
    // strength (day/night ramp, UI toggle).
    float edgeFade =
        smoothstep(uCascadeSplits.z * 0.85, uCascadeSplits.z, viewDist);
    lit = mix(lit, 1.0, edgeFade);
    return mix(1.0, lit, uShadowInfo.w);
}
