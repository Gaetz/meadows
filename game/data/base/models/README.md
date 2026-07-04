# Authored models (brick 23)

- `rock_cc0.gltf` + `moon_rock_02.bin` — "Moon Rock 02" by James Ray Cock,
  Poly Haven (https://polyhaven.com/a/moon_rock_02), CC0. Geometry only: the
  landscape pipeline is untextured (vertex colors), so the referenced
  `textures/*.jpg` are intentionally not shipped and never fetched.
  Loaded in `LandscapeScene::onEnter` as rock variant 0 via
  `assets::loadGltfMesh` + `VegetationSystem::overrideVariantMesh`;
  if the file is missing the procedural rock silently takes its place.
