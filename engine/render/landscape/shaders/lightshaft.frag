#version 460 core
#include "common.glsl"

// Dust light shaft (the Skyrim FXShaft model, procedural):
// additive translucent blades, no depth write. Axial fade x radial fade
// x two scrolling noise layers (dust drift along the beam) + a fine
// thresholded sparkle band (the motes).

layout(std140, binding = 1) uniform ShaftUbo {
    vec4 uShaftColor;  // rgb premultiplied color*intensity, a = softness
    vec4 uShaftParams; // x = dust density, y = length (m), z/w free
};

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

// Cheap 2D value noise (hash lattice, bilinear) — enough for dust.
float hash21(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 s = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), s.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), s.x),
               s.y);
}

void main() {
    float axial = vUv.y;            // 0 at the source, 1 at the far end
    float radial = clamp(1.0 - vUv.x * vUv.x, 0.0, 1.0);

    // Axial fade: bright near the source, gone at the end; softness
    // blends between a tight beam and a long haze.
    float softness = uShaftColor.a;
    float fadeTight = pow(1.0 - axial, 2.2);
    float fadeSoft = (1.0 - axial) * (1.0 - axial * 0.5);
    float fade = mix(fadeTight, fadeSoft, clamp(softness, 0.0, 1.0));

    // Dust drift: two noise layers scrolling at different speeds along
    // the beam; their product keeps the shaft alive without strobing.
    float len = max(uShaftParams.y, 1e-3);
    vec2 flow = vec2(vUv.x * 3.0, axial * len * 0.8);
    float dust1 = vnoise(flow * 1.7 + vec2(0.0, -uTime.x * 0.22));
    float dust2 = vnoise(flow * 4.3 + vec2(3.7, -uTime.x * 0.55));
    float dust = mix(1.0, dust1 * 0.7 + dust2 * 0.5,
                     clamp(uShaftParams.x, 0.0, 1.0));

    // Motes: a fine band thresholded high — sparse bright specks that
    // drift with the slow layer.
    float fine = vnoise(flow * 11.0 + vec2(9.1, -uTime.x * 0.35));
    float motes = smoothstep(0.82, 0.95, fine) * uShaftParams.x;

    vec3 beam = uShaftColor.rgb *
                (fade * radial * radial * (dust * 0.55 + motes * 1.4));
    fragColor = vec4(beam, 1.0); // additive: rgb adds, alpha unused
}
