// The prop flag protocol — mirrors engine/render/landscape/PropEncoding.hpp
// constant for constant (change both together). Vertex uv lanes and
// instance aParams lanes carry flags as NEGATIVE values below the ranges
// real data uses; the helpers below are the only decoders.

const float kPropCardFlagBias = -10.0;
const float kPropCardSlotStep = -20.0;
const float kPropCardThreshold = -5.0;
const float kPropCardSlotCount = 8.0; // leaf-mask atlas tiles
const float kPropBarkThreshold = -0.5;

// Vertex: a degenerate billboard leaf card (space-colonization trees).
bool isLeafCard(vec2 uv) {
    return uv.x < kPropCardThreshold;
}

// Vertex: the card's leaf-mask atlas slot (the bias steps per slot).
float cardSlot(vec2 uv) {
    return floor((-uv.x + kPropCardThreshold) / -kPropCardSlotStep);
}

// Vertex: the corner (x halfSize) a card vertex encodes around its
// slot's bias — exactly +-halfSize on each axis.
vec2 cardCorner(vec2 uv, float slot) {
    return vec2(uv.x - kPropCardFlagBias + slot * -kPropCardSlotStep, uv.y);
}

// Vertex: procedural wood carrying the bark flag (non-card vertex).
bool isBark(vec2 uv) {
    return uv.y < kPropBarkThreshold;
}

// Instance: the mesh uv are texture coordinates (bound albedo).
bool isTextured(vec4 params) {
    return params.w < 0.0;
}

// Instance: never sways (rocks, scans).
bool isRigid(vec4 params) {
    return params.z < 0.0;
}

// Instance: the fade-end distance (m), flag stripped.
float propFadeEnd(vec4 params) {
    return abs(params.w);
}

// Far-tree impostor instance (far_tree.vert/frag): a negative width ratio
// flags the conifer silhouette.
bool isConiferImpostor(vec4 params) {
    return params.z < 0.0;
}

// tree.vert -> tree.frag varyings, the same "negative = absent" idiom.
const vec2 kNoCardUv = vec2(-1.0);
const vec2 kNoPropUv = vec2(-10.0);
bool hasCardUv(vec2 cardUv) {
    return cardUv.x >= 0.0;
}
bool hasPropUv(vec2 propUv) {
    return propUv.x > -5.0;
}
// Bark lane packs flag + instance yaw (0 = no bark; 1 + yaw otherwise).
float barkLane(bool bark, float yaw) {
    return bark ? 1.0 + yaw : 0.0;
}
bool hasBark(float lane) {
    return lane > 0.5;
}
float barkYaw(float lane) {
    return lane - 1.0;
}

// Real prop presence at `dist` (1 = drawn, 0 = gone) at the end of its
// reach, under the handoff mode uVegInfo.w selects (requires common.glsl):
// DITHER (w >= 0): the prop keeps its size, the fragment dissolves it by
// screen dither over w metres (capped at 14 % of the reach) while the far
// impostor keeps the complementary pixels (far_tree.*); LEGACY (w < 0):
// tree.vert / shadow_prop.vert scale the prop to its base by this value
// over the last 14 % and the impostor fades in alongside.
bool ditherHandoff() {
    return uVegInfo.w >= 0.0;
}

float propPresence(float fadeEnd, float dist) {
    float band = ditherHandoff() ? min(max(uVegInfo.w, 1.0), fadeEnd * 0.14)
                                 : fadeEnd * 0.14;
    return 1.0 - smoothstep(fadeEnd - band, fadeEnd, dist);
}
