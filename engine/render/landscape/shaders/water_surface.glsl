#include "common.glsl"
#include "sky.glsl"

// Pre-water scene snapshot (copied between the opaque and water passes —
// sampling a bound attachment would be undefined).
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uSceneDepth;
// Half-res mirrored scene (uTerrainInfo.w = 1 when valid this frame). For
// points on the water plane the mirror camera projects to the SAME screen
// UV, so the fragment's own UV samples its reflection.
layout(binding = 2) uniform sampler2D uReflection;
// CPU-baked pool depth around the camera (pre-dilated neighborhood max) —
// view-independent, unlike screen-space probing.
layout(binding = 3) uniform sampler2D uPoolDepth;
// Main-view volumetric sky clouds (rgb + transmittance a, last frame's
// display buffer) — reprojected into the mirror, see below.
layout(binding = 4) uniform sampler2D uSkyClouds;
// Camera-local water-info map (WaterInfoMap, CPU-baked): A = surface Y
// (-1e6 dry), B = depth / flow XZ / spare. Junctions and ribbon
// overlaps resolve per PIXEL against this — the Unreal Water model.
layout(binding = 5) uniform sampler2D uWaterInfoA;
layout(binding = 6) uniform sampler2D uWaterInfoB;
// Live sim window (WaterSystem::updateSim): A = display level (wet
// surface, or ground minus a tuck where dry), B = depth / current XZ /
// spare. uWaterSimMapInfo maps world -> window uv.
layout(binding = 7) uniform sampler2D uWaterSimA;
layout(binding = 8) uniform sampler2D uWaterSimB;

// World XZ -> texel-centered uv of the sim textures (n nodes = n
// texels over the window span).
vec2 waterSimUv(vec2 worldXz) {
    vec2 rel = (worldXz - uWaterSimMapInfo.xy) * uWaterSimMapInfo.z;
    vec2 texSize = vec2(textureSize(uWaterSimB, 0));
    return (rel * (texSize - 1.0) + 0.5) / texSize;
}

#ifdef WATER_LOCAL
// Water material presets (WaterSystem::rebuildMaterials — std140
// lockstep): slot 0 = default water, whose values ARE the constants the
// sea path uses below.
struct WaterMaterial {
    vec4 tintStrength;      // rgb tint, w mix strength
    vec4 deepEmissive;      // rgb deep color, w emissive strength
    vec4 absorptionFlow;    // rgb absorption, w flow-speed scale
    vec4 foamWave;          // rgb foam color, w wave scale
    vec4 emissiveViscosity; // rgb emissive color, w viscosity
    vec4 extras;            // x foam gain
};
layout(std140, binding = 1) uniform WaterMaterialsUbo {
    WaterMaterial uWaterMaterials[16];
};
#endif

#include "view_util.glsl"

layout(location = 0) in vec3 vWorldPos;
#if defined(WATER_LOCAL) && !defined(WATER_SIM)
// Local surfaces (lakes/rivers, waterlocal.vert): the per-surface
// character the shading below specializes on.
layout(location = 1) in vec2 vFlow; // direction * speed; (0,0) = lake
layout(location = 2) in vec4 vInfo; // halfWidth (0 = lake), lateral, arc, endDist
layout(location = 3) flat in float vMaterial; // preset slot
#endif
#ifdef WATER_SIM
// Sim sheet (water_sim.vert): per-fragment character comes from the
// sim textures instead of vertex attributes — same NAMES so the whole
// WATER_LOCAL shading below runs unchanged.
layout(location = 1) in vec2 vSimUv;
vec2 vFlow;
vec4 vInfo;
float vMaterial;
#endif
layout(location = 0) out vec4 fragColor;

