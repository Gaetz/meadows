// Height-based layer blending (Drobot GDC10 / Mishkinis; brief §5 phase 1):
// within the transition band, the material whose displaced height wins
// claims the pixel — gravel pokes through sand instead of cross-fading.
// depth = band thickness below the max (uSplatDetailInfo.x); smaller =
// crisper interfaces. depth <= 0 falls back to the plain weighted blend.
// Layers with negligible input weight are excluded (their heights are
// never fetched — see terrain.frag).

const int kSplatLayers = 5;
const float kSplatWeightEps = 1.0e-3;

// h[i] = displacement of layer i (0 where w[i] ~ 0), w[i] = rule weights.
// Writes the blended weights to b[] and returns their sum (normalizer).
float blendHeights(const float h[kSplatLayers], const float w[kSplatLayers],
                   float depth, out float b[kSplatLayers]) {
    float lifted[kSplatLayers];
    float maxH = 0.0;
    for (int i = 0; i < kSplatLayers; ++i) {
        lifted[i] = h[i] + w[i];
        if (w[i] > kSplatWeightEps) {
            maxH = max(maxH, lifted[i]);
        }
    }
    maxH -= depth;
    float sum = 0.0;
    for (int i = 0; i < kSplatLayers; ++i) {
        b[i] = w[i] > kSplatWeightEps ? max(lifted[i] - maxH, 0.0) : 0.0;
        sum += b[i];
    }
    return max(sum, 1.0e-5);
}
