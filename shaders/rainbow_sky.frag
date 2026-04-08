#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform RainbowUBO {
    mat4  invViewProj;
    vec4  cameraPos;
    vec4  sunDir;          // xyz: direction FROM observer TOWARD sun (normalized)
    float dropletRadius;   // mm
    float nBase;           // refractive index of water at 589nm
    float primaryIntensity;
    float secondaryIntensity;
    int   showPrimary;
    int   showSecondary;
    int   numWavelengths;
    float altitude;   // metres — controle la visibilite du cercle complet
} ubo;

const float PI = 3.14159265359;

// Approximate CIE color matching: wavelength (nm) → linear RGB
vec3 wavelengthToRGB(float lambda) {
    float r, g, b;
    float t;
    if (lambda < 380.0 || lambda > 700.0) return vec3(0.0);

    if (lambda < 440.0) {
        t = (440.0 - lambda) / 60.0;
        r = 0.3 + 0.7 * t; g = 0.0; b = 1.0;
    } else if (lambda < 490.0) {
        t = (lambda - 440.0) / 50.0;
        r = 0.0; g = t; b = 1.0;
    } else if (lambda < 510.0) {
        t = (lambda - 490.0) / 20.0;
        r = 0.0; g = 1.0; b = 1.0 - t;
    } else if (lambda < 580.0) {
        t = (lambda - 510.0) / 70.0;
        r = t; g = 1.0; b = 0.0;
    } else if (lambda < 645.0) {
        t = (lambda - 580.0) / 65.0;
        r = 1.0; g = 1.0 - t; b = 0.0;
    } else {
        r = 1.0; g = 0.0; b = 0.0;
    }
    return vec3(r, g, b);
}

// Cauchy dispersion anchored to ubo.nBase at 589 nm (sodium D line)
float waterN(float lambda_nm) {
    float L    = lambda_nm / 1000.0;        // nm → µm
    const float L589 = 0.589;
    float disp = 6.5e-3 / (L * L) - 6.5e-3 / (L589 * L589);
    return ubo.nBase + disp;
}

// Rainbow angle (radians) from the anti-solar point, for a given order.
// order=1 → primary (~42°), order=2 → secondary (~51°)
float rainbowAngle(float n, int order) {
    float bcSq;
    if (order == 1) {
        bcSq = (4.0 - n * n) / 3.0;
    } else {
        bcSq = (9.0 - n * n) / 8.0;
    }
    if (bcSq <= 0.0 || bcSq >= 1.0) return -1.0;
    float bc = sqrt(bcSq);
    float i  = asin(bc);
    float r  = asin(bc / n);
    if (order == 1) {
        return 4.0 * r - 2.0 * i;           // primary: 4r − 2i ≈ 42.6°
    } else {
        return PI - 6.0 * r + 2.0 * i;      // secondary: π − 6r + 2i ≈ 50.5°
    }
}

