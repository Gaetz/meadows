# RENDERING — architecture, state, lessons & roadmap of the 3D renderer

> The single reference for Meadows' graphics stack. It consolidates six
> former documents (2026-07-26); their full brick-by-brick journals are
> preserved under `docs/archive/` — this file keeps the *state, the
> decisions with their why, the durable lessons, and what remains*.
> Read this before touching `engine/rhi/`, `engine/render/` (incl. the
> `WorldRenderer` orchestrator), or `LandscapeScene::render`.

## 0. Archive map

| Archived doc (`docs/archive/`) | Superseded by section |
|---|---|
| `VULKAN.md` — Vulkan/MoltenVK backend journal (V0→V8) | §1 RHI & backends, §4 Performance, §5 Lessons |
| `3D-RENDERER.md` — landscape renderer bricks 1–34 | §2 Frame & systems, §6 Roadmap |
| `LIGHTING.md` — light architecture + clustered chantier | §3.1–3.3 |
| `RADIANCE-CASCADES.md` — GI chantier (G0→G8) | §3.4 |
| `VOLUMETRIC.md` — fog-as-lighting (V1→V4, H1→H4) | §3.5–3.6 |
| `GPU-PERF.md` — measurement tooling + perf chantiers (P/PG series) | §4, §6 |

Exact §/brick numbers cited by older code comments refer to the archived
files; the sections below carry everything still load-bearing.

## 1. RHI & backends

**Vulkan is the final renderer** (decision 2026-07-12, confirmed by the
memory `renderer-backend-lineup`): GL 4.6 stays as the PC fallback, a
GL 4.1 low-spec mode is a someday-item, macOS is Vulkan-only (via
MoltenVK). Backend selection is **runtime** (`rhi::Device::create`
factory, preference chain Vulkan→GL); OS selection is compile-time
(CLAUDE.md §3.1). Everything goes through `engine/rhi/` — no gameplay,
world or UI code touches a graphics API.

### 1.1 Interface shape

Explicit, Vulkan-shaped concepts: command buffer, pipeline state objects
(depth/cull/blend/bias/wireframe/topology), bind groups (list of writes,
push-descriptor style), render passes (dynamic rendering — no VkRenderPass
objects), samplers (incl. comparison/PCF), texture arrays & 3D volumes,
mip control, `copyTexture`/`copyBuffer`, per-feature `DeviceCaps` (systems
gate on flags, never on API versions). Compute extension:
`BufferUsage::Storage` (+ `readback` staging buffers),
`createComputePipeline`, `dispatch`, storage images with mip selection.

**Barriers are scoped** (parallelism chantier PG1): `memoryBarrier(dst)`
takes `BarrierStage_{Compute,Fragment,Vertex,Transfer,All}` — the
destination scope is what *permits* the GPU to overlap independent passes;
the historical no-arg overload is the serialize-everything `All`.
`readBarrier(src)` is the execution-only WAR fence (prior reads finish
before subsequent compute writes; no-op on GL where ordering is implicit).

**Async compute** (PG3): `caps().asyncCompute` + `asyncComputeCmd()` /
`endAsyncCompute()` — a second command buffer submitted to a dedicated
compute-capable queue family *after* the frame's graphics (it waits it via
a timeline semaphore) and awaited by the *next* frame's consumers at their
consuming stages only (fragment / compute / depth-writes; vertex and
transfer overlap freely). While the async window is open, `updateBuffer`
staging copies are routed into the compute CB (they execute on the compute
queue, ordered with the chain that reads them). Resources are created with
CONCURRENT sharing across the two families (prototype simplicity; Metal
ignores it, PC pays a minor bandwidth cost — refine to EXCLUSIVE +
ownership transfers post-demo if a PC re-baseline justifies it).

### 1.2 Vulkan backend — settled decisions (bring-up V0→V8)

- **One GLSL corpus for both backends.** shaderc compiles GLSL 460 →
  SPIR-V at *runtime* (hot-reload preserved). The corpus was already
  explicit (`layout(binding=)`, std140/430); the only divergences were
  varying locations (numbered) and `gl_VertexID` (`compat.glsl`, keyed on
  the `VULKAN` predefine). GL needs no injection.
- **Binding remap by descriptor class**: GL has separate namespaces for
  UBO/sampler/SSBO/image bindings, Vulkan has one per set. The backend
  rewrites `binding = N` into per-class ranges while compiling, and that
  rewrite doubles as the reflection that builds pipeline layouts
  (`ShaderDesc::uniformBlocks/samplers` are GL-4.1-only and ignored here).
- **Dynamic rendering + push descriptors** (both verified on MoltenVK) —
  no render-pass/framebuffer/descriptor-pool objects; a bind group is a
  list of writes pushed against the bound pipeline's layout.
