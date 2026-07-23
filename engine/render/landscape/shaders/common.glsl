// Per-frame constants shared by every landscape shader. Must match
// render::FrameUniforms (engine/render/landscape/FrameUniforms.hpp) exactly;
// new fields are only ever APPENDED at the end (never inserted).
layout(std140, binding = 0) uniform FrameUbo {
    mat4 uViewProj;
    mat4 uInvViewProj;   // NDC -> world, for fullscreen ray reconstruction
    vec4 uCameraPos;     // xyz = camera world position
    vec4 uTime;          // x = seconds since scene start,
                         // z = volumetric shaft intensity
    vec4 uSunDirection;  // xyz = normalized, points TOWARD the sun
    vec4 uSunColor;      // rgb = sun light color, w = sun disc intensity
    vec4 uSunGlowColor;  // rgb = sky halo/afterglow (outlives the disc)
    vec4 uAmbientColor;  // rgb = sky ambient term, w = stylized-lighting
                         // blend (0 classic, 1 BotW step — stylized.glsl)
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
    vec4 uCloudInfo;        // x = coverage [0,1], y = layer height (m),
                            // z = pattern scale (1/m), w = shadow strength
    vec4 uSunScreen;        // xy = sun position in screen UV, z = shaft
                            // visibility fade, w = god-ray intensity
    vec4 uCloudMapInfo;     // xy = baked cloud-field center (world XZ),
                            // z = 1/span
    vec4 uWaterMapInfo;     // xy = pool-depth map center (world XZ),
                            // z = 1/span
    vec4 uWindInfo;         // x = accumulated wind time (s, speed-scaled),
                            // y = sway strength, z = water chop multiplier
    // Terrain light map.
    vec4 uTerrainLightInfo; // xy = map center (world XZ), z = 1/span,
                            // w = strength (0 = feature off)
    // x = effective water surface Y for the camera
    // (sea outdoors, a volume top inside one, -1e6 = dry).
    vec4 uSubmersionInfo;
    // The interior key-light shadow.
    mat4 uKeyShadowViewProj;
    vec4 uKeyShadowInfo; // xyz = that light's position, w = active
    // x = storm front, y = rain intensity.
    vec4 uStormInfo;
    // world -> top-down rain-occlusion clip.
    mat4 uRainOcclusionViewProj;
    // Grass bending — xy = feet XZ, z = feet Y,
    // w = radius (0 = off).
    vec4 uGrassBendInfo;
    // Meadow tuning, live from the render panel.
    vec4 uGrassShapeInfo; // x = blade height, y = half width,
                          // zw = high-detail near/far (m)
    vec4 uGrassLodInfo;   // xy = density thinning start/end (m),
                          // z = far density floor, w = width compensation
    vec4 uGrassBaseColor; // rgb = base albedo, w = fade start (m)
    vec4 uGrassTipColor;  // rgb = tip albedo, w = fade end (m)
    // The GI technique switch (gi.glsl).
    vec4 uGiInfo;     // x = technique (0 classic / 1 RC), y = intensity,
                      // z = edge fade width (m), w = grid resolution
    vec4 uGiGridInfo; // xyz = cascade-0 grid origin, w = probe spacing
    // Fixed log-step GI ramp: x = stops per band, y = AA,
    // z = GI ambient floor (fraction of classic).
    vec4 uGiBandInfo;
    // Foliage-card leaf cutout -> solid ramp: xy = mip window.
    vec4 uLeafLodInfo;
    // Fog sun single-scatter: x = strength, y = phase exponent.
    vec4 uFogSunInfo;
    // Stylized diffuse ramp: xy = terminator edges, zw = full-light edges.
    vec4 uStylizedDiffuse;
    // Stylized shadow snap: xy = window, z = floor, w = half-tone level.
    vec4 uStylizedShadow;
};
