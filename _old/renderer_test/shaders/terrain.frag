#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in float inHeight;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform TerrainUBO {
    mat4  viewProj;
    vec4  cameraPos;
    vec4  sunDir;
    float patchSize;
    float heightScale;
    float fbmFrequency;
    float fbmPersistence;
    int   fbmOctaves;
    int   gridSize;
    float ambientLight;
    float time;
} ubo;

// Palettes identiques au sky shader (mêmes constantes)
void skyPalette(float elev,
                out vec3 horizon, out vec3 zenith,
                out float tDay,   out float tNight) {
    tDay   = smoothstep(0.0,   0.30, elev);
    tNight = smoothstep(-0.05, -0.22, elev);

    horizon = mix(mix(vec3(0.98, 0.62, 0.18),   // sunset
                      vec3(0.72, 0.88, 1.00),    // jour
                      tDay),
                  vec3(0.04, 0.07, 0.20),         // nuit
                  tNight);

    zenith  = mix(mix(vec3(0.20, 0.12, 0.42),   // sunset
                      vec3(0.12, 0.28, 0.78),    // jour
                      tDay),
                  vec3(0.01, 0.02, 0.10),         // nuit
                  tNight);
}

// Option A — ACES filmic tone mapping
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x*(a*x+b)) / (x*(c*x+d)+e), 0.0, 1.0);
}

void main() {
    vec3  sunN = normalize(ubo.sunDir.xyz);
    float elev = sunN.y;

    vec3  horizonCol, zenithCol;
    float tDay, tNight;
    skyPalette(elev, horizonCol, zenithCol, tDay, tNight);

    // ── Albedo : hauteur + pente ──────────────────────────────────────────
    float slope = 1.0 - inNormal.y;

    vec3 col = mix(vec3(0.05, 0.18, 0.45), vec3(0.76, 0.70, 0.50), smoothstep(0.00, 0.08, inHeight));
    col = mix(col, vec3(0.20, 0.45, 0.12), smoothstep(0.08, 0.18, inHeight));
    col = mix(col, vec3(0.45, 0.40, 0.35), smoothstep(0.50, 0.65, inHeight));
    col = mix(col, vec3(0.92, 0.95, 1.00), smoothstep(0.70, 0.85, inHeight));
    col = mix(col, vec3(0.45, 0.40, 0.35), smoothstep(0.25, 0.50, slope));

    // ── Option B — Éclairage hémisphérique ────────────────────────────────
    float diff = max(dot(inNormal, sunN), 0.0);

    // Hemisphere ambient : ciel au-dessus, sol en dessous
    vec3  gndBounce = vec3(0.18, 0.14, 0.08);  // rebond sol neutre
    float hemiT     = inNormal.y * 0.5 + 0.5;  // [0=bas, 1=haut]
    vec3  hemiAmb   = mix(gndBounce, zenithCol, hemiT) * ubo.ambientLight;

    // Couleur du soleil : blanc pur à midi, orange au coucher, éteint la nuit
    vec3 sunColor = mix(vec3(1.00, 0.52, 0.08), vec3(1.00, 1.00, 1.00), tDay) * (1.0 - tNight);

    col = col * (hemiAmb + sunColor * diff * (1.0 - ubo.ambientLight));

    // Scalar light factor conservé pour l'eau (Option D)
    float lightFactor = ubo.ambientLight + (1.0 - ubo.ambientLight) * diff;

    // ── Lueur de rebond coucher de soleil ─────────────────────────────────
    float sunsetStr = smoothstep(0.28, 0.0, elev) * smoothstep(-0.18, 0.0, elev);
    if (sunsetStr > 0.001) {
        float backLight = max(dot(inNormal, -sunN), 0.0);
        col += vec3(0.80, 0.28, 0.02) * backLight * sunsetStr * 0.18;
    }

    // ── Option C — Height fog (perspective aérienne) ──────────────────────
    float fogTopScale = 80.0;       // échelle de hauteur de brouillard (mètres)
    float fogBase     = 0.0000125;    // densité au niveau de la mer
    float fragH   = max(inWorldPos.y, 0.0);
    float hFog    = fogBase * exp(-fragH / fogTopScale);
    float dist    = length(inWorldPos.xyz - ubo.cameraPos.xyz);
    float fogFactor = 1.0 - exp(-dist * hFog);

    // In-scattering : couleur du ciel dans la direction du fragment
    vec3  toFrag    = normalize(inWorldPos.xyz - ubo.cameraPos.xyz);
    float skyT      = smoothstep(-0.05, 0.4, toFrag.y);
    vec3  inScatter = mix(horizonCol, zenithCol, max(skyT, 0.0));
    // Lueur directionnelle coucher de soleil dans le brouillard
    float sunAlign = max(dot(vec2(toFrag.x, toFrag.z), vec2(sunN.x, sunN.z)), 0.0);
    inScatter += vec3(1.0, 0.38, 0.04) * pow(sunAlign, 5.0) * sunsetStr * 0.6;

    col = mix(col, inScatter, fogFactor);

    // ── Option D — Plan d'eau procédural ──────────────────────────────────
    const float waterLevel = 0.06;
    if (inHeight < waterLevel) {
        // Normale eau avec vagues simples
        float wX = sin(inWorldPos.x * 0.25 + ubo.time * 1.1)  * 0.05;
        float wZ = sin(inWorldPos.z * 0.31 + ubo.time * 0.85) * 0.05;
        vec3 waterNormal = normalize(vec3(wX, 1.0, wZ));

        // Fresnel (schlick approx)
        vec3  viewDir = normalize(ubo.cameraPos.xyz - inWorldPos);
        float cosA    = max(dot(waterNormal, viewDir), 0.0);
        float fresnel = pow(1.0 - cosA, 4.0);

        // Couleur du ciel réfléchi
        vec3  refl   = reflect(-viewDir, waterNormal);
        float reflT  = smoothstep(-0.05, 0.4, refl.y);
        vec3  skyRefl = mix(horizonCol, zenithCol, max(reflT, 0.0));

        vec3 deepWater = vec3(0.02, 0.06, 0.16);
        col = mix(deepWater, skyRefl, fresnel * 0.75) * lightFactor;
    }

    // ── Option A — Tone mapping ACES filmic ───────────────────────────────
    outColor = vec4(aces(col), 1.0);
}
