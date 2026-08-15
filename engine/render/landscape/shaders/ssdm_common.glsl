// Shared SSDM displacement math (docs/GRASS-REDO.md — the Lobel 2008
// scatter). The includer declares uSceneColor (binding 0) and
// uSceneDepth (binding 1) and includes view_util.glsl first.
// Returns the displaced uv DELTA (bounded to kSsdmMaxPx pixels);
// `dispDepth` = the displaced point's reversed-Z depth, the
// nearest-wins key of the resolve.

const float kSsdmMaxPx = 10.0;

// `hMin`/`hMax` clamp the decoded relief before displacing: the flow
// (scatter) uses the full range, the resolve's gather fallback digs
// PITS ONLY (hMax 0) — un-clamped, every coverage hole between
// extruded crests warped inward and high amplitudes read as "the
// texture sinks INTO the trunk".
vec2 ssdmDeltaClamped(vec2 uv, vec2 texel, float hMin, float hMax,
                      out float dispDepth) {
    float d = texture(uSceneDepth, uv).r;
    dispDepth = d;
    float a = texture(uSceneColor, uv).a;
    if (a < 0.5 || a >= 0.995 || d < 1e-8) {
        return vec2(0.0); // grass flag / neutral material / sky
    }
    float h = clamp((a - 0.5) * 2.0 - 0.5, hMin, hMax);
    if (h == 0.0) {
        return vec2(0.0);
    }
    vec3 w = worldFromDepth(uv, d);
    // One-sided neighbor differences toward the depth-closest side —
    // a two-sided difference crosses the silhouette on thin props and
    // the normal blows up exactly where the scatter should extrude.
    float dxp = texture(uSceneDepth, uv + vec2(texel.x, 0.0)).r;
    float dxm = texture(uSceneDepth, uv - vec2(texel.x, 0.0)).r;
    float dyp = texture(uSceneDepth, uv + vec2(0.0, texel.y)).r;
    float dym = texture(uSceneDepth, uv - vec2(0.0, texel.y)).r;
    vec3 dwx =
        abs(dxp - d) <= abs(dxm - d)
            ? worldFromDepth(uv + vec2(texel.x, 0.0), dxp) - w
            : w - worldFromDepth(uv - vec2(texel.x, 0.0), dxm);
    vec3 dwy =
        abs(dyp - d) <= abs(dym - d)
            ? worldFromDepth(uv + vec2(0.0, texel.y), dyp) - w
            : w - worldFromDepth(uv - vec2(0.0, texel.y), dym);
    vec3 n = cross(dwx, dwy);
    float len = length(n);
    if (len < 1e-12) {
        return vec2(0.0);
    }
    n /= len;
    n *= sign(dot(n, uCameraPos.xyz - w));
    vec4 clip = uViewProj * vec4(w + n * (h * uSsaoInfo.z), 1.0);
    if (clip.w <= 0.0) {
        return vec2(0.0);
    }
    dispDepth = clip.z / clip.w;
    vec2 lim = texel * kSsdmMaxPx;
    return clamp(clip.xy / clip.w * 0.5 + 0.5 - uv, -lim, lim);
}

vec2 ssdmDelta(vec2 uv, vec2 texel, out float dispDepth) {
    return ssdmDeltaClamped(uv, texel, -0.5, 0.5, dispDepth);
}
