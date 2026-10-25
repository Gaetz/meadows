// The splat array layer table, the GLSL mirror of SplatTextures.hpp:
// five semantic layers, then the per-family texture variants, then the
// per-pixel deposit layers. Every shader that names a layer reads it
// here; change both files together.
const float kLayerGrass = 0.0;
const float kLayerRock = 1.0;
const float kLayerSnow = 2.0;
const float kLayerSand = 3.0;
const float kLayerCliff = 4.0;
// Variant g (1..) of a family lives at first - 1 + g; the fifth rock
// and fourth snow variants were appended later and sit out of sequence.
const int kGrassVariantFirst = 5;  // 5..7
const int kRockVariantFirst = 8;   // 8..10, then kRockVariantLast
const int kSnowVariantFirst = 11;  // 11..12, then kSnowVariantLast
const int kSandVariantFirst = 13;  // 13..15
const float kScreeLayer = 16.0;    // sand-family talus
const int kCliffVariantFirst = 17; // 17..18
const float kRockVariantLast = 19.0;
const float kSnowVariantLast = 20.0;
const float kFrostGrassLayer = 21.0;
const float kHeathLayerA = 22.0;
const float kHeathLayerB = 23.0;
const float kSteppeGrassLayer = 24.0;
