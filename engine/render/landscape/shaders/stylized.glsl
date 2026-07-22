// BotW-style stylized lighting, shared by terrain/tree/leaf/grass
// (requires common.glsl). Faithful to the halisavakis cel model: ONE hard
// diffuse threshold (he explicitly removed the intermediate shadow bands),
// quantized shadow attenuation (round(atten) -> flat shadow pools), fake
// SSS gated to lit areas, and a stepped rim on 1-N·V. The narrow
// smoothsteps everywhere are anti-aliasing only, not softness.
// uAmbientColor.w blends classic (0) -> stylized (1): the panel's A/B.

// `ndl` is the RAW N·L (not clamped); `classic` is the shader's
// pre-stylization response (lambert or wrap), used when the toggle is off.
// TWO shadow steps: shade 0 -> half-tone 0.6 at the
// terminator -> full light 1.0 higher up. Three flat plateaus.
float stylizedDiffuse(float ndl, float classic) {
    float stepped = 0.6 * smoothstep(0.02, 0.09, ndl) +
                    0.4 * smoothstep(0.32, 0.40, ndl);
    return mix(classic, stepped, uAmbientColor.w);
}

// The article's round(atten): CSM attenuation snaps to shadow / lit, so
// cast shadows read as flat pools instead of PCF gradients. (The
// narrow 0.45-0.55 window keeps the edges crisp.)
float stylizedShadow(float shadow) {
    return mix(shadow, smoothstep(0.45, 0.55, shadow), uAmbientColor.w);
}

// Fake SSS: pow(dot(view, -light)) — sun punching through thin vegetation
// toward the camera. The caller multiplies by diffuse and shadow (the
// article's stepAtten * diff gate: it lives on the lit edges).
float stylizedSss(vec3 worldPos) {
    vec3 viewDir = normalize(worldPos - uCameraPos.xyz);
    return pow(max(dot(viewDir, uSunDirection.xyz), 0.0), 3.0);
}

// Stepped rim on the silhouettes (1 - N·V) — pays off on spherized
// normals: a crisp bright fringe where the canopy meets the sky.
float stylizedRim(vec3 n, vec3 worldPos) {
    vec3 toCamera = normalize(uCameraPos.xyz - worldPos);
    float rim = 1.0 - max(dot(n, toCamera), 0.0);
    return smoothstep(0.70, 0.78, rim) * uAmbientColor.w;
}
