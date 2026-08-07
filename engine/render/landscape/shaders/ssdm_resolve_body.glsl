// SSDM scatter, pass 3/3 — the RESOLVE (Lobel 2008): every destination
// pixel walks the bounds quadtree top-down, visiting only the nodes
// whose displaced bbox can cover it, and keeps the NEAREST candidate
// (largest reversed-Z displaced depth) — crests genuinely EXTRUDE over
// their neighbors, sky included, and occlude instead of stretching.
// Pixels nobody lands on (dis-occluded pits) fall back to the v1
// gather warp — pits keep digging, and the two directions of relief
// finally coexist.
//
// The chain is resolution-agnostic (flow deltas in UV units, tiles in
// chain pixels): the SAME body runs full-res (into the offscreen) and
// half-res (SSDM_HALF: into the intermediate, alpha = "moved" flag for
// the edge-aware upsample).

vec4 boundsAt(int level, ivec2 t) {
    switch (level) {
    case 4: return texelFetch(uBounds4, t, 0);
    case 3: return texelFetch(uBounds3, t, 0);
    case 2: return texelFetch(uBounds2, t, 0);
    case 1: return texelFetch(uBounds1, t, 0);
    }
    return texelFetch(uBounds0, t, 0);
}

void main() {
    ivec2 pPix = ivec2(gl_FragCoord.xy);
    vec2 pC = vec2(pPix) + 0.5;
    ivec2 res = textureSize(uFlow, 0); // chain pixels
    vec2 fRes = vec2(res);

    float bestDepth = -1.0;
    ivec2 bestLeaf = ivec2(-1);
    // Seed: the level-4 (16 px) tiles whose footprint can reach p
    // (displacement is bounded to kSsdmMaxPx).
    const int kReach = int(kSsdmMaxPx) + 1;
    ivec2 lo = (pPix - kReach) >> 4;
    ivec2 hi = (pPix + kReach) >> 4;
    ivec3 stack[64];
    int sp = 0;
    for (int ty = lo.y; ty <= hi.y; ++ty) {
        for (int tx = lo.x; tx <= hi.x; ++tx) {
            if (tx >= 0 && ty >= 0) {
                stack[sp++] = ivec3(tx, ty, 4);
            }
        }
    }
    for (int iter = 0; iter < 160 && sp > 0; ++iter) {
        ivec3 node = stack[--sp];
        int level = node.z;
        ivec2 levelSize = max(res >> level, ivec2(1));
        if (node.x >= levelSize.x || node.y >= levelSize.y) {
            continue;
        }
        vec4 b = boundsAt(level, node.xy);
        if (pC.x < b.x || pC.y < b.y || pC.x > b.z || pC.y > b.w) {
            continue; // no child can land here
        }
        if (level == 0) {
            vec4 flow = texelFetch(uFlow, node.xy, 0);
            vec2 landed = vec2(node.xy) + 0.5 + flow.rg * fRes;
            if (max(abs(landed.x - pC.x), abs(landed.y - pC.y)) <=
                    0.8 &&
                flow.b > bestDepth) {
                bestDepth = flow.b;
                bestLeaf = node.xy;
            }
        } else if (sp <= 59) {
            ivec2 c = node.xy * 2;
            int child = level - 1;
            stack[sp++] = ivec3(c, child);
            stack[sp++] = ivec3(c + ivec2(1, 0), child);
            stack[sp++] = ivec3(c + ivec2(0, 1), child);
            stack[sp++] = ivec3(c + ivec2(1, 1), child);
        }
    }
    if (bestLeaf.x >= 0) {
        // The leaf is a CHAIN pixel; the scene color is sampled at its
        // normalized position (identical texels at full res).
        vec2 srcUv = (vec2(bestLeaf) + 0.5) / fRes;
        vec4 color = texture(uSceneColor, srcUv);
#ifdef SSDM_HALF
        // Mark moved ONLY on a significant displacement: sub-pixel
        // relief landed on itself, and rewriting it dragged the whole
        // ground down to chain resolution (the "tout flou" verdict).
        vec4 leafFlow = texelFetch(uFlow, bestLeaf, 0);
        float moved =
            length(leafFlow.rg * fRes) > 0.6 ? 1.0 : 0.0;
        fragColor = vec4(color.rgb, moved);
#else
        fragColor = color;
#endif
        return;
    }
    // Hole (everything moved away): the v1 gather digs the pit.
    // Gradients tap the FULL-res depth like the flow pass.
    vec2 sceneTexel = 1.0 / vec2(textureSize(uSceneDepth, 0));
    vec2 q = vUv;
    float unusedDepth;
    for (int i = 0; i < 3; ++i) {
        q = vUv - ssdmDelta(q, sceneTexel, unusedDepth);
    }
    // Silhouette guard: a gathered source from ACROSS a depth edge
    // would duplicate the foreground rim onto the background (the
    // "image doublée derrière la frontière" seen up close on trunks) —
    // keep the original pixel instead of digging.
    {
        float dHere = texture(uSceneDepth, vUv).r;
        float dSrc = texture(uSceneDepth, q).r;
        vec3 wHere = worldFromDepth(vUv, max(dHere, 1.0e-8));
        vec3 wSrc = worldFromDepth(q, max(dSrc, 1.0e-8));
        float viewDist = distance(wHere, uCameraPos.xyz);
        if (distance(wHere, wSrc) > 0.4 + viewDist * 0.02) {
#ifdef SSDM_HALF
            fragColor = vec4(texture(uSceneColor, vUv).rgb, 0.0);
#else
            fragColor = texture(uSceneColor, vUv);
#endif
            return;
        }
    }
#ifdef SSDM_HALF
    float moved = length((q - vUv) * fRes) > 0.6 ? 1.0 : 0.0;
    fragColor = vec4(texture(uSceneColor, q).rgb, moved);
#else
    fragColor = texture(uSceneColor, q);
#endif
}
