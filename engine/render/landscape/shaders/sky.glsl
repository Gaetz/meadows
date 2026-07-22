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
vec3 applyFog(vec3 color, vec3 worldPos) {
    vec3 toPoint = worldPos - uCameraPos.xyz;
    float dist = length(toPoint);
    vec3 viewDir = toPoint / max(dist, 1e-4);
    // Altitude term uses the ray midpoint so tall peaks seen from a valley
    // (and valleys seen from a peak) both fog sensibly.
    float midHeight = 0.5 * (worldPos.y + uCameraPos.y);
    float lowBoost = exp(-max(midHeight - uTerrainInfo.x, 0.0) * uFogInfo.y);
    float density = uFogInfo.x * (1.0 + lowBoost * uFogInfo.z);
    // Only the distance BEYOND the start counts: everything nearer keeps its
    // true colors, the haze belongs to the far field.
    float fogDist = max(dist - uFogInfo.w, 0.0);
    float amount = 1.0 - exp(-fogDist * density);
    return mix(color, skyGradient(viewDir), amount);
}
