// The fire field's scorch mask (FireScorchMap): one texel per 2 m cell
// of the fire window, 0 untouched .. 1 burnt; cell (col, row) is
// centered on origin + (col, row) * texel. uFireScorchInfo = {originX,
// originZ, 1/texel, n} — n = 0 while nothing burnt.
layout(binding = 10) uniform sampler2D uFireScorch;

float fireScorchAt(vec2 xz) {
    if (uFireScorchInfo.w < 0.5) {
        return 0.0;
    }
    vec2 uv = ((xz - uFireScorchInfo.xy) * uFireScorchInfo.z + 0.5) /
              uFireScorchInfo.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 0.0;
    }
    return texture(uFireScorch, uv).r;
}
