#version 460 core
#include "common.glsl"

// Screen-space contact shadows (the Bend Studio GDC
// technique, reimplemented): from each pixel, march a short distance
// TOWARD the sun in world space; if the depth buffer holds geometry in
// front of a marched probe (within a thickness window, against haloing),
// the pixel sits in a contact shadow the 2048² CSM cannot resolve —
// grass blades, prop feet, NPC soles. Half-res, composited by the
// tonemap as a multiplier (the SSAO pattern). The scene skips this pass
// and clears the target to WHITE when the toggle is off (no free
// FrameUbo slot for a flag).
layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
// Scene color for its ALPHA flag (0 = grass): grass neither receives
// nor casts contact shadows — the meadow reads as one flat mass.
layout(binding = 2) uniform sampler2D uSceneColor;
#include "shadow.glsl"
#include "stylized.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec3 worldFromDepth(vec2 uv, float depth) {
    // 0..1 clip: the stored depth IS ndc z (no *2-1 remap).
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    // No sun (interiors, night): neutral.
    if (uSunColor.r + uSunColor.g + uSunColor.b <= 0.001 ||
        uSunDirection.y <= 0.0) {
        fragColor = vec4(1.0);
        return;
    }
    // GRAZING-SUN fade: near the horizon the march runs almost parallel
    // to the ground — depth-reconstruction error reads the terrain as
    // its own occluder (a half-res darkening film over everything)
    // while the thickness window EXEMPTS the strip behind raised
    // occluders (inverted bright "shadows" behind rocks). The CSM owns
    // grazing light; contact detail only means something with the sun
    // reasonably up.
    float elevation = smoothstep(0.08, 0.30, uSunDirection.y);
    if (elevation <= 0.001) {
        fragColor = vec4(1.0);
        return;
    }
    float depth = texture(uSceneDepth, vUv).r;
    if (depth >= 0.99995) {
        fragColor = vec4(1.0); // sky
        return;
    }
    if (texture(uSceneColor, vUv).a < 0.5) {
        fragColor = vec4(1.0); // grass receiver: exempt, skip the march
        return;
    }
    vec3 position = worldFromDepth(vUv, depth);
    float dist = distance(position, uCameraPos.xyz);

    // Receiver normal from depth derivatives (half-res, faceted — a
    // GATE, not shading). The direct term contact darkens also carries
    // the stylized DIFFUSE ramp: slopes facing away from a grazing sun
    // sit below the terminator (ndl ~ 0, zero direct) yet pass the CSM
    // gate (nothing occludes them) — un-gated, the march's skimming
    // false hits filmed those ambient-only slopes with a second,
    // darker shadow (and every receiver hole read bright inside it).
    vec3 nrm = normalize(cross(dFdx(position), dFdy(position)));
    nrm *= sign(dot(nrm, uCameraPos.xyz - position));
    float ndl = dot(nrm, uSunDirection.xyz);
    float diffuseGate =
        smoothstep(0.02, 0.15, stylizedDiffuse(ndl, max(ndl, 0.0)));
    if (diffuseGate <= 0.001) {
        fragColor = vec4(1.0);
        return;
    }

    // Reach grows a little with distance so the effect keeps a similar
    // on-screen footprint; thickness bounds what counts as an occluder.
    float reach = 0.45 + dist * 0.01;
    float thickness = 0.35 + dist * 0.01;

    const int kSteps = 12;
    // IGN jitter breaks the marching bands into filterable noise.
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                            0.00583715 * gl_FragCoord.y));
    float stepLen = reach / float(kSteps);
    float shadow = 0.0;
    for (int i = 1; i <= kSteps; ++i) {
        vec3 p = position +
                 uSunDirection.xyz * ((float(i) - 0.5 + jitter) * stepLen);
        vec4 clip = uViewProj * vec4(p, 1.0);
        if (clip.w <= 0.0) {
            break;
        }
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) ||
            any(greaterThan(uv, vec2(1.0)))) {
            break;
        }
        vec3 surface = worldFromDepth(uv, texture(uSceneDepth, uv).r);
        float surfaceDist = distance(surface, uCameraPos.xyz);
        float probeDist = distance(p, uCameraPos.xyz);
        float ahead = probeDist - surfaceDist; // >0: geometry in front
        // Distance-scaled floor (speckle fix): at range the
        // depth reconstruction error alone exceeds a fixed 0.02 m and
        // surfaces self-shadowed into black dots — worst at altitude
        // where everything is far.
        float minAhead = 0.02 + dist * 0.0025;
        if (ahead > minAhead && ahead < thickness &&
            texture(uSceneColor, uv).a > 0.5) { // grass never casts
            shadow = 1.0;
            break;
        }
    }
    // COMPOSITION CONTRACT: every legitimate shadow (CSM, terrain light
    // map, clouds) cuts only the DIRECT sun term; the tonemap applies
    // contact as a fullscreen multiply that would darken the AMBIENT
    // too — a second, darker shadow inside shadows, where every
    // receiver hole (exempt grass, thickness-rejected strips behind
    // rocks) then read as bright anti-shadows. So contact is gated to
    // SUNLIT pixels — it is contact DETAIL the CSM cannot resolve, not
    // a shadow of its own. The reference uses the same STYLIZED shadow
    // mapping the surfaces multiply by (the raw factor compared apples
    // to oranges under a snapped ramp). Offset toward the sun replaces
    // the normal bias (we have no normals here).
    float sun = stylizedShadow(shadowFactor(
        position + uSunDirection.xyz * 0.3, uSunDirection.xyz));
    float gate = smoothstep(0.25, 0.75, sun);
    // Soft floor: contact darkens, never blacks out (the CSM and
    // ambient own the real shadow terms); eased out toward the horizon
    // (grazing fade), inside CSM/terrain shadow (gate) and below the
    // stylized terminator (diffuseGate) — neutral wherever the DIRECT
    // term it belongs to is already zero.
    fragColor = vec4(
        vec3(1.0 - shadow * 0.45 * elevation * gate * diffuseGate), 1.0);
}
