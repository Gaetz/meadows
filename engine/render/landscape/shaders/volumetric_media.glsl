// Shared participating-media math (Schneider/Frostbite family) for the
// raymarched volumetrics: ground mist today (mist.frag), volumetric sky
// clouds tomorrow (docs/RENDERING.md §8). Pure functions only — no
// uniforms, no samplers; each consumer supplies its own density field.

// Henyey-Greenstein phase lobe. g > 0 forward-scatters (silver lining
// toward the sun), g < 0 back-scatters.
float mediaHgPhase(float mu, float g) {
    float gg = g * g;
    return (1.0 - gg) /
           (12.566371 * pow(1.0 + gg - 2.0 * g * mu, 1.5)); // 4*pi
}

// Dual-lobe blend: forward silver lining + a soft backscatter floor so
// the medium never goes black side-on.
float mediaDualLobeHg(float mu, float gForward, float gBack, float wBack) {
    return mix(mediaHgPhase(mu, gForward), mediaHgPhase(mu, gBack), wBack);
}

// Schneider "powder" term: brightens optically thin edges that plain
// Beer-Lambert renders too dark (in-scattering from the surface layer).
float mediaPowder(float tau) {
    return 1.0 - exp(-2.0 * tau);
}

// Schneider erosion remap: carve `erosion` out of a [0,1] envelope while
// keeping the dense core at 1 (remap(v, e, 1, 0, 1), clamped).
float mediaErosionRemap(float value, float erosion) {
    return clamp((value - erosion) / max(1.0 - erosion, 1e-3), 0.0, 1.0);
}

// Multi-octave scattering approximation (Wrenninge, via Fewes): sums
// progressively attenuated Beer terms with progressively isotropic phase
// — the cheap stand-in for multiple scattering that keeps dense media
// luminous from within. The sky clouds' light march will use it; the
// ground mist's analytic sun path can too.
float mediaMultiOctaveScattering(float tau, float mu, float g) {
    float a = 1.0; // extinction attenuation per octave
    float b = 1.0; // contribution per octave
    float c = 1.0; // phase eccentricity per octave
    float luminance = 0.0;
    for (int i = 0; i < 4; ++i) {
        luminance += b * mediaHgPhase(mu, g * c) * exp(-tau * a);
        a *= 0.2;
        b *= 0.2;
        c *= 0.5;
    }
    return luminance;
}

// --- Analytic 3D value noise (the erosion fallback when no baked noise
// volume is bound; NoiseVolume replaces these taps with texture fetches).

vec3 mediaHash3(vec3 p) {
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}

float mediaValueNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = mediaHash3(i + vec3(0, 0, 0)).x;
    float n100 = mediaHash3(i + vec3(1, 0, 0)).x;
    float n010 = mediaHash3(i + vec3(0, 1, 0)).x;
    float n110 = mediaHash3(i + vec3(1, 1, 0)).x;
    float n001 = mediaHash3(i + vec3(0, 0, 1)).x;
    float n101 = mediaHash3(i + vec3(1, 0, 1)).x;
    float n011 = mediaHash3(i + vec3(0, 1, 1)).x;
    float n111 = mediaHash3(i + vec3(1, 1, 1)).x;
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// Two octaves: enough structure to break the envelope without eating the
// per-step budget (the baked noise volume carries the fine detail later).
float mediaFbm3(vec3 p) {
    return mediaValueNoise3(p) * 0.667 + mediaValueNoise3(p * 2.7) * 0.333;
}