void main() {
    // ── Reconstruct world-space view direction ───────────────────────────────
    vec2 ndc = inUV * 2.0 - 1.0;
    vec4 nearW = ubo.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farW  = ubo.invViewProj * vec4(ndc, 1.0, 1.0);
    nearW /= nearW.w;
    farW  /= farW.w;
    vec3 viewDir = normalize(farW.xyz - nearW.xyz);

    // ── Sun direction (needed for atmosphere) ────────────────────────────────
    vec3  sunN    = normalize(ubo.sunDir.xyz);

    // ── Sky background — couleur atmosphérique selon l'élévation solaire ────
    // sunN.y = sin(elevation) : 0 = horizon, ~0.87 = 60°, <0 = nuit
    float elev = sunN.y;

    // Palettes horizon / zénith selon la hauteur du soleil
    vec3 horizonDay    = vec3(0.72, 0.88, 1.00);   // bleu clair de jour
    vec3 horizonSunset = vec3(0.98, 0.62, 0.18);   // orange chaud
    vec3 horizonNight  = vec3(0.04, 0.07, 0.20);   // bleu nuit profond

    vec3 zenithDay     = vec3(0.12, 0.28, 0.78);   // bleu profond midi
    vec3 zenithSunset  = vec3(0.20, 0.12, 0.42);   // violet crépuscule
    vec3 zenithNight   = vec3(0.01, 0.02, 0.10);   // quasi noir

    float tDay   = smoothstep(0.0, 0.30, elev);    // 0 = horizon/nuit, 1 = soleil haut
    float tNight = smoothstep(-0.05, -0.22, elev); // 1 = pleine nuit, 0 = jour

    vec3 horizonCol = mix(mix(horizonSunset, horizonDay, tDay), horizonNight, tNight);
    vec3 zenithCol  = mix(mix(zenithSunset,  zenithDay,  tDay), zenithNight,  tNight);

    float skyBlend = smoothstep(-0.05, 0.4, viewDir.y);
    vec3  skyColor = mix(horizonCol, zenithCol, skyBlend);

    // Lueur de lever/coucher de soleil autour de l'azimut solaire
    float sunsetStr = smoothstep(0.28, 0.0, elev) * smoothstep(-0.18, 0.0, elev);
    if (sunsetStr > 0.001) {
        vec2 viewH = normalize(vec2(viewDir.x, viewDir.z) + vec2(1e-6));
        vec2 sunH  = normalize(vec2(sunN.x, sunN.z) + vec2(1e-6));
        float azimAlign  = dot(viewH, sunH);              // 1 = vers le soleil
        float nearHorizon = smoothstep(0.22, 0.0, abs(viewDir.y));
        float glow = pow(max(0.0, azimAlign), 3.0) * nearHorizon * sunsetStr;
        skyColor += vec3(1.0, 0.38, 0.04) * glow * 2.2;
    }

    // Ground (disparait progressivement avec l'altitude)
    if (viewDir.y < 0.0) {
        float altFade = 1.0 - clamp(ubo.altitude / 2000.0, 0.0, 1.0);
        float g = smoothstep(0.0, -0.12, viewDir.y) * altFade;
        // Teinte du sol selon heure du jour
        vec3 groundDay    = vec3(0.22, 0.32, 0.12);
        vec3 groundSunset = vec3(0.32, 0.22, 0.08);
        vec3 groundCol    = mix(groundSunset, groundDay, tDay);
        skyColor = mix(skyColor, groundCol, g);
    }

    // Sun disc
    float sunCos  = dot(viewDir, sunN);
    float sunAng  = acos(clamp(sunCos, -1.0, 1.0));
    float sunDisc = smoothstep(0.012, 0.0, sunAng);
    skyColor      = mix(skyColor, vec3(2.2, 1.9, 1.3), sunDisc);

    // ── Rainbow arcs ─────────────────────────────────────────────────────────
    vec3  antiSolar = -sunN;
    float cosTheta  = dot(viewDir, antiSolar);
    float theta     = acos(clamp(cosTheta, -1.0, 1.0)); // angle from anti-solar

    vec3 rainbow = vec3(0.0);
    int  N       = clamp(ubo.numWavelengths, 4, 32);

    for (int i = 0; i < N; i++) {
        float t      = float(i) / float(N - 1);
        float lambda = mix(380.0, 700.0, t);
        float n      = waterN(lambda);
        vec3  col    = wavelengthToRGB(lambda);

        // Primary
        if (ubo.showPrimary != 0) {
            float thetaR = rainbowAngle(n, 1);
            if (thetaR > 0.0) {
                float dTheta  = theta - thetaR;
                float sigma   = 0.008 / ubo.dropletRadius; // larger drops → narrower arc
                float intensity = exp(-dTheta * dTheta / (sigma * sigma)) * ubo.primaryIntensity;
                rainbow += col * max(intensity, 0.0);
            }
        }

        // Secondary (broader, reversed color order)
        if (ubo.showSecondary != 0) {
            float thetaR = rainbowAngle(n, 2);
            if (thetaR > 0.0) {
                float dTheta    = theta - thetaR;
                float sigma     = 0.012 / ubo.dropletRadius;
                float intensity = exp(-dTheta * dTheta / (sigma * sigma)) * ubo.secondaryIntensity;
                rainbow += col * max(intensity, 0.0);
            }
        }
    }

    rainbow /= float(N);

    // A altitude 0 : arc limité au-dessus de l'horizon.
    // En montant, l'horizon se creuse jusqu'à laisser voir le cercle complet (~3000 m).
    float horizonDip = -clamp(ubo.altitude / 3000.0, 0.0, 1.0);
    float visible = smoothstep(horizonDip - 0.04, horizonDip + 0.04, viewDir.y);
    rainbow *= visible;

    // ── Final composite ──────────────────────────────────────────────────────
    outColor = vec4(skyColor + rainbow, 1.0);
}