// Scrolling multi-octave wave normal, analytic derivatives (no texture).
vec3 waveNormal(vec2 p, float t) {
    vec2 grad = vec2(0.0);
    grad.x += 0.9 * cos(p.x * 0.23 + p.y * 0.11 + t * 0.85);
    grad.y += 0.7 * cos(p.y * 0.19 - p.x * 0.07 + t * 0.63);
    grad.x += 0.45 * cos(p.x * 0.71 - p.y * 0.33 - t * 1.7);
    grad.y += 0.45 * cos(p.y * 0.83 + p.x * 0.29 + t * 1.9);
    grad += 0.22 * vec2(cos(p.x * 2.3 + t * 3.1), cos(p.y * 2.1 - t * 2.7));
    // Weather chop (uWindInfo.z): storm whips the slopes up, calm flattens
    // the surface toward a mirror.
    float slope = 0.045 * uWindInfo.z;
    return normalize(vec3(-grad.x * slope, 1.0, -grad.y * slope));
}

void main() {
    vec2 screenUv = gl_FragCoord.xy * uScreenInfo.zw;
    float t = uWindInfo.x; // wind-scaled phase: waves slow down in a calm
#ifdef WATER_SIM
    // Character from the sim textures. The EXTENT of the water is the
    // MESH (docs/WATER-RENDER.md §1.2): no dryness discard here, ever
    // — threshold discards blinked whole surfaces out (measured).
    vec4 simTexB = texture(uWaterSimB, vSimUv);
    float simDepth = max(simTexB.x, 0.01); // the SIMULATED column
    vFlow = simTexB.yz;
    vInfo = vec4(dot(vFlow, vFlow) > 0.09 ? 4.0 : 0.0, 0.0, 0.0, 1.0e6);
    vMaterial = 0.0;
    // Trusted-rim fade: 0 at the margin boundary, 1 a fade-band inside
    // — the baked bodies own everything past it.
    vec2 simRel =
        (vWorldPos.xz - uWaterSimMapInfo.xy) * uWaterSimMapInfo.z;
    float simEdgeM = min(min(simRel.x, 1.0 - simRel.x),
                         min(simRel.y, 1.0 - simRel.y)) /
                     uWaterSimMapInfo.z;
    float simFade = smoothstep(uWaterSimTuneInfo.x,
                               uWaterSimTuneInfo.x + uWaterSimTuneInfo.y,
                               simEdgeM);
    if (simFade <= 0.001) {
        discard;
    }
#endif
#if defined(WATER_LOCAL) && !defined(WATER_SIM)
    // Inside the sim window the LIVE volumes are the ONLY water. The
    // first arbitration was per texel ("yield where the sim has water
    // here") — but the sim's courses legitimately differ from the
    // baked ones, so wherever they disagreed BOTH networks showed:
    // baked ribbons snaking up the hillsides beside the sim's valley
    // water (measured in-game). The baked bodies are DATA for the sim
    // (pinned lakes, entry sources), never geometry, anywhere inside
    // the trusted rect; the crossfade band ties the two worlds.
    {
        int simMode = int(uWaterSimTuneInfo.z + 0.5);
        if (uWaterSimMapInfo.w > 0.5 && simMode != 1) {
            vec2 rel = (vWorldPos.xz - uWaterSimMapInfo.xy) *
                       uWaterSimMapInfo.z;
            if (all(greaterThan(rel, vec2(0.0))) &&
                all(lessThan(rel, vec2(1.0)))) {
                float edgeM = min(min(rel.x, 1.0 - rel.x),
                                  min(rel.y, 1.0 - rel.y)) /
                              uWaterSimMapInfo.z;
                if (edgeM > uWaterSimTuneInfo.x +
                                uWaterSimTuneInfo.y * 0.5) {
                    discard;
                }
            }
        }
    }
#endif
#ifdef WATER_LOCAL
    // Rivers: the wave field is ADVECTED downstream (current, not wind)
    // and its ripples shrink with the channel — a 4 m creek carries
    // wavelets, not ocean swell. Lakes keep the still-water field.
    // Where the water-info map is valid and agrees this fragment IS the
    // local surface, its composited flow replaces the per-ribbon flow:
    // two crossing ribbons then shade identically and their overlap
    // becomes invisible.
    vec2 flowVec = vFlow;
    vec2 infoUv =
        (vWorldPos.xz - uWaterInfoMapInfo.xy) * uWaterInfoMapInfo.z + 0.5;
    bool infoValid = uWaterInfoMapInfo.w > 0.5 &&
                     all(greaterThan(infoUv, vec2(0.002))) &&
                     all(lessThan(infoUv, vec2(0.998)));
    if (infoValid) {
        float infoSurface = texture(uWaterInfoA, infoUv).r;
        if (abs(infoSurface - vWorldPos.y) < 0.35) {
            vec2 infoFlow = texture(uWaterInfoB, infoUv).yz;
            if (dot(infoFlow, infoFlow) > 1.0e-6) {
                flowVec = infoFlow;
            }
        }
    }
    float flowSpeed = length(flowVec);
    float riverness = step(0.001, vInfo.x);
    WaterMaterial mtl = uWaterMaterials[int(vMaterial + 0.5)];
    vec3 mDeep = mtl.deepEmissive.rgb;
    vec3 mAbsorption = mtl.absorptionFlow.rgb;
    vec3 mFoam = mtl.foamWave.rgb;
    float mFoamGain = mtl.extras.x;
    // Torrent factor: how fast the surface plunges (screen derivatives)
    // — 0 on calm reaches, 1 on waterfall-grade drops. Drives the
    // mountain-stream look: faster advection, choppier finer ripples,
    // milky aerated water, streaked whitewater.
    vec2 sgx = vec2(dFdx(vWorldPos.x), dFdx(vWorldPos.z));
    vec2 sgy = vec2(dFdy(vWorldPos.x), dFdy(vWorldPos.z));
    float sHoriz = max(length(sgx) + length(sgy), 1e-3);
    float sVert = abs(dFdx(vWorldPos.y)) + abs(dFdy(vWorldPos.y));
    float torrent = riverness * smoothstep(0.12, 0.5, sVert / sHoriz);
    float rippleScale =
        mix(1.0, mix(2.8, 1.2, smoothstep(2.0, 18.0, vInfo.x)),
            riverness) *
        (1.0 + 0.7 * torrent) * mtl.foamWave.w;
    // TWO-PHASE flow mapping (the Waterways/Valve trick): straight
    // advection stretches the field forever; sampling it at two phases
    // half a cycle apart and blending on a triangle wave resets the
    // stretch invisibly.
    vec3 n;
    if (riverness > 0.5) {
        // Composed flow force (the Waterways model, analytic): base +
        // steepness, SLOWER near the banks (drag) — the mid-channel
        // visibly outruns the edges.
        float lateralProfile = 0.55 + 0.45 * (1.0 - vInfo.y * vInfo.y);
        float speed = (1.6 + 2.6 * torrent) * lateralProfile *
                      mtl.absorptionFlow.w *
                      (1.0 - 0.6 * mtl.emissiveViscosity.w);
        float cycle = 3.0; // seconds per phase reset
        float phase = fract(uTime.x / cycle);
        vec2 advA = flowVec * speed * phase * cycle;
        vec2 advB = flowVec * speed * (fract(phase + 0.5) - 0.5) * cycle;
        vec3 nA = waveNormal((vWorldPos.xz - advA) * rippleScale,
                             t * (1.0 + torrent));
        vec3 nB = waveNormal((vWorldPos.xz - advB) * rippleScale + 37.0,
                             t * (1.0 + torrent) + 11.0);
        n = normalize(mix(nA, nB, abs(phase * 2.0 - 1.0)));
        // Narrow water never shows deep-swell slopes — but a torrent
        // stays visibly agitated.
        n = normalize(mix(n, vec3(0.0, 1.0, 0.0),
                          0.35 * (1.0 - 0.7 * torrent)));
    } else {
        n = waveNormal(vWorldPos.xz * rippleScale, t);
    }
    // Ripple LOD (Waterways' lod0_distance): distant local water flattens
    // toward a calm mirror instead of shimmering sub-pixel detail.
    float lodFade = smoothstep(150.0, 45.0,
                               distance(uCameraPos.xyz, vWorldPos));
    n = normalize(mix(vec3(0.0, 1.0, 0.0), n,
                      mix(0.3, 1.0, lodFade)));

#ifdef WATER_SIM
    // Wall factor, computed at TOP level (uniform control flow keeps
    // the derivatives defined) and BEFORE the underside branch: only
    // GENUINELY steep facets count (a draped rapid at 45-60° must stay
    // a water surface — the loose threshold striped whole hillsides).
    // Degenerate derivatives are treated as flat (normalize would NaN
    // and poison the bloom chain tile by tile — the black rectangles).
    vec3 gpx = dFdx(vWorldPos);
    vec3 gpy = dFdy(vWorldPos);
    vec3 crossN = cross(gpy, gpx);
    float crossLen = length(crossN);
    float simWallness =
        crossLen > 1.0e-9
            ? 1.0 - smoothstep(0.15, 0.40, abs(crossN.y / crossLen))
            : 0.0;

    // Underside: only for surface-like fragments — a WALL below eye
    // height must not flip to the seen-from-below look (it cut every
    // waterfall with a horizontal seam at camera height).
    if (uCameraPos.y < vWorldPos.y - 0.02 && simWallness < 0.5) {
#else
    // Underside: the camera is below THIS surface's level.
    if (uCameraPos.y < vWorldPos.y - 0.02) {
#endif
#else
    vec3 n = waveNormal(vWorldPos.xz, t);
    // Sea path: the default-water constants (slot 0 of the presets).
    vec3 mDeep = vec3(0.008, 0.045, 0.055);
    vec3 mAbsorption = vec3(0.42, 0.16, 0.12);
    vec3 mFoam = vec3(0.75, 0.82, 0.85);
    float mFoamGain = 1.0;

    // Underside: the camera is below the surface, looking up through it.
    if (uCameraPos.y < uTerrainInfo.x) {
#endif
        vec3 upViewDir = normalize(vWorldPos - uCameraPos.xyz);
        vec3 nDown = -n; // face the submerged viewer
        // The world above shows through, wobbled by the waves; grazing
        // angles bend into the deep tint (cheap total internal reflection).
        vec2 aboveUv = screenUv + n.xz * 0.035;
        vec3 above = texture(uSceneColor, aboveUv).rgb *
                     vec3(0.70, 0.95, 1.00);
        float internal =
            pow(1.0 - max(dot(-upViewDir, nDown), 0.0), 3.0);
        vec3 color = mix(above, vec3(0.006, 0.040, 0.048),
                         clamp(internal * 0.9, 0.0, 1.0));
        fragColor = vec4(color, 1.0);
        return;
    }

    // Water thickness along the view ray, from the scene depth snapshot.
    vec3 floorWorld =
        worldFromDepth(screenUv, texture(uSceneDepth, screenUv).r);
    float thickness = max(distance(uCameraPos.xyz, floorWorld) -
                              distance(uCameraPos.xyz, vWorldPos),
                          0.0);

    // Refraction: scene color behind the surface, wobbled by the waves
    // (distortion grows with depth, none at the waterline so shores stay
    // glued), then absorbed toward the deep tint (red dies first).
    vec2 refractionUv =
        screenUv + n.xz * (0.018 * clamp(thickness * 0.5, 0.0, 1.0));
    vec3 refracted = texture(uSceneColor, refractionUv).rgb;
    vec3 absorption = exp(-thickness * mAbsorption);
    vec3 transmitted = mix(mDeep, refracted, absorption);
#ifdef WATER_LOCAL
    // Aerated torrent water is milky, not glassy.
    float milkGate = 1.0;
#ifdef WATER_SIM
    // Aeration needs SPEED: a wide slow glide down a terraced slope
    // read as one white plastic sheet (measured in-game) — only fast
    // water whips air in.
    milkGate = smoothstep(0.8, 2.5, flowSpeed);
#endif
    transmitted = mix(transmitted, vec3(0.52, 0.66, 0.70),
                      torrent * 0.55 * milkGate);
    // Preset tint (mud, enchanted pools) — strength 0 on plain water.
    transmitted =
        mix(transmitted, mtl.tintStrength.rgb, mtl.tintStrength.w);
#endif
#ifdef WATER_SIM
    // The From Dust BODY: color from the SIMULATED column, not only
    // the view-ray optical thickness — a 20 cm flow reads as a body
    // of water, not a transparent film. Shallow water is a vivid
    // turquoise, deep water falls to the dark deep color.
    vec3 bodyColor = mix(vec3(0.10, 0.34, 0.42), mDeep,
                         clamp(simDepth * 0.18, 0.0, 1.0));
    float bodyMix = 1.0 - exp(-simDepth * 1.6);
    transmitted = mix(transmitted, bodyColor, bodyMix * 0.75);
#endif

    // Reflection: the mirrored scene (terrain, trees, sky + sun glints all
    // included), wobbled by the waves; falls back to the analytic sky when
    // the planar pass is off.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    vec3 reflected;
#ifdef WATER_LOCAL
    // The planar mirror is rendered for the SEA plane; a lake at 700 m
    // or a river ribbon would sample garbage — analytic sky instead
    // (the cloud reprojection below still applies, it is directional).
    if (false) {
#else
    if (uTerrainInfo.w > 0.5) {
#endif
        vec2 reflectionUv = clamp(screenUv + n.xz * 0.035, vec2(0.001),
                                  vec2(0.999));
        reflected = texture(uReflection, reflectionUv).rgb;
    } else {
        vec3 reflectDir = reflect(viewDir, n);
        reflectDir.y = abs(reflectDir.y); // never reflect below the horizon
        reflected = skyWithSun(reflectDir);
    }
    // Volumetric sky clouds live in a main-view screen buffer
    // (composited by the tonemap), so the mirrored scene never contains
    // them. Clouds sit at quasi-infinity, so DIRECTION alone reprojects
    // them: follow the reflected ray from the main camera and composite
    // the cloud buffer where it lands on-screen. Off-screen (or with
    // the volumetric pass off) the 2D dome layer the reflection pass
    // drew remains — same coverage field, so the patterns agree.
    if (uCloudVolInfo.x > 0.5) {
        vec3 reflectDir = reflect(viewDir, n);
        reflectDir.y = abs(reflectDir.y);
        vec4 clip =
            uViewProj * vec4(uCameraPos.xyz + reflectDir * 40000.0, 1.0);
        if (clip.w > 0.0) {
            vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
            vec2 edge = min(uv, 1.0 - uv);
            float onScreen = smoothstep(0.0, 0.06, min(edge.x, edge.y));
            if (onScreen > 0.0) {
                vec4 cloud = texture(uSkyClouds, uv);
                reflected = mix(reflected,
                                reflected * cloud.a + cloud.rgb,
                                onScreen);
            }
        }
    }
    float fresnel =
        0.02 + 0.98 * pow(1.0 - max(dot(-viewDir, n), 0.0), 5.0);
    // Dialed down so the reflection reads as water, not as a second world.
    fresnel *= 0.75;
#ifdef WATER_SIM
    // The sim sheet has no planar mirror (analytic sky only): a full
    // fresnel painted whole rivers sky-white at grazing angles — the
    // BODY must dominate, the sky is a glaze.
    fresnel *= 0.6;
#endif

    vec3 color = mix(transmitted, reflected, fresnel);
#ifdef WATER_SIM
    // Side WALLS: genuinely steep facets (flood fronts, fall lips, the
    // dive into a bank) are the water body seen from the side — an
    // opaque wall, never a grazing mirror. Streaks run ALONG the local
    // fall (lanes across the current), and the white only comes with
    // SPEED — a slow wall stays body-colored.
    if (simWallness > 0.001) {
        vec2 fallDir =
            flowSpeed > 1.0e-3 ? vFlow / flowSpeed : vec2(0.0, 1.0);
        float streak =
            0.5 + 0.5 * sin(dot(vWorldPos.xz,
                                vec2(-fallDir.y, fallDir.x)) *
                                2.8 +
                            vWorldPos.y * 1.3 -
                            t * (3.0 + 2.0 * flowSpeed));
        float aeration =
            smoothstep(1.0, 4.0, flowSpeed + simDepth * 0.5);
        vec3 wall = mix(bodyColor, mFoam,
                        (0.10 + 0.45 * streak) * aeration);
        color = mix(color, wall, simWallness * 0.85);
    }
#endif

    // Shore foam: a solid lapping line right at the waterline plus a wider
    // animated fringe further out.
    float band = 1.0 - smoothstep(0.0, 4.5, thickness);
    float waterline = 1.0 - smoothstep(0.0, 0.8, thickness);
    float pattern = 0.5 + 0.5 * sin(thickness * 2.6 - t * 1.8 +
                                    (n.x + n.z) * 22.0);
    float foam = waterline +
                 band * smoothstep(0.30, 0.75, pattern + band * 0.30) * 0.8;
#ifdef WATER_LOCAL
    // Shore foam belongs to the SEA and the BIG lakes only (>= ~0.5 ha,
    // gate baked into the lake quads' free lateral lane): on rivers it
    // whitewashed the shallow channel, and on the junction pools and
    // mountain tarns the lapping ring read as noise. Rivers keep only
    // the torrent whitewater below.
    foam *= (1.0 - riverness) * clamp(vInfo.y, 0.0, 1.0);
#endif

    // Small-pool suppression: ONE tap into the CPU-baked pool-depth map
    // (already a neighborhood max). Shallow-everywhere puddles lose their
    // foam; real shores — deep water nearby — keep it. View-independent:
    // no camera-distance popping.
    vec2 poolUv =
        (vWorldPos.xz - uWaterMapInfo.xy) * uWaterMapInfo.z + 0.5;
    float poolDepth = texture(uPoolDepth, poolUv).r;
    float poolGate = smoothstep(2.0, 4.0, poolDepth);
#ifdef WATER_LOCAL
    // Narrow ribbons fall between the pool map's texels — rivers keep
    // their foam regardless, and earn extra whitewater at the banks and
    // where the surface drops fast (rapids, from screen derivatives).
    foam *= mix(poolGate, 1.0, riverness);
    // Whitewater rides the torrent factor, STREAKED along the current
    // (aerated lanes, not a flat white sheet).
    vec2 flowDirN = flowSpeed > 1e-3 ? flowVec / flowSpeed : vec2(1.0, 0.0);
    vec2 flowPerp = vec2(-flowDirN.y, flowDirN.x);
    float streak =
        0.55 + 0.45 * sin(dot(vWorldPos.xz, flowPerp) * 2.4 +
                          dot(vWorldPos.xz, flowDirN) * 0.35 - t * 3.5);
    float rapids = smoothstep(0.18, 0.55, sVert / sHoriz) * riverness;
    float agitation = 0.35 + 0.65 * smoothstep(0.4, 2.0, flowSpeed);
    foam += riverness * agitation * rapids * (0.25 + 0.45 * streak);
    // (Bank foam removed: two ribbons crossing at a junction each drew
    // their band and the overlap read as chaos — river foam is now the
    // torrent whitewater alone, shore foam is sea + big lakes.)
#else
    foam *= poolGate;
#endif

#ifdef WATER_SIM
    // Meniscus: a thin lapping line where the volume meets the ground
    // (its contact edge) — the torrent whitewater above still stacks.
    foam = max(foam, (1.0 - smoothstep(0.0, 0.6, thickness)) * 0.45);
#endif
    color = mix(color, mFoam, clamp(foam * mFoamGain, 0.0, 1.0));

#ifdef WATER_LOCAL
    // Edge fade (Waterways' ALPHA feather, opaque-pipeline version):
    // where the water thins out — or a ribbon reaches its end — the
    // surface dissolves into the plain refracted ground.
#ifdef WATER_SIM
    // The SIM column counts too: a 10 cm draped stream has almost no
    // view-ray thickness, and the optical-only feather dissolved the
    // whole body color right back into the ground (measured — the
    // "transparent film" look). The feather now only trims the true
    // shoreline edge.
    color = mix(refracted, color,
                smoothstep(0.0, 0.25, max(thickness, simDepth)));
#else
    color = mix(refracted, color,
                smoothstep(0.0, 0.35, thickness) *
                    mix(1.0, smoothstep(0.0, 1.5, vInfo.w /
                                                      max(vInfo.x, 0.5)),
                        riverness));
#endif
    // Preset emissive (lava): glows through the fog like any emitter.
    color += mtl.emissiveViscosity.rgb * mtl.deepEmissive.w;
#ifdef WATER_SIM
    // Trusted-rim crossfade toward the ground (the baked bodies pick
    // the water up past the band), and the seam-overlay debug tint.
    color = mix(refracted, color, simFade);
    if (int(uWaterSimTuneInfo.z + 0.5) == 2) {
        color = mix(color, vec3(1.0, 0.25, 0.2), 0.30);
    }
    // Last-resort sanitize: ONE NaN fragment poisons the bloom chain
    // tile by tile (black rectangles) — never let one out.
    if (any(isnan(color)) || any(isinf(color))) {
        color = vec3(0.02, 0.10, 0.12);
    }
#endif

    // Debug views (render panel > Water > Debug view). Modes 4-6 show
    // the water-info texture channels once that map is bound.
    int waterDebug = int(uWaterDebugInfo.x + 0.5);
    if (waterDebug != 0) {
        vec3 dbg = vec3(0.05);
        if (waterDebug == 1) {
            // Flow: hue by direction, brightness by speed, stripes
            // marching downstream — a still lake reads flat blue.
            vec2 dir = flowSpeed > 1e-3 ? flowVec / flowSpeed : vec2(0.0);
            float stripe =
                0.5 + 0.5 * sin(dot(vWorldPos.xz, dir) * 3.0 -
                                uTime.x * 4.0 * max(flowSpeed, 0.2));
            float energy = clamp(flowSpeed / 3.0, 0.0, 1.0);
            dbg = mix(vec3(0.05, 0.10, 0.45),
                      vec3(0.5 + 0.5 * dir.x, 0.5 + 0.5 * dir.y, 0.2) *
                          mix(0.4, 1.0, stripe),
                      max(energy, riverness * 0.25));
        } else if (waterDebug == 2) {
            dbg = mix(vec3(0.0, 0.15, 0.5), vec3(1.0, 0.1, 0.0),
                      torrent);
        } else if (waterDebug == 3) {
            // Ribbon UV: lateral as green (banks dark), arc as a
            // checker, lakes flat gray.
            float checker = step(0.5, fract(vInfo.z * 0.125));
            dbg = mix(vec3(0.3),
                      vec3(checker, 1.0 - abs(vInfo.y), 1.0 - checker),
                      riverness);
        } else if (waterDebug >= 4) {
            if (!infoValid) {
                dbg = vec3(0.4, 0.0, 0.4); // magenta: no valid map here
            } else if (waterDebug == 4) {
                float s = texture(uWaterInfoA, infoUv).r;
                dbg = s <= -1.0e5
                          ? vec3(0.08, 0.0, 0.12) // dry texel
                          : vec3(fract(s * 0.05), 0.6, 0.25);
            } else if (waterDebug == 5) {
                dbg = mix(vec3(0.02, 0.05, 0.2), vec3(0.2, 0.9, 1.0),
                          clamp(texture(uWaterInfoB, infoUv).x / 8.0,
                                0.0, 1.0));
            } else {
                vec2 f = texture(uWaterInfoB, infoUv).yz;
                float mag = length(f);
                vec2 dir = mag > 1e-4 ? f / mag : vec2(0.0);
                dbg = vec3(0.5 + 0.5 * dir, 0.15) *
                      clamp(mag * 0.5 + 0.15, 0.0, 1.0);
            }
        }
        fragColor = vec4(dbg, 1.0);
        return;
    }
#endif

    fragColor = vec4(applyFog(color, vWorldPos), 1.0);
}
