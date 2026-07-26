# Audit U3 — Renderer paysage (`engine/render/`)

Scope: `engine/render/` — 2D sprite renderer + the 14-system `landscape/` subtree
(terrain, grass, vegetation, water, sky, shadows/CSM, postfx, two occluders,
splat, tree-gen, mesh-builder, terrain-noise, terrain-lightmap) + 55 GLSL files.
~6168 lines C++. Reference: `docs/archive/3D-RENDERER.md` (bricks 1-34).

## Invariant checks (explicit pass/fail)

- **§2.1 (all GPU access via `rhi::`) — PASS.** Grep for `gl[A-Z]…(` across all of
  `engine/render/` returns **0 hits**. Every system talks only to
  `rhi::Device`/`rhi::CommandBuffer`. No raw GL leaked out of the backend.
- **§2.10 (headless purity) — N/A for this unit** (render/ is allowed rhi/render
  includes). No gameplay/data includes reach into render/.
- **Positive reuse noted:** the async worker→queue idiom correctly uses
  `core::ConcurrentQueue` + `core::JobSystem` in every streaming system
  (Terrain/Grass/Vegetation/Water/ChunkOcclusion) — the core primitive is used,
  not re-implemented. `TerrainNoise` (height/normal/materialWeights/noise01) is
  the shared terrain-sampling source and is reused cleanly across systems.
- **No dead code** (`_old/`, `*test*`, `.bak`) and **no TODO/FIXME/HACK** in the unit.

## Findings

| id | sev | axe | fichier:ligne | description | action | effort | inter |
|----|-----|-----|---------------|-------------|--------|--------|-------|
| U3-1 | high | factor | GrassSystem.cpp:244-331; TerrainSystem.cpp:223-365; VegetationSystem.cpp:305-400 | The chunk-streaming ring (budgeted uploads, nearest-first request via a `Candidate{cx,cz,dist2}` sort, evict-beyond-hysteresis, per-chunk `minY/maxY`, `generation` counter) is implemented **three times** near-identically. ~100-120 lines each. | Extract a `ChunkStreamer<Instance>` / `ChunkGrid` helper (map + upload/request/evict); systems supply only the scatter fn + per-chunk payload. | L | no |
| U3-2 | high | réutil | TerrainNoise.cpp:10; GrassSystem.cpp:35; VegetationSystem.cpp:21; SplatTextures.cpp:13; TreeGenerator.cpp:11; MeshBuilder.cpp:10 | The identical murmur3 finalizer `hashU32` (same constants `0x7feb352d`/`0x846ca68b`) is copy-pasted in **6 files**; `HashRng` in 3 (Grass/Veg/TreeGen). | Hoist `hashU32`/`HashRng` once (natural home: `TerrainNoise.hpp`, or `engine/core/Hash.hpp` beside `fnv1a`). | S | yes (core candidate) |
| U3-3 | high | archi/qualité | FrameUniforms.hpp:13-68 vs shaders/common.glsl:3-52 | The C++ `FrameUniforms` struct and the GLSL `FrameUbo` block are kept in sync by **human comment convention only** ("append at end", "keep every member vec4/mat4-sized"). No `static_assert` on `sizeof`/offsets. A single mis-aligned or reordered member silently corrupts every one of ~14 shaders (the "no free .w" memory note is a symptom of this fragility). | Add a `static_assert(sizeof(FrameUniforms)==N)` + offset table, or codegen the GLSL block from the struct. | M | yes (U4 fills it, U2 UBO upload) |
| U3-4 | med | factor | TerrainSystem.hpp:112-115,184; GrassSystem.hpp:99; VegetationSystem.cpp:387-388,547-548,603-604 | Manual u64 chunk-key pack/unpack (`cx<<32|cz`, then `key>>32` / `key & 0xffffffffu` re-derived by hand) appears at ~15+ sites across the three systems and both occluders. Error-prone (i32/u32 sign casts hand-repeated). | Provide `chunkKey(cx,cz)`/`keyCx(key)`/`keyCz(key)` (fold into U3-1's helper). | S | no |
| U3-5 | med | factor | TerrainSystem.cpp:371-407; VegetationSystem.cpp:407-462 (caster 439-468); GrassSystem.cpp:337-362 | The `MeshVertex` vertex-attribute pipeline descriptor (loc 0=pos,1=normal,2=uv,3=color) is hand-written in ~5 `createPipeline` calls (Terrain draw+caster, Veg draw+caster, Grass). | Expose a `meshVertexLayout()` returning the `rhi::VertexBufferLayout` (MeshVertex lives in `MeshData.hpp`). | S | no |
| U3-6 | med | factor | PostFx.cpp:199-261 (targets), 81-101 (generation sum) | Half-res post targets (godRay/volumetric/ssao/contact) are created by 4 near-identical create-texture + framebuffer + bindGroup-from-sceneDepthCopy blocks; and the `shaderGeneration` sum over 9 shaders is spelled out **twice** (build + refresh) — adding a pass needs edits in 3 places. | `makeHalfResTarget(...)` helper + a pass/shader-name table iterated for load/build/refresh. | M | no |
| U3-7 | low | qualité | PostFx.cpp:107-158; VegetationSystem.cpp:268-292; TerrainSystem.cpp:186-210 | RHI resources are freed by long hand-written paired `destroyX()` + `=  {}` lists (PostFx `destroyTargets` ~50 lines). Correct today but each new resource is a manual add in ≥2 places; a leak is invisible. | A minimal RAII handle wrapper (owning `unique`-style RHI handle) would delete this whole class of boilerplate. | L | yes (U2 RHI handle scheme) |
| U3-8 | low | propreté | FrameUniforms.hpp:17,41 vs common.glsl:7-9,45 | Comment drift between the two mirrors: `uTime` documents `z = volumetric shaft intensity` in GLSL but `yzw unused` in C++; `uCloudMapInfo.w`/`uWaterMapInfo.w` "unused" only on one side. Harmless now but is exactly the desync U3-3 warns about. | Sync comments (or eliminate the second copy via U3-3 codegen). | S | no |
| U3-9 | low | archi (note) | ChunkOcclusion.hpp:31; GpuOcclusion.hpp:30 | Two occlusion producers (CPU horizon-march + GPU Hi-Z compute) both emit into the same `std::unordered_set<u64> occluded` merged by the scene. This is **intentional complementary staging** (brick 26), not duplication — flagged only so the dual maintenance + merge-site (in LandscapeScene) is on record. | None; document the seam. | — | yes (U4 merge site) |

## Notes on severity calibration

Per CLAUDE.md §1 the renderer is deliberately not gold-plated; shader micro-style
was not audited. Findings are scoped to **maintainability/reuse across the 14
systems**. The three `crit`-adjacent items are all factorization/robustness, not
correctness bugs — the renderer works. File sizes (VegetationSystem 632,
TerrainSystem 517) are large but cohesive; the only real "god" artefact is the
shared `FrameUniforms` struct (U3-3), not any single `.cpp`.

The **GrassSystem** (recent churn hotspot) was inspected specifically: its
scatter/LOD-prefix logic is sound and well-commented; its only issues are the
shared ones (U3-1 streaming dup, U3-2 hash dup, U3-4 key packing) — no
grass-specific defect found.
