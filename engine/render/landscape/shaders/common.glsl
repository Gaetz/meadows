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
    vec4 uTerrainInfo;      // x = sea level, y = snow line (m),
                            // z = splat UV scale (tiles/meter)
    vec4 uPostInfo;         // x = filmic tonemap (0/1), y = exposure
    vec4 uFogInfo;          // x = density, y = height falloff (1/m),
                            // z = low-altitude boost, w = start distance (m)
    mat4 uSunViewProj[3];   // world -> cascade shadow clip space
    vec4 uCascadeSplits;    // xyz = cascade far view-distances (m)
    vec4 uShadowInfo;       // xyz = world texel size per cascade,
                            // w = shadow strength (0 = shadows off)
    vec4 uScreenInfo;       // xy = viewport size (px), zw = 1/size
};