- **Coordinate conventions absorbed in the backend**: Y-flip via
  negative-height viewport (front-face inverted at pipeline build to keep
  GL winding semantics); depth 0..1 everywhere —
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` with GL adopting the convention through
  `glClipControl(GL_ZERO_TO_ONE)`.
- **Swapchain B8G8R8A8_UNORM (not sRGB)** — the tonemap owns gamma.
- **VMA** for memory; **mini deletion queue** (destroy after
  `kFramesInFlight` frames, fence-proven; mid-recording destroys are the
  reason a plain `vkDeviceWaitIdle` is not enough).
- **`recordHostUpdate`** (the V7e lesson, see §5): while a frame records,
  dynamic-buffer updates go through a staged `vkCmdCopyBuffer` *in the
  frame's command buffer*, barriered both ways — in-place writes raced the
  in-flight frame (CSM matrices flipped mid-frame → flickering shadow
  plates). In-place remains only for init/tools and inside-pass updates
  (ImGui vertex streams).
- **Persisted VkPipelineCache** (`vulkan-pipeline-cache.bin`) — Metal
  shader compilation happens on first launch only; the title screen warms
  the main-path variants.
- **Sync validation on demand**: `MEADOWS_VK_SYNC_VALIDATION=1` (or a
  `vk_layer_settings.txt` with `khronos_validation.validate_sync`) chains
  the synchronization-validation feature; messages land in spdlog
  (`[vk-validation]`) with an end-of-run verdict. **Every RHI chantier
  must run clean under it** — standard validation sees none of these races
  (proven three times: V7e, V8a's three classes, PG3's two hazards).
- `tools/vksmoke` — windowless-engine bring-up harness (device, clears,
  resources, shaders, draw), still the quickest backend smoke.

### 1.3 GL backends

GL 4.6 (DSA + bindless-style paths) implements the full caps set and is
the PC fallback; it does not run on macOS (Apple GL is 4.1). The GL 4.1
degraded mode (no compute/SSBO/volumes — CPU occlusion as Hi-Z fallback,
features off via caps) remains a backlog item. After the barrier-scoping
work, GL compiles but has not been smoked — **first PC run must visually
re-validate GL 4.6 parity**.

## 2. Frame anatomy, systems & data flow

### 2.1 Who orchestrates (and the known entanglement)

The reusable **systems** live in `engine/render/landscape/`:
`TerrainSystem` (64 m streamed chunks, 4 LODs + skirts, deterministic
world-space noise, sRGB splat array), `GrassSystem` (Quick_Grass blades,
metric density prefix), `VegetationSystem` (12 variants, deterministic
forest-belt scatter, canopy LOD + low twins, space-colonization trees by
default with lobe trees as A/B), `TreeGenerator` /
`SpaceColonizationTree`, `SkySystem` (analytic day/night palette, weather
grading, 512² cloud-map bake, cumulonimbus billboards), `ShadowMapper`
(CSM), `WaterSystem` (sea plane, planar reflection, pool-depth bake),
`PostFx` (bloom, god rays, 2D volumetric march, froxel fog, contact
shadows, auto-exposure, SSAO-slot), `RadianceCascades` (GI),
`LightClusters` (clustered-forward culling), `ChunkOcclusion` (CPU
horizon) + `GpuOcclusion` (Hi-Z compute cull + fence readback),
`TerrainLightMap` (worker-baked far sun shadows + sky aperture),
`FxRenderer` (CPU particles), `ShaderLibrary` (includes, hot-reload).

The **orchestrator**: `engine/render/WorldRenderer.{hpp,cpp}`
(`render::WorldRenderer`, engine-side since R4 of §7) owns pass order,
FrameUbo composition (via `render::FrameComposer`), offscreen targets,
light UBO fill, key-shadow selection, and the per-subsystem
`RendererConfig` (R3). Game-side remain only its two friends: the ImGui
tuning panels (`game/ui/RenderTuningPanels`, R1) and the Forms↔flat-params
tuning seam (`game/scenes/RenderTuningIo` — the engine never sees a Form,
CLAUDE.md §4). The chantier's proof runs: `TreeCreationScene` mounts a
partial config (R5). `AnimPreviewPanel` still hand-rolls its own RHI
offscreen pipeline — moving it onto a second configured instance is a
follow-up.

### 2.2 Pass order (one frame)

Graphics queue: streaming/occlusion pumps → cluster-light culling
(compute) → cloud-map bake 512² → key-shadow atlas tiles → rain occlusion
(storm only) → CSM ×3 (round-robin; per-cascade caster cull) → GI
consumer fence (pipelined GI) → planar reflection (half-res, sea visible
only, non-oblique culling frustum) → opaque HDR pass (terrain, props,
grass, meshes, skinned NPCs, sky, water volumes; frustum + CPU∪GPU
occlusion) → color+depth copies → Hi-Z build + cull (fence readback) →
water composite → PostFx (bloom/god rays/froxel or march
volumetric/contact/auto-expo) → **GI chain recording (pipelined)** →
tonemap composite (+ RmlUi in-pass, ImGui after) →
**async compute submit** (GI chain on the second queue, overlapping
present and the next frame's front).

The sim/render seam is the Phase-5 snapshot: `extractScene → 
RenderSnapshot → submit`; the renderer never touches the ECS `World`.
`FrameComposer` resolves per-frame uniforms (interior mode, weather
crossfade, froxel reach 48/800 m…); `LandscapeTuningForm` /
`RcTuningForm` / tree tuning forms (moddable TOML, CLAUDE.md §5) map to
flat engine params at the scene boundary — the engine never sees a Form.
Live tuning: render panels + "Save render tuning" →
`data/mods/render-tuning.toml` (an ordinary plugin layer).

### 2.3 FrameUbo discipline

`FrameUniforms` (C++) ↔ `common.glsl` FrameUbo mirror **by comment only**,
locked by static_asserts on byte offsets. **Append-only** at the end of
the struct, both sides in lockstep, then bump the size assert. One
mid-struct insertion desyncs ~14 shaders at once (paid lesson). The
LightsUbo (binding 5) follows the same append-only rule; coordinated
array resizes (24→64 lights) touch both GLSL declarations and the C++
mirror in one change.

## 3. Lighting & atmosphere

One forward model everywhere; interior vs exterior differ by *parameters*
not algorithms. Outdoors the big shadowed light is the sun (CSM is its
map) and local lights decorate; indoors the locals ARE the lighting.

### 3.1 Sun, shadows, shadow policy

- **CSM**: 3 cascades, 2048² Depth32F, ~800 m, PCF, quantized sun
  (~0.4°/8 s hysteresis so cascades re-fit rarely), **round-robin**
  (cascade 0 every frame, far cascades alternate; a sun step re-renders
  all), **per-cascade caster culling** (the V8b ÷7 win — the ortho volume
  includes caster reach, so distant mountain shadows survive).
- **Key-shadow atlas** (interior-grade exact shadows): the up-to-4
  best-scored `castsShadow` lights render one 1024² tile each into a
  2048² atlas (one caster UBO per tile); shaders find a light's tile via
  `LightsUbo.windowInfo.z` (slot+1, 0 = unshadowed). PCF taps stay one
  texel inside the tile.
- **Window projectors**: a window's light is its authored rectangle
  extruded along the *live* sun — the aperture is DATA
  (`LightForm.windowHalfWidth/Height`), not a shadow map; the UBO
  direction carries the window's into-room normal (marker w = −3), the
  facing gate lives in the shader. Sheared floor pools that stretch at
  sunset, froxel dust slabs, ~zero cost. Limit: the frame clips, not the
  furniture (that is the key shadow's job).
- **Per-light policy is data** (`LightForm.shadowMode`): `""/none`,
  `"key"` (atlas), `"rcOnly"` (routed entirely through the GI field —
  free soft penumbrae; the light leaves the direct path and the froxels,
  its GI blob carries everything). Distant torches never get shadows —
  their night readability is froxel halo + bloom.
- Deferred rendering is **rejected** (recorded so it stays rejected):
  per-surface-TYPE stylization would need a fat G-buffer or shader IDs,
  cutout/transparency regress to special cases, M1 pays G-buffer
  bandwidth without tile-memory access, and clustered forward gives the
  same scalability without refactoring shaders.

### 3.2 Clustered forward (Forward+)

- **Light budget 64** (LightsUbo binding 5), CPU-selected per frame:
  sphere-vs-frustum cull (`render::Frustum` + `intersectsSphere`) +
  importance score `intensity/(1+dist²)`; the returned list stays
  nearest-first (flicker phases per index stay stable; the GI takes the
  first 24 = the nearest for its ~32 m window). Legacy/no-compute path
  and the planar-reflection pass clamp to the 24 nearest.
- **Cluster grid 16×9×64** = the froxel grid downsampled ×8 in XY with
  **identical exponential z-slices** (shared `clusters.glsl` — change the
  mapping in lockstep or the grids shear). `cluster_cull.comp`
  (`LightClusters`): one thread per cell, conservative sphere test
  against the cell's world AABB (4 corner rays × 2 shell depths + far
  face center — slices are camera-distance shells), writes
  `count + idx[31]` per cell into an SSBO riding binding 4 of the frame
  bind group. **Capacity lesson**: at 15 slots the interior hall
  (~18 overlapping light spheres) overflowed and dropped the
  farthest-from-camera lights — the far windows lost their pools; 32
  covers the worst current stack, overflow drops nearest-last.
- Surface shaders (`locallights.glsl`, shared `shadeLocalLight`) find
  their cell from `gl_FragCoord` + camera-distance slice and loop only
  the cell's list; `froxel_inject` reads the same lists (xy downsample,
  same z). A/B: "Clustered lights" toggle (default ON).
- Local light model: windowed inverse-square falloff, spot cones with
  relative edge, interior wrap diffuse + small normal-free bounce (omni
  only — the room takes its hue; beams bounce via GI).

### 3.3 Interior model ("Helios")

Interior ambient = artistic base × daylight(sun elevation) ×
weather.ambientIntensity, weight knob (H1). `sunLinked` lights take the
sun's *live* color/gate in the direct path AND their GI blobs (H2) — a
window ray and its pool pale under storm and die at night. The
**buried rule** (`WorldspaceForm.buriedBelowY`, 4 m fade band, evaluated
per-position): below it a cellar decouples from the outside even inside a
windowed house (H3). Interior dust = froxels (H4) — the removed
FXShaft-style blade meshes lost the A/B; window projectors light the dust
slabs for real.

### 3.4 Global illumination — Radiance Cascades

World-space radiance cascades (Sannikov), rebuilt every frame
(single-shot, no ghosting by construction):

- **Voxel clipmap** 2 levels 64³ RGBA16F (0.5 m / 2 m voxels; spans
  ~32 m / ~128 m), camera-snapped. Injection each frame: analytic
  terrain (height/normal/material palette; solid below ground), props &
  NPCs as AABBs (SSBO; box occlusion assumed stylized), vegetation
  (canopies as semi-transparent green boxes — per-voxel opacity ~0.12,
  high values saturate to a black lid), and the frame's local lights
  splatted with the same windowed falloff as the direct path.
- **Cascades**: 5 levels, 8·4ⁱ octahedral directions, cascade 0
  dir-major for hardware trilinear; build raymarches the clipmap;
  **interval extension** (march a quarter, double twice by shift+merge)
  makes long intervals affordable; merge top→0; apply = `giAmbient()`
  (cosine-weighted 8 dirs, classic-ambient fade at volume edges, banding
  ramp available but default smooth — the cel ramp lives on direct
  light) and `giAir()` (direction-averaged, for fog).
- **Multi-bounce** via temporal feedback (inject reads last frame's
  merged cascade 0, knob 0.5). **rcOnly lights** (§3.1). **Splat
  re-contract** (clustered era): with clustered direct on every surface,
  a normal light's splat drops to its *bounce share* ("Light splat
  bounce", 0.35) — **exterior only**: interiors draw no terrain/grass,
  clustered adds no new receiver there, and the factor was killing the
  tuned window glow (dev-caught regression).
- **Pipelined N−1 + async compute** (PG2/PG3): the chain records at the
  END of the frame, consumers read last frame's cascade 0 (invisible —
  the field is already temporal); the chain ends *unfenced*, the next
  frame posts the consumer fence before its first GI reader; with
  `caps().asyncCompute` the whole chain (its staged uniform copies
  included) runs on the second queue. RC keeps its own 24-light cap
  (nearest of the 64 budget).
- RC replaces the INDIRECT term only — sun/CSM/direct never route
  through it. What RC "shadows" is directional occlusion of the ambient
  (the AO reading) plus occluded light bounce.
- Anti-lesson on record: the adaptive stylized ramp (scene-measured
  quantization) was removed — it measured the sky, moved with weather,
  coupled to multi-bounce. Quantization anchors are artistic values,
  never scene measurements.

### 3.5 Volumetric — fog as lighting

The founding inversion: fog color = ambient in-scatter **+ sun
in-scatter × per-step visibility** — lit and shadowed air no longer
converge to the same grey; the fog structures light instead of
flattening it. No raytracing (industry-standard shadow-map raymarch;
MoltenVK exposes no RT anyway).

- **V1 analytic**: `applyFog` gains the sun lobe
  (`skyGradient + sunColor × phase^k × strength`), weather-crossfaded
  (`WeatherForm.fogSunScatter`). Night: dies with sunColor.
- **Froxel fog (V4/H4)**: 128×72×64 grid (RGBA16F ×2 ping-pong,
  ~5 MB), exponential camera-distance slices 1→800 m exterior / 48 m
  interior (the composer sets the reach; the analytic fog keeps the tail
  beyond). Inject computes density (height fog + interior dust with
  drifting value-noise "wisps") and light (sun × phase × CSM+cloud
  visibility + `giAir` + the cell's cluster-listed local lights, window
  projectors carving dust slabs, key-shadow atlas clipping beams);
  integrate per column; apply = one trilinear fetch at pixel depth into
  the same target the 2D march writes (tonemap composite untouched).
  **Temporal accumulation**: white-in-time jitter (integer hash of
  cell+frame — a rolled spatial pattern only *translates* and the EMA
  can never erase its marching bands), reprojection into last frame's
  volume, EMA 0.1; **selective jitter** (only hard-depth signals — CSM
  boundaries, beam edges, key shadow — sample at jittered depth; smooth
  analytic terms sample slice centers); display-only 7-tap cross blur
  (history stays sharp). History invalidated on teleport/reach
  change/frames without froxels. Fallback + A/B: the 20-step ½-res 2D
  march (also the no-compute path). Measured ~0.5 ms (M1 Debug).
- Screen-space god rays remain (foliage, sunsets — taste call).

### 3.6 Sky, weather, clouds, water, post

- **Sky**: analytic day/night palette (dawn ≠ dusk), sun disc + glow;
  weather grading CPU-side. **Weather**: `WeatherForm` records (moddable
  TOML), ~30 s crossfades; wind time is ACCUMULATED (`+= dt×speed`) so
  clouds/waves never teleport on weather change. Storm front
  (cumulonimbus billboard towers on the horizon ring), rain (procedural
  hash-scrolled streaks in a camera cylinder, count × intensity),
  wetness (albedo darkening), rain occlusion (top-down ortho depth —
  no rain under roofs or tree canopies; trees render their solid
  shadow proxies through the same caster path, one chunk of reach).
- **Clouds today**: one 512² cloud-map texture baked once per frame
  (every shadow consumer taps a texture, not an FBM);
  `cloudShadowFactor()` (clouds.glsl) consumed by terrain, grass, trees,
  froxels, RC inject, volumetric march; `cloudInfo`/`cloudMapInfo`
  FrameUbo fields. See §8 for the extensibility contract.
- **Water**: global sea plane (planar mirror + oblique clip; culling
  frustum from the NON-oblique projection — Lengyel's trick corrupts the
  far plane), pool-depth bake killing foam on small ponds, submersion
  tint via effective-surface uniform. **Placed water volumes**
  (`WaterVolumeForm`, spec decided): box volumes whose top face is the
  surface, per-volume tint/chop, sea keeps the mirror, volumes get a
  "deaf" sky-fresnel surface; per-cell water heights rejected (two
  mechanisms for one thing).
- **Stylized pass** (`stylized.glsl`): 2-step BotW ramp, snapped CSM
  pools, gated SSS, stepped rim, all behind `uAmbientColor.w` A/B.
  **Grading**: analytic vibrance/split-tone/contrast in tonemap (LUT 3D
  someday). **Auto-exposure**: log-luminance 64² → mip 1×1 → asymmetric
  adaptation ping-pong; slider is the EV bias. **Contact shadows**
  (Bend-style, ½-res, 12 steps toward the sun; toggle = white-cleared
  texture). **Terrain light map** (worker-baked 256², ~1.5 km): R = far
  sun visibility beyond CSM reach, G = sky aperture multiplying ambient
  (valley grounding). **Vertex AO** baked at mesh decode (disk cache).

## 4. Performance

### 4.1 Tooling

- **GpuProbe / F6 panel**: per-pass GPU ms (ring of 4 frames, never
  blocking), CPU column (FrameProbe), geometry counters (Mtri per system
  — the honest dissection on Metal, where mid-pass timestamps are
  structurally meaningless: `caps().midPassTimestamps`).
- **`gpu budget` log line**: the F6 table auto-logged once at frame 2000
  (warmup passed, window full) — measurement for scripted/headless
  sessions.
- **Protocol**: Release, 4 spots (exterior ridge with sea, village,
  interior hall, storm), avg+max per pass, 360° + fast movement.
  Measure first — a pass under 0.5 ms is not worth a brick. The spawn
  spot deliberately faces a wall (kept so baselines stay comparable).
- Sync validation on every barrier/queue change (§1.2).

### 4.2 Current numbers (2026-07-26)

- **M1 Release, spawn spot**: serial baseline **34.7 ms** → scoped
  barriers (PG1) **26.4 ms** → +pipelined GI (PG2, neutral on M1 —
  MoltenVK doesn't overlap across submits, but composite drops
  0.33→0.02 ms and the structure is the PC prerequisite) → +async
  compute (PG3) **19.2 ms** — the RC chain (~7.5 ms) fully hidden.
  In-game: forest 15–17 fps before the chantier → **27 fps min** after.
  Sum-of-passes ≈ total means no overlap; totals below sums prove it.
- **M1 verdict: vertex-bound** (V8c: quarter-res pixels changed mainPass
  by ~1 ms) — the M1 levers are geometry (imposters, low-twin veg in
  main/reflection, grass density), not resolution. Render scale was
  built, measured useless on M1, and retired (the measurement was the
  deliverable).
- **CSM caster cull** (V8b): shadows 35–50 ms → ~5 ms (÷7) once culled
  per cascade volume.
- **RTX 4070 baseline (10 ms) is STALE** — pre-RC/froxels/clustered, on
  GL. First PC session: re-baseline via the gpu-budget line, sync
  validation pass, GL 4.6 visual parity check.

### 4.3 Remaining levers (see roadmap)

CSM static/dynamic split cache (P5b) — only if `shadows` still dominates
after round-robin+cull; upload stall re-measure (P6, byte budget or
persistent-mapped staging ring); grass = knobs + measurement only until
its visual redo; vegetation imposters (deferred decision); per-queue GPU
timestamps (rc* scopes are blind in async mode); transfer queue (PC);
RC tuning pass (3 → 1–2 ms targets via the GI panel knobs).

## 5. Durable lessons (cross-chantier)

1. **Measure first.** Remedies without measurements are hypotheses —
   paid twice (P0 rule; render scale retired by its own measurement).
2. **UBO lockstep, append-only** (§2.3). After any shared-type layout
   change: clean rebuild (ninja header-dep miss → stale-object heap
   corruption).
3. **Fill-rate cutout is enemy #1** (leaf-cards verdict). Opaque +
   early-Z wherever possible.
4. **Barriers are permission, not bookkeeping**: a broad barrier
   serializes the whole GPU; scope destinations (PG1: −8 ms from
   scoping alone). Under overlap, per-pass timestamps blur — trust the
   frame total.
5. **Standard validation sees zero races; sync validation sees them
   all** (V7e, V8a ×3, PG3 ×2). Vulkan work ships with a clean
   sync-validation run, every time.
6. **In-place dynamic-buffer writes race frames in flight** — staged
   copies inside the frame CB (`recordHostUpdate`); GL only *looked*
   immune because the driver versions updates.
7. **Temporal jitter must be white in time** (integer hash per
   cell+frame). A translated spatial pattern gives the EMA marching
   bands it can never erase; don't jitter smooth analytic signals at
   all (variance without banding to hide).
8. **Quantization anchors are artistic constants**, never scene
   measurements (adaptive ramp post-mortem).
9. **GPU→CPU readbacks**: staging + fence, poll never block (a sync
   readback once cost 25 ms/frame); keep last verdict while pending.
10. **Wind/cloud phase time is accumulated**, never `t × speed`.
11. **Tiled GPUs (Metal) can't time mid-pass** — gate sub-probes on
    `midPassTimestamps`, dissect with geometry counters instead.
12. **Planar-reflection culling uses the non-oblique projection.**
13. **Hi-Z pyramids in compute** (fragment self-feedback is UB).
14. **Check Apple clang before blaming code** (nested-class default
    initializers, missing libc++ features) and mind the Vulkan loader
    RPATH — both bit at first M1 build.
15. **Shader-compile failure at first load aborts** (only hot-reload
    keeps the old program) — RHI hardening candidate.
16. **Billboards face the viewpoint of the pass drawing them** — never
    "the camera". The contract as implemented: `tree.vert` expands leaf
    cards from the bound frame UBO's viewProj rows (main or mirrored
    view), `shadow_prop.vert` from the bound ShadowUbo's matrix (sun
    cascades, rain occlusion) — so any new pass (cubemap capture,
    impostor bake, top-down) gets correct orientation by construction,
    just by binding its own matrix. One rider: a MIRRORED pass must
    also tell billboard shaders to flip their corners
    (`uLeafLodInfo.z`) — a self-orienting quad keeps its screen winding
    under the mirror, so the pass's inverted front face back-face-culls
    it while static geometry renders fine (the leafless-reflected-trees
    bug).

## 6. Roadmap (consolidated next steps)

**Pending dev validations** (features live, eyes needed): Hi-Z occlusion
visual check (valley floor + fast rotation, A/B checkboxes); volumetric
at critical hours (dawn/noon/dusk/night/storm, Morning Mist fogStart
re-tune); torchbench night session — clustered A/B, "Light splat bounce"
exterior tune, F6 numbers into §4; key-shadow atlas orientation check
(TableSpot interior); GI latency sanity pan (pipelined+async, should be
indistinguishable).

**Lighting**: light LOD (beyond X m a torch = emissive + bloom + froxel
halo, zero direct slot — a natural `shadowMode` tier); stylized local
specular (torch glints) + ambience cubemap; moon as the night CSM
directional (pure data); cached key-shadow atlas (round-robin static
tiles) if >4 keys ever needed.

**GI**: dev perf pass on the GI panel knobs (target 3 → 1–2 ms); G8
spatial index / sparse voxels only if measured necessary; fine interior
triangles if kit-box leaks show.

**Volumetric**: froxel reprojection reserve (kept in design, unused);
particles for individual dust motes (fx, not froxels).

**Perf**: P5b CSM static/dynamic split (only if shadows still dominate);
P6 terrain upload stall (re-measure, then byte budget or persistent
staging ring); P7 grass knobs+measure (structural work waits for the
visual redo); vegetation imposters (decision deferred — the V8e geometry
counters are the input); per-queue timestamps; PC return bundle:
re-baseline, transfer queue, GL 4.6 parity smoke, sync validation on the
PC driver, CONCURRENT→EXCLUSIVE sharing refinement.

**Features**: placed water volumes (brique-32 spec in archive:
surface+submersion first, no foam v1, swimming = gameplay chantier);
cumulonimbus/rain polish as WeatherForm content; GL 4.1 degraded mode;
TAA, 3D LUT, caustics, Gerstner water — backlog.

**Chantiers ready to start**: the new clouds implementation against the
§8 contract (RENDERER-EXTRACT §7 is complete — follow-ups: tree types →
forest scatter; AnimPreviewPanel onto a configured WorldRenderer;
postFx-less blit fallback hardening on Vulkan).

## 7. Chantier RENDERER-EXTRACT (prepared, not yet executed)

**Goal**: the orchestrator becomes an engine-side, multi-instance
`engine/render/` component usable by any 3D scene — the game world
(LandscapeScene), the anim-preview tool, a coming tree-builder scene —
ending the AnimPreviewPanel-style duplication. The Vulkan backend needs
nothing: the coupling to dissolve is orchestrator↔scene.

Bricks (each lands alone, LandscapeScene byte-identical at every step):

- **R1 — UI/renderer split. DONE (2026-07-26).** The four panels
  (render, terrain & streaming, tree builder, GPU perf) moved to
  `game/ui/RenderTuningPanels` — a stateless static class, friend of
  `LandscapeRenderer`, editing its live knobs in place. Pure move
  (labels and behavior byte-identical; `LandscapeRenderer.cpp` no
  longer includes ImGui, −518 lines). The live-tuning workflow (panels
  + "Save render tuning" → `consumeSaveTuningRequest` → the scene's
  plugin write) is unchanged.
- **R2 — Neutralize game/ types. DONE (2026-07-26).** The snapshot types
  (`RenderSnapshot`/`SceneLight`/`WaterVolumeInstance`) live in
  `engine/render/SceneView.hpp` (namespace `render`); `game/SceneSubmit`
  keeps the extract functions plus `using` aliases so scene/test callers
  are untouched. The dependency audit came back clean (engine-only
  includes), so `ResidencyCache`/`MeshCache`/`TextureCache` moved
  **wholesale** to `engine/render/` (namespace `render`, lib
  `meadows-render` — they need rhi, so NOT the base lib: the §2.10
  simlink proof stays intact, re-verified). `FrameComposer`/
  `AtmosphereParams` follow the orchestrator at R4 as planned.
- **R3 — RendererConfig (per-subsystem opt-in). DONE (2026-07-26).**
  `RendererConfig` construction flags: terrain (ring streaming + terrain
  light map folded in), water, sky (weather/cloud bake/rain), vegetation,
  grass, gi, froxels (implies postFx), occlusion (CPU horizon + Hi-Z),
  postFx. A system left off is never created, allocated or ticked;
  defaults = everything, so `LandscapeScene::create(device, jobs)` is
  unchanged and the full-config call sequence is identical by
  construction. The always-on core: meshes + skinned NPCs + CSM/key
  shadows + lights + tonemap (the postFx-less blit fallback already
  existed for caps-poor devices and now serves `postFx=false` too).
  Structural side-fix: the scene color/depth copies (Hi-Z + postFx
  inputs) are decoupled from the water bind group — the copy pass gates
  on `sceneColorCopy`, not `waterSceneBindGroup`; `depthSampler` moved
  out of the water gate. **Multi-instance audit verdict: per-instance
  ShaderLibrary** — each renderer owns its programs and hot-reload
  generations; sharing would couple instance lifetimes for a compile
  cost the persisted pipeline cache already amortizes. Revisit only if
  a tool scene's startup measures slow. Proof of the opt-in path = R5's
  first consumer (this brick is validated full-config-identical only).
- **R4 — Move. DONE (2026-07-26).** The orchestrator is
  `engine/render/WorldRenderer.{hpp,cpp}` (`render::WorldRenderer`), in
  lib `meadows-render`; `FrameComposer` and `AtmosphereParams` moved
  with it (namespace `render`; the headless FrameComposer test now
  targets `render::`). One extraction the move forced: the
  `applyTuning`/`captureTuning` family took `data::*Form` arguments —
  engine code never sees a Form (CLAUDE.md §4) — so the whole
  Forms↔flat-params seam became `game/scenes/RenderTuningIo` (static,
  stateless, friend of the renderer, same bodies verbatim); the scene
  calls `RenderTuningIo::applyTuning(renderer, …)` where it called
  `renderer.applyTuning(…)`. The renderer's game-side friends are
  forward-declared names only (`::game::RenderTuningPanels`,
  `::game::RenderTuningIo`) — no game include enters the engine.
  Note: `WorldRenderer.cpp` references `ui::UiSystem` (the in-pass game
  UI composite); as a static-lib member this resolves when the exe links
  `meadows-ui` — tools that skip both never pull the object.
- **R5 — First consumer. DONE (2026-07-26).** `TreeCreationScene`
  (game/scenes/) — the tree-TYPE authoring tool: flat terrain (zero
  amplitudes — the ground plane is ordinary terrain, so splat/shadows/
  grass just work), sky, grass, one showcased tree; config
  `{water,gi,froxels,occlusion}=false` — the first real partial mount.
  The tree renders through a small VegetationSystem **showcase mode**
  (explicit instances of variant 0's full-detail mesh replace the
  streamed scatter; update() skips streaming) — the tree pipeline
  (cards, leaf mask, sway, casters) reused as-is, no hand-rolled
  offscreen path. Tree types are ordinary records of the two
  *TreeTuningForm types (the algorithm IS the record type) in the
  `mods/tree-types.toml` layer — the forest singletons untouched;
  wiring types into the forest scatter is a follow-up chantier, as is
  replacing AnimPreviewPanel's hand-rolled pipeline with a second
  configured instance. Entered from the Edit-mode scene strip, which
  REPLACES the world (no warm overlay — the tool runs alone).

**Invariants**: §2.10 headless untouched (renderer stays out of
sim/tests); FrameUbo append-only; each brick A/B byte-identical on
LandscapeScene; no parallel mechanism — one orchestrator, configured.

## 8. Case study — swapping the clouds (extensibility contract)

The test the dev asked for: could a different cloud implementation
(e.g. volumetric skyscapes) replace the current one without surgery?

**Current coupling** (what a swap touches today): `SkySystem` hard-owns
the 512² bake (`bakeCloudMap`, `cloud_bake.frag`, FBM pattern), the
cumulonimbus billboards and the storm front; FrameUbo carries
`cloudInfo` (coverage/height/scale/shadow strength) and `cloudMapInfo`
(bake center/span); `clouds.glsl`'s `cloudShadowFactor(p)` is included
by terrain/grass/tree/froxel/RC/volumetric shaders; `WeatherForm` drives
coverage/storm.

**The contract to extract** — any clouds implementation must provide:
1. **Sky visual** — a draw hook in the sky portion of the opaque pass
   (dome-time billboards, raymarched slab, whatever).
2. **Sun-visibility field** — the query every lighting consumer already
   uses: *a texture + mapping uniforms behind `cloudShadowFactor(p)`*.
   The GLSL entry point stays; only its sampling source varies. This is
   the hard requirement: GI, froxels, volumetric and every surface
   shader read cloud shadows through this one function today — keep it
   the single seam.
3. **Coverage scalar(s)** — what weather/ambient/froxel density read
   (`cloudInfo.x` today), fed from `WeatherForm` like everything else.

**What violates the contract today**: the bake resolution/pattern is
hard-wired in SkySystem rather than behind an interface; FrameUbo field
names assume "a baked map" (acceptable — fields are opaque vec4s; a new
impl reuses the slots or appends); cumulonimbus/storm visuals live in
the same class as the shadow field. **Conformance path** (cheap, can
ride R3 or precede it): split `SkySystem` clouds into a `CloudField`
unit owning bake+uniforms behind the three-point contract above; the
sky dome, palette and sun stay in SkySystem. Then "new clouds" = a
second CloudField implementation behind a toggle — an exchange, not
surgery. Verdict: the architecture is one small seam away from
swappable; the consumers are already funneled through one GLSL function
and two vec4s.
