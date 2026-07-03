// Per-frame constants shared by every landscape shader. Must match
// render::FrameUniforms (engine/render/landscape/FrameUniforms.hpp) exactly.
layout(std140, binding = 0) uniform FrameUbo {
    mat4 uViewProj;
    mat4 uInvViewProj;   // NDC -> world, for fullscreen ray reconstruction
    vec4 uCameraPos;     // xyz = camera world position
    vec4 uTime;          // x = seconds since scene start
    vec4 uSunDirection;  // xyz = normalized, points TOWARD the sun
    vec4 uSunColor;      // rgb = sun light color, w = sun disc intensity
    vec4 uSunGlowColor;  // rgb = sky halo/afterglow (outlives the disc)
    vec4 uAmbientColor;  // rgb = sky ambient term
    vec4 uZenithColor;      // rgb = sky gradient top
    vec4 uHorizonColor;     // rgb = horizon on the SUN side (warm at dusk)
    vec4 uHorizonFarColor;  // rgb = horizon opposite the sun (already night)
};
