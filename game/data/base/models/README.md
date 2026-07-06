# Authored models

- `character_cc0.glb` (chantier 1, B2) — "Fox" by PixelMannen (model, CC0),
  rig + animations by @tomkranis (CC-BY 4.0), from
  KhronosGroup/glTF-Sample-Assets. Rigged sample with embedded clips
  (Survey/Walk/Run) proving the skinned pipeline; the demo's real humanoid
  NPC (Quaternius CC0 pack, dev's pick) replaces it in brick B6. Its
  embedded texture is ignored (flat tint — the stylized look).
- `rock_cc0.gltf` + `moon_rock_02.bin` — "Moon Rock 02" by James Ray Cock,
  Poly Haven (https://polyhaven.com/a/moon_rock_02), CC0. Geometry only: the
  landscape pipeline is untextured (vertex colors), so the referenced
  `textures/*.jpg` are intentionally not shipped and never fetched.
  Loaded in `LandscapeScene::onEnter` as rock variant 0 via
  `assets::loadGltfMesh` + `VegetationSystem::overrideVariantMesh`;
  if the file is missing the procedural rock silently takes its place.
