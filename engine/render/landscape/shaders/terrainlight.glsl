// The worker-baked terrain light map (TerrainLightMap):
// R = long-range sun visibility (mountains casting past the CSM range),
// G = sky openness (valley floors get less ambient). Texture unit 7,
// bound by the scene as its own group; uTerrainLightInfo.w = strength
// (0 = feature off, the exterior stays byte-identical).

layout(binding = 7) uniform sampler2D uTerrainLight;

// x = sun multiplier, y = ambient multiplier at this world position.
vec2 terrainLightFactors(vec3 worldPos) {
    float strength = uTerrainLightInfo.w;
    if (strength <= 0.001) {
        return vec2(1.0);
    }
    vec2 uv = (worldPos.xz - uTerrainLightInfo.xy) * uTerrainLightInfo.z +
              0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec2(1.0);
    }
    vec2 rg = texture(uTerrainLight, uv).rg;
    // Sun: full occlusion allowed (a mountain IS night). Sky openness:
    // gentle — ambient never goes fully black in a valley.
    float sunMul = mix(1.0, rg.r, strength);
    float skyMul = mix(1.0, 0.45 + 0.55 * rg.g, strength);
    return vec2(sunMul, skyMul);
}
