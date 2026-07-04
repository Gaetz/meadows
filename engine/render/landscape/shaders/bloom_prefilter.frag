#version 460 core

layout(binding = 0) uniform sampler2D uSource;

in vec2 vUv;
out vec4 fragColor;

// Soft-knee threshold: only HDR highlights (sun disc, glints, embers) feed
// the bloom chain; regular scene colors stay out.
void main() {
    vec3 color = texture(uSource, vUv).rgb;
    const float threshold = 1.0;
    const float knee = 0.5;
    float peak = max(color.r, max(color.g, color.b));
    float soft = clamp(peak - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, peak - threshold) / max(peak, 1e-4);
    fragColor = vec4(color * contribution, 1.0);
}
