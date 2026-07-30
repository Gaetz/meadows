// Shared sky color functions (requires common.glsl). The fog pass tints
// distant terrain with skyGradient() — the SAME function that paints the
// dome — so far geometry dissolves into the sky at any time of day.

// Sky color along a view direction, without the sun disc.
vec3 skyGradient(vec3 dir) {
    // The warm horizon belongs to the sun's side of the sky; opposite it the
    // horizon has already gone to night. Blend by azimuthal alignment.
    vec2 sunH = uSunDirection.xz;
    float sunHLen = max(length(sunH), 1e-4);
    vec2 dirH = dir.xz;
    float dirHLen = max(length(dirH), 1e-4);
    float sunward = dot(dirH / dirHLen, sunH / sunHLen) * 0.5 + 0.5;
    vec3 horizonColor =
        mix(uHorizonFarColor.rgb, uHorizonColor.rgb, sunward * sunward);

    // Horizon glow curve: color hugs the horizon, zenith takes over above.
    float horizonT = pow(1.0 - clamp(dir.y, 0.0, 1.0), 2.5);
    vec3 sky = mix(uZenithColor.rgb, horizonColor, horizonT);
    // Warm halo around the sun; a second, broader lobe swells when the sun
    // sits low, spreading the glow across half the sky. Driven by the GLOW
    // color, not the disc color: it survives sunset (l'heure entre chien et
    // loup — the scattered light lingers after the disc is gone).
    float sunAmount = max(dot(dir, uSunDirection.xyz), 0.0);
    float lowSun = 1.0 - smoothstep(0.05, 0.35, uSunDirection.y);
    sky += uSunGlowColor.rgb * pow(sunAmount, 24.0) * 0.30;
    sky += uSunGlowColor.rgb * pow(sunAmount, 5.0) * 0.24 * lowSun;
    return sky;
}

// Sky with the sun disc added (the dome shader).
vec3 skyWithSun(vec3 dir) {
    vec3 sky = skyGradient(dir);
    float cosSun = dot(dir, uSunDirection.xyz);
    sky += uSunColor.rgb * uSunColor.a * smoothstep(0.99945, 0.99985, cosSun);
    return sky;
}

// Distance + height fog, tinted by skyGradient along the view ray: distant
// geometry dissolves into EXACTLY the sky behind it, at any time of day (the
// BotW haze). Denser near sea level so valleys and shores go misty first.
// `cloudVis` = the cloud sun-visibility at the surface point
// (cloudShadowFactor, passed by callers that have the cloud map bound —
// sky.glsl is included before clouds.glsl so it cannot tap it here). It
// carries the cloud-shadow pattern into the analytic tail: distant fog
// dims under clouds and the froxel band's ray curtains continue to the
// horizon instead of stopping at the volumetric reach.
vec3 applyFog(vec3 color, vec3 worldPos, float cloudVis) {
    vec3 toPoint = worldPos - uCameraPos.xyz;
    float dist = length(toPoint);
    vec3 viewDir = toPoint / max(dist, 1e-4);
    // Altitude term uses the ray midpoint so tall peaks seen from a valley
    // (and valleys seen from a peak) both fog sensibly.
    float midHeight = 0.5 * (worldPos.y + uCameraPos.y);
    float lowBoost = exp(-max(midHeight - uTerrainInfo.x, 0.0) * uFogInfo.y);
    float density = uFogInfo.x * (1.0 + lowBoost * uFogInfo.z) *
                    exp(-max(midHeight - uTerrainInfo.x, 0.0) *
                        uFogLayerInfo.x);
    // Only the distance BEYOND the start counts: everything nearer keeps its
    // true colors, the haze belongs to the far field. When the volumetric
    // march owns the near fog (uFogSunInfo.z = its reach, composer-set),
    // the analytic term is only the TAIL beyond it.
    float fogStart = max(uFogInfo.w, uFogSunInfo.z);
    float fogDist = max(dist - fogStart, 0.0);
    float amount = 1.0 - exp(-fogDist * density);
    // HORIZON CLOSURE (uFogLayerInfo.y = the terrain streaming edge, m):
    // whatever the weather's fog, geometry is FULLY dissolved into the
    // sky by the edge of the ring — new chunks are born inside the veil
    // instead of popping, and trees stop sprouting from bare ground.
    if (uFogLayerInfo.y > 0.0) {
        amount = max(amount, smoothstep(uFogLayerInfo.y * 0.72,
                                        uFogLayerInfo.y * 0.97, dist));
    }
    // Sun single-scatter (docs/RENDERING.md V1): without it, lit and
    // shadowed air converge to the same sky color — the grey veil. The
    // forward lobe warms fog toward the sun and lets it cool away from
    // it; strength rides the weather, the exponent is global tuning.
    float mu = dot(viewDir, uSunDirection.xyz);
    // The sun lobe carries the full cloud pattern; the gradient only a
    // touch (shadowed distant air still sees most of the sky sideways).
    vec3 fogColor = skyGradient(viewDir) * mix(0.9, 1.0, cloudVis) +
                    uSunColor.rgb *
                        (pow(clamp(mu * 0.5 + 0.5, 0.0, 1.0),
                             uFogSunInfo.y) *
                         uFogSunInfo.x * cloudVis);
    return mix(color, fogColor, amount);
}

vec3 applyFog(vec3 color, vec3 worldPos) {
    return applyFog(color, worldPos, 1.0);
}
