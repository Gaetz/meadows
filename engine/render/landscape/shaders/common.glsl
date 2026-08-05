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
    vec4 uGrassBaseTint; // rgb = base tint × ground albedo, w = fade start (m)
    vec4 uGrassTipTint;  // rgb = tip tint × ground albedo, w = fade end (m)
    // The GI technique switch (gi.glsl).
    vec4 uGiInfo;     // x = technique (0 classic / 1 RC), y = intensity,
                      // z = edge fade width (m), w = grid resolution
    vec4 uGiGridInfo; // xyz = cascade-0 grid origin, w = probe spacing
    // GI ramp: x = band count floor->classic (0 = smooth), y = AA,
    // z = GI ambient floor (fraction of classic).
    vec4 uGiBandInfo;
    // Foliage-card leaf cutout -> solid ramp: xy = mip window;
    // z = mirror pass (flip the billboard card winding).
    vec4 uLeafLodInfo;
    // Fog sun single-scatter: x = strength, y = phase exponent.
    vec4 uFogSunInfo;
    // Stylized diffuse ramp: xy = terminator edges, zw = full-light edges.
    vec4 uStylizedDiffuse;
    // Stylized shadow snap: xy = window, z = floor, w = half-tone level.
    vec4 uStylizedShadow;
    // Clustered forward: x = active, y = cluster grid far reach (m).
    vec4 uClusterInfo;
    // Key-shadow atlas: world -> tile clip per slot; a light's slot
    // rides LightsUbo windowInfo.z (slot+1, 0 = unshadowed).
    mat4 uKeyShadowAtlas[4];
    // Grass shading knobs (grass.vert/.frag): x = root-AO floor
    // (1 = none), y = tip sheen strength, z = near blade-normal share
    // (0 = blades light like the ground), w free.
    vec4 uGrassShadeInfo;
    // x/y = whole-blade brightness hash range, z = middle darkening,
    // w = backscatter strength.
    vec4 uGrassBladeInfo;
    // Stylized specular band on characters/props (mesh/skinned.frag):
    // x = strength, y = band threshold, z = Blinn-Phong exponent.
    vec4 uStylizedSpec;
    // Ground mist (mist.frag): x = extinction sigma (1/m, 0 = off),
    // y = reach (m), z = coverage threshold, w = coverage softness.
    vec4 uMistInfo;
    // x = coverage pattern scale (1/m), y = erosion noise scale (1/m),
    // z = erosion strength, w = lift (m, raises the baked mist top).
    vec4 uMistShapeInfo;
    // Mist valley map: xy = bake center (world XZ), z = 1/span,
    // w = max mist-top Y (bounds the raymarch's slab clip).
    vec4 uMistMapInfo;
    // x = erode with the baked NoiseVolume (0 = analytic fbm3),
    // y = march steps, z = erosion detail dropout distance (m),
    // w = sun-beam gain.
    vec4 uMistDetailInfo;
    // x = forward HG lobe g, y = backscatter weight, z = ambient gain,
    // w = ambient floor in shadow (the silver-lining kit).
    vec4 uMistLightInfo;
    // x = fog ceiling falloff (1/m above sea level) — the fog is a
    // ground layer; upward rays exit it and the sky stays readable.
    // y = horizon-closure distance (m; the far-terrain reach or the
    // streaming ring, 0 = off), z = the streaming ring itself (m) —
    // the far mesh's sink bias.
    vec4 uFogLayerInfo;
    // Volumetric sky clouds: x = active (gates the 2D dome layer off),
    // y = slab thickness (m), z = extinction sigma (1/m), w = erosion
    // strength. Slab base = uCloudInfo.y; coverage = uCloudInfo.x.
    vec4 uCloudVolInfo;
    // x = body sun gain, y = body HG lobe g, z = ambient gain,
    // w = lining gain (direct transmission — the silver lining).
    vec4 uCloudVolLightInfo;
    // x = thickness<->coverage spread, y = lining HG lobe g,
    // z = powder strength, w = puffiness (fractal edge erosion).
    vec4 uCloudVolShapeInfo;
    // x = rim gain (view-thin silhouette glow), y = rim HG lobe g,
    // z = storm darkening 0..10, exponential (coverage-scaled; ambient
    // + 85% of the sun body; lining/rim untouched), w free.
    vec4 uCloudVolRimInfo;
    // x = mist puffiness (fractal edge florets), yzw free.
    vec4 uMistPuffInfo;
    // x = water debug view mode (0 off), yzw free.
    vec4 uWaterDebugInfo;
    // Water-info map: xy = center (world XZ), z = 1/span, w = valid.
    vec4 uWaterInfoMapInfo;
    // Seasons: x = autumn blend 0..1, y = leaf fall 0..1, zw free.
    vec4 uSeasonInfo;
    // Per leaf-mask atlas slot: rgb = autumn tint, a = seasonality
    // (0 = evergreen — conifers keep needles and color).
    vec4 uLeafSeason[8];
    // x = height-blend band depth (0 = plain weighted blend), yzw
    // reserved (detail fade / POM knobs).
    vec4 uSplatDetailInfo;
    // Region shading maps (TerrainShadeMap): xy = center, z = 1/span,
    // w = valid.
    vec4 uTerrainShadeMapInfo;
    // x = bi-frequency variety strength (0 = off), yzw reserved.
    vec4 uSplatVarietyInfo;
    // Grass species table (GrassSpecies.hpp): shape = {height (unused in
    // shader — applied at scatter), width, lean, tip profile}; base/tip
    // tints multiply the inherited ground albedo.
    vec4 uGrassSpeciesShape[6];
    vec4 uGrassSpeciesBase[6];
    vec4 uGrassSpeciesTip[6];
    vec4 uSsaoInfo;
};
