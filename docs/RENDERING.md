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
  (ImGui vertex streams). The two barriers are **scoped to the buffer's
  declared usage** (`bufferReadScope`: vertex → VERTEX_INPUT, uniform →
  the shader stages, …), not a fixed all-read mask — the PG1 principle
  applied to uploads. The staging is a slice of a **persistent-mapped
  staging ring** (one per frame slot, reset fence-proven in beginFrame,
  growth parks the old buffer in the deletion queue): the former P6
  lever — no more per-update `vmaCreateBuffer` + deletion-queue entry.
- **`StoreOp` on render passes**: `RenderPassDesc.storeOp/depthStoreOp`
  (default Store) map to `STORE_OP_DONT_CARE` for transient attachments —
  on a tiled GPU (M1) a DontCare attachment never leaves the tile. First
  user: the reflection pass's depth (mirror-only scaffolding). GL 4.3+
  maps it to `glInvalidateFramebuffer`; GL 4.1 silently keeps Store.
- **Per-queue GPU timestamps**: a second query pool serves the async
  compute stream (reset at the top of the compute cb — same-queue
  ordering with its writes; harvest rides the slot's timeline wait). The
  rc* scopes are measurable in async mode — first reading on M1:
  inject 3.0 / build 2.5 / merge 3.3 ms, ~8.9 ms of async chain that was
  invisible. `GpuProbe` holds a slot's resolution two device frames so a
  mixed graphics+compute slot never reads an in-flight compute query.
- **Upload (transfer) queue**: in-frame DEVICE-LOCAL buffer uploads
  record into a per-slot transfer cb, submitted at endFrame before the
  graphics submit. Sync: its OWN timeline (a third queue interleaving on
  the shared one could signal out of order — forbidden); waits last
  frame's graphics value (WAR), graphics waits its value at the
  reader stages; buffers are CONCURRENT across the three families. On PC
  the transfer-only family is a real DMA engine; MoltenVK gives a third
  generic queue (family 2) — same topology, so M1 sync-validates the PC
  path. The compute chain does NOT wait uploads (the rc chain reads no
  streamed buffer — extend the wait if that changes). Dynamic-UBO
  updates stay on the frame cb (they need in-stream ordering);
  compute-window updates stay on the compute cb.
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
world-space noise; the material layer — cooked BC7/BC5/R16 `.mtex` splat
arrays with an A/B against the procedural tiles, height-based layer
blending, per-layer normal mapping over the planar-UV tangent basis,
POM on the dominant layer, bi-frequency anti-repetition, and the
`TerrainShadeMap` region bands feeding biome rules + macro tint — is the
TERRAIN-TEXTURING chantier, journal + knobs: `docs/TERRAIN-TEXTURING.md`),
`GrassSystem` (Quick_Grass blades,
metric density prefix; blades **inherit the terrain albedo at their
root** — the scatter bakes the splat blotch color per instance
(packed in `groundNormal.w`) and the panel colors are tints ×ground,
so meadow and terrain share ONE color source: dry blotches tint the
blades above them, and the distance fade dissolves into the ground —
the BotW raccord, completing the ground-normal shading; root AO eases
out with the LOD for the same reason), `VegetationSystem` (12 variants, deterministic
forest-belt scatter, canopy LOD + low twins, space-colonization trees by
default with lobe trees as A/B), `TreeGenerator` /
`SpaceColonizationTree`, `SkySystem` (analytic day/night palette, weather
grading, 512² cloud-map bake, cumulonimbus billboards), `ShadowMapper`
(CSM), `WaterSystem` (sea plane, planar reflection, pool-depth bake),
`PostFx` (bloom, god rays, 2D volumetric march, froxel fog, ground
mist, contact shadows, auto-exposure, SSAO — half-res Alchemy taps over
the depth snapshot, tonemap multiplier like contact. LIGHTING CONTRACT:
short radius only (~0.7 m, `uSsaoInfo`) — contact-scale crevices the RC
GI probes cannot resolve; distance-faded by 140 m (far occlusion belongs
to RC + mist); sky neutral; grass blades exempt via the scene alpha
flag; sun-independent so interiors keep it. CSM/froxels untouched — the
pass reads only the depth copy), `MistMap` /
`NoiseVolume` (valley bake + shared Perlin-Worley volume — §3.5
"Ground mist"), `RadianceCascades` (GI),
`LightClusters` (clustered-forward culling), `ChunkOcclusion` (CPU
horizon) + `GpuOcclusion` (Hi-Z compute cull -> indirect commands, no readback),
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
occlusion) → color+depth copies → Hi-Z build + cull (writes next frame's indirect commands) →
water composite → PostFx (bloom/god rays/froxel or march
volumetric/contact/ground mist/auto-expo) → **GI chain recording
(pipelined)** →
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
  (nearest of the 64 budget). **N−1 origin contract** (fix 2026-07-27):
  `uGiGridInfo` carries the PREVIOUS inject's origin — the one matching
  the content readers actually sample. Publishing the current frame's
  snap shifted the whole GI field by one voxel for one frame on every
  0.5 m camera step ("GI blinks while walking"; `updateInterval` could
  not hide it — every inject frame mismatched). Touches the RC apply
  (`giAmbient`/`giAir`, froxels included) only; froxel injection,
  shadows and the chain's own build origins are unchanged.
  **Per-cascade lattice origins** (fix 2026-07-28): the probe lattices
  used to hang off the shared FINE origin (rc_build: origin +
  k·spacing·2^i); a fine-granular origin translated the upper cascades
  by FRACTIONS of their own spacing on every 0.5 m camera step — a
  low-frequency color ripple sweeping surfaces while moving (worst on
  dark uniform walls: cliffs). First cure (coarsest-spacing anchor,
  8 m) killed the ripple but made the fine window — and the apply's
  border fade — JUMP 8 m per recenter (GI popping in on the ground).
  Final design: each cascade snaps its own origin to its own spacing
  (the CSM texel-snap lesson per level; DDGI scrolling volumes /
  clipmap toroidal levels are the same cure) — world-fixed probes at
  every level AND the fine window back to its smooth 0.5 m creep, so
  the border fade appears progressively again. Plumbing: rc_build takes
  the lattice origin from push constants (c.yzw); rc_merge adds the
  dst→src lattice offset (parent-probe units) to its index-space parent
  lookup; scene clips, apply, feedback and health probe stay on the
  fine origin — froxels/shadows untouched. KNOWN residual: on a
  perfectly uniform albedo, a probe-period dark speckle (child/parent
  PARITY — every other fine probe inherits an exact parent, the rest an
  interpolated average; near the ground the parent variance makes the
  difference visible). Any albedo variation masks it (a ±1% ground
  drift suffices); real cures if a flat-color look ever needs one: the
  RC community's bilinear-fix (child rays reprojected per parent, ~4×
  build cost) or a probe-space smoothing pass on merged cascade 0.
  Two follow-ups the first cut
  needed: CENTERED origin snap (round — floor let a parent lattice
  trail the fine window by its full spacing), and a COVERAGE fade in
  the merge — where the parent lattice falls short (≤ spacing/2), the
  gather used to freeze at the border texel (a C1 break reading as
  ground bands at sunset); it now fades to the far-field sky, the top
  level's own fallback. Separately, the inject lights terrain voxels at
  the TRUE surface point (litPos, exact tile height) instead of the
  voxel center: the 0.5 m surface-voxel staircase along a slope
  quantized the grazing-sun CSM/light-map terms — terraced bounce bands
  following the slope contours (occlusion stays voxelized). The tile
  also bakes the ANALYTIC terrain normal (RGBA8 rg, [-1,1] remap):
  deriving the normal from the bilinear height in the inject faceted it
  per 0.625 m tile texel — residual grazing-sun ndl bands along slopes.
  **Buried-probe relocation** (the actual cure for the ~1 m sinusoid
  bands following slopes): probes below the terrain surface marched
  from inside solid ground — black — and the apply's trilinear against
  those black layers oscillated along the slope with a probe-spacing
  period. rc_build now starts such probes' rays one fine voxel above
  the tile surface (continuous height → the relocated content follows
  the slope smoothly); the DDGI probe-relocation idea, terrain-flavored.
  Interiors no-op (placeholder tile).
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
- **Ceiling envelope** (all three density sites — froxel inject, 2D
  march, `applyFog`): density × `exp(-(y − seaLevel) × fogCeiling)`
  (`WeatherForm.fogCeiling`, crossfaded; panel "Fog ceiling falloff").
  The fog is a GROUND layer: upward rays exit it so the sky stays
  readable instead of greying under the in-scatter of the whole reach,
  while horizontal rays keep the full fog band and its cloud-shadow ray
  curtains. The old behavior = 0.
- **Froxel fog (V4/H4)**: 128×72×64 grid (RGBA16F ×2 ping-pong,
  ~5 MB), exponential camera-distance slices, 48 m interior; exterior
  far = `clamp(fogStart × 3, 800, 2400)` (`volumetricReach`,
  FrameComposer.hpp — the ONE formula, shared with the light-cluster
  grid far since both grids share z slices §5 B5): a far-fog weather
  (high start) pushes the froxel band and its cloud-shadow ray curtains
  into the distance instead of spending ~90% of the slices on clear air
  (with start 450 and the old fixed 800 far, the visible band held ~6 of
  64 slices). The analytic fog keeps the tail beyond, and CARRIES the
  cloud pattern there: terrain/grass/tree pass `cloudShadowFactor` into
  the `applyFog(color, pos, cloudVis)` overload (sun lobe × cloudVis,
  gradient dimmed a touch) so the curtains continue to the horizon —
  water/mesh/skinned keep the neutral overload (no cloud map bound in
  those passes; revisit if the seam shows on the distant sea). Inject computes density (height fog + interior dust with
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
  march (also the no-compute path); its `giAir` is sampled once per step
  PAIR and held — the 8 slab fetches were the step's dominant cost and
  the field is trilinear over metre-scale probes, so a one-step hold
  stays under its own filtering radius. Measured ~0.5 ms (M1 Debug).
- Screen-space god rays remain (foliage, sunsets — taste call),
  deliberately CLOUD-BLIND (a cloud coupling was tried 2026-07-30 and
  reverted: it overpowered and drowned the tree/ground occluders). The
  shaft fade now survives the sun sitting a full screen off-edge
  (side-lit shafts; the radial march converges fine off-screen).
- **The cloud-ray-curtain chain** (ONE mechanism, the same knobs —
  fogDensity/fogCeiling, fogSunScatter/fogSunPhase × the volumetric
  slider, cloudShadowFactor for the pattern): the froxels own
  [fogStart, volumetricReach] — ground-to-cloud shafts and shadow
  curtains, any time of day; the surfaces' analytic `applyFog` tail
  (with its one cloud tap) owns beyond. The SUN term rides a 3x SOFTER
  altitude envelope than the extinction (fogCeiling × 0.35 — the sky
  stays readable while the curtains keep their medium) and FADES toward
  the cloud base (0.45→0.9 of cloudHeight — kilometres of near-base air
  otherwise pile into a glowing cushion under the deck). A separate
  far-sky "tail" march was tried in the sky-cloud pass (2026-07-30) and
  REMOVED after two containments: all it could still light was the air
  BETWEEN the clouds — negative visual value (the tuning-form field
  `skyCloudRays` stays, unused, so records keep resolving).
- **Horizon closure** (`fogLayerInfo.y` = the terrain streaming edge,
  composer-set from the ring radius): `applyFog` guarantees geometry is
  FULLY dissolved into the sky gradient over the last quarter of the
  streaming ring, whatever the weather's fog — chunks are born inside
  the veil instead of popping and trees stop sprouting from bare
  ground. 0 disables (tool scenes without terrain). The ring radius is
  LIVE-TUNABLE (`TerrainSystem::viewRadius`, panel "View radius",
  `LandscapeTuningForm::terrainViewRadius`, 8-30 chunks = 512-1920 m;
  chunk count grows (2r+1)² — watch F6).
- **Far terrain** (`FarTerrain`, 2026-07-30 — the "see the landscape"
  chantier): ONE coarse worker-baked grid (193², ~12 km span, 62 m
  cells, ~220k tris) of the same height function, drawn under the near
  terrain in the main pass — ridgeline silhouettes to ~5 km, painted
  with the SHARED `terrainColor` palette and raised+darkened by the
  SHARED `forestMask` (both made public for it), so the distant forest
  fringe continues the real scatter past the vegetation ring.
  **In-ring containment is MIN-SAMPLING, not a sink** (fix 2026-07-31:
  the original fixed 12 m sink could not absorb a 62 m cell's
  interpolation overshoot on slopes, and the baked canopy raise ate
  most of it — big triangles rose through the fine terrain). The bake
  takes each vertex as the MIN of the true height over its quad
  support (half-cell grid) so a coarse triangle can never rise above
  the true surface; the residual 12 m in-ring sink only covers
  sub-half-cell relief, and what the min gave up (true-height delta +
  canopy raise) rides uv.x, restored by the vertex shader beyond the
  streaming ring where the crests must keep their real silhouettes.
  The horizon closure moves out to `FarTerrain::reach()` (~5 km) when
  it stands in. Flat shading (color × (ambient + sun·N·L × cloud
  shadow)) + `applyFog` — the veil does the silhouette work. Rebake on
  1 km stray; toggle "Far terrain" (persisted `farTerrain`).
  **Tree impostors** ride the same bake: cylindrical billboards
  scattered with the REAL forestMask + tree gates at 3.5x the real
  spacing (700 m → 5.2 km, IGN-dither dissolve at both ends, so they
  take over exactly where the true trees fade ~880 m). The silhouette
  is ANALYTIC in the fragment (hash-jittered crown discs + trunk — the
  lobe-tree read at distance, no texture bake), instanced in one draw
  (the vegetation Instance layout, corners from the vertex index), lit
  flat (ambient + sun × cloud shadow) and dissolved by `applyFog`.
  **Impostor size/shape is MEASURED, not authored**:
  `VegetationSystem::treeSilhouette()` averages the AABB of the
  generated tree variants (height, crown-width ratio, bare-trunk
  fraction, captured in `uploadVariantMesh`) × the scatter scale range
  — instance height, billboard width (`aParams.z`), trunk/crown split
  (`aParams.w`) and the far-mesh canopy raise (~60 % of tree height)
  all derive from it, so a future tree-type change reshapes the far
  woods automatically (the bake keys on the silhouette height and
  re-runs when the async variant meshes land). A pre-rendered impostor
  atlas from the real tree pipeline remains the upgrade path if the
  analytic read ever falls short.
- **Bilateral composite** (the ground-mist brique 8, done): the
  tonemap's ½-res volumetric taps (mist + sky clouds) are
  depth-weighted (full-res depth at binding 8) — silhouette edges stop
  bleeding 1-px halos across ridges and cloud/mountain boundaries.
- **Mist puffiness** (`mistPuffInfo.x`, panel + persisted): the cloud
  floret pass ported to the mist's patch borders (3.7x fine tap, edge
  band only, dropout + segment-LOD gated).

#### Ground mist — the erasing mist (2026-07-29)

A SEPARATE raymarched medium (`mist.frag`, PostFx pass, tonemap
binding 4) for the world-eating valley mist — not fog tuning: authored
density with crisp fronts, which the froxel grid is too coarse to hold.
Schneider/Frostbite family: envelope × noise erosion, Beer-Lambert +
powder + dual-lobe HG, IGN-jittered ½-res march (16 steps default,
live knob) + temporal EMA.

- **Density** = valley envelope × coverage gate × vertical profile ×
  3D erosion. The envelope is CPU-baked (`MistMap`, 256²/2048 m RGBA8,
  worker; R = box-blurred terrain height = the "water table" the mist
  pools under, G = valleyness `(smoothed − h)/16 m` — ordinary vales
  read as misty, not only gorges; underwater floors are gated out with
  a ±2/+3 m shore fade, or the sea/lakes would read as perfect valleys
  and blanket the water). Bake is
  sun-independent and center-snapped to the texel grid ⇒ overlap texels
  of consecutive bakes are bit-identical ⇒ **no rebake crossfade needed
  by construction**. Coverage = wind-drifted `cloudFbm` threshold
  (which valleys hold mist — sparse so the player keeps landmarks);
  erosion = shared `NoiseVolume` Perlin-Worley 128³ (analytic `fbm3`
  fallback + A/B), mean-preserving distance dropout.
- **Lighting contract**: mist RECEIVES `cloudShadowFactor` + one CSM tap
  per step; ambient = `giAir` with a shadow floor; sun transmittance is
  ANALYTIC (Beer over the slab-exit path toward the sun + one ceiling
  refinement tap — no light march). The powder term is DIRECTIONAL
  (blended out toward the sun, the Schneider/repo formula) — applied
  flat it kills the silver lining at the thin lit rim; the sun beam
  carries a gain knob (`mistSunBoost`, the normalized HG phase alone is
  too dim against the full-sky ambient). The light-shaping kit lives in
  `mistLightInfo` (panel "Mist lighting", persisted): forward HG lobe g
  (rim tightness), backscatter weight, ambient gain (the silver-lining
  contrast is ambient-vs-sun) and shadow floor. Mist does NOT cast shadows, is NOT
  injected into RC or the froxels (v1), does not appear in the
  reflection pass or interiors. The `volumetric shafts` slider does not
  drive it — density/coverage are per-weather (`WeatherForm.mistDensity/
  mistCoverage`, ~30 s crossfade), structure knobs are
  `LandscapeTuningForm.mist*` (Save render tuning persists them).
- **Composite order**: contact → **mist** → froxel/2D fog → bloom → god
  rays — the air fog veils distant mist, never the reverse (both media
  overlap in the same meters; independent transmittances multiply ≈
  combined Beer; only additive glow can stack — tune misty weathers'
  `fogLowBoost` down). Debug buffer 4 isolates the target.
- **Temporal**: world-space reprojection of the pixel's depth point
  (`temporal_resolve.glsl`, reusable), EMA 0.15 with a soft clamp toward
  the current sample, history = `postcopy` blit of the target (no
  ping-pong — the tonemap blit group stays static), golden-ratio-rolled
  IGN. Invalidated on camera jump >10 m / resize / mist-off frames.
  NEAR geometry (< ~25 m ray length) takes little to no history: its
  reprojection error is the largest and moving foreground objects
  (carried weapon, swaying grass tips) would drag mist ghosts — that
  plus the clamp tolerance is the anti-trail contract.
- **Slab clip**: the ray is clipped to
  `[seaLevel−64, maxBakedTop+lift]` before marching — sky and peak
  pixels exit before the loop; `NoiseVolume` bakes on frame 1 (one-shot
  ~50 ms during scene load, never mid-play).
- **Measured (M1 Air Release, worst case: camera INSIDE dense mist,
  density 0.12, full screen): 1.6/1.9 ms avg/max**; typical distant-
  valley mist is far cheaper (slab clip + coverage early-outs), mist-off
  is a ½-res clear. In dense mist the transmittance early-out ends the
  march at the same optical depth regardless of the step knob — the
  per-step cost is not the lever there. History via `postcopy` blit, NOT
  `copyTexture` (that cost ~1.0 ms of MoltenVK layout transitions).
  Escalation levers, in order: steps 16→12, ¼-res + temporal (HZD
  pattern — the temporal brick makes the switch trivial), MistMap 128²,
  erosion dropout closer.
- **Shared with the future volumetric sky clouds** (§8 contract):
  `volumetric_media.glsl` (HG/dual-lobe/powder/Schneider-remap/
  multi-octave scattering/integration), `NoiseVolume` (R channel + B/A
  reserved for the sky), `temporal_resolve.glsl`, and the ½-res+temporal
  pattern proven here. A volumetric sky would bake its vertical
  transmittance into the existing 512² map behind `cloudShadowFactor` —
  the GLSL seam does not move.
- **Vulkan note**: post passes must bind their extra maps at their OWN
  slots (`renderMist`: cloud map @5, mist map @6) — `PostFx::render`
  overwrites slot 3 with the GI group, and an overwritten Vulkan slot
  loses its bindings where GL's texture units stay sticky. The
  pre-existing case was CONFIRMED AND FIXED (2026-07-30): the cloud-map
  group bound at slot 3 before `postFx.render` was clobbered by the GI
  group, so the froxel inject and the 2D march sampled a DUMMY for
  `uCloudMap` on Vulkan — the fog's cloud-shadow ray curtains were dead
  on macOS the whole time (GL sticky units masked it). `PostFx::render`
  now takes the cloud-map group and binds it at slot 5 for both fog
  paths.

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
- **Volumetric sky clouds** (2026-07-29, the §8 swap, on the ground-mist
  socle): `skyclouds.frag`, a ½-res+temporal PostFx pass (the exact mist
  family: history blit, EMA, golden-rolled IGN) marching the altitude
  slab `[cloudInfo.y, +cloudVolInfo.y]`. Density = the SAME analytic
  coverage field as the dome and the shadow bake
  (`cloudDensityAnalytic` — ground shadows match the volumetric clouds
  by construction, the `cloudShadowFactor` seam untouched) × a rounded
  vertical profile (cores tower with coverage) × Perlin-Worley erosion
  (`NoiseVolume` R channel — the lane reserved for the sky). Lighting =
  3-tap sun march feeding `mediaMultiOctaveScattering` + directional
  powder + height-graded sky ambient; knobs in `cloudVolInfo`/
  `cloudVolLightInfo` (panel "Sky clouds", persisted in
  LandscapeTuningForm). Tonemap binding 7, composited FIRST (farthest
  medium — mist and fog veil it); scene depth occludes (mountains mask
  clouds behind them); debug buffer 5. When active it gates the 2D dome
  layer — RESOLVED only, so the planar REFLECTION pass draws the 2D
  layer (same coverage field, so the patterns agree). **Clouds in the
  water** come from screen-space reprojection, not the mirror: the
  water shader (uSkyClouds, binding 4) follows the reflected ray from
  the main camera — clouds sit at quasi-infinity, so direction alone
  reprojects them into last frame's cloud display buffer — and
  composites `refl × a + rgb` where it lands on-screen, fading to the
  mirrored 2D dome near the screen edges. Occlusion is inherited: a
  ridge the main view sees leaves the buffer neutral there. Gated on
  the NoiseVolume caps: without compute+3D textures the 2D dome IS the
  fallback. **Far clouds** (the classical
  flattening): sky pixels bypass the depth clamp (the far-plane
  reconstruction sits closer than a slanted slab entry — without this,
  clouds exist only overhead), the volumetric fades out over elevation
  0.10→0.04 and the 2D dome fades IN complementarily (`domeShare` in
  `applyClouds`) — a grazing slab is one 2D sample, and the dome is
  that sample on the same coverage field. Cost control: step count
  scales with the traversed span (8 overhead → 24 grazing), the sun
  march drops to 1 tap beyond 3 km, span capped at 15 km. The sun
  lighting is TWO terms — multi-octave body + direct-transmission
  lining `exp(−τ)·HG(g≈0.9)` (the silver lining; per-tap light-march
  coverage so edges measure thin) — and the slab thickness scales with
  the weather's coverage (`cloudVolShapeInfo.x`, up to ×5: full skies
  tower). **Shape (the Nubis 2015/2017 subset)**: per-column top
  variation (a fixed-z volume tap), height-mixed erosion (wispy
  Perlin-Worley bases → round Worley billows at the tops), a fractal
  floret pass on the low-density edge band (`skyCloudPuffiness`), and a
  curl-style domain warp (slow vertical advection — boiling edges;
  amplitude rides puffiness). **Anti-flicker contract**: steps target a
  fixed ~40 m segment (max 40) and EVERY noise octave — warp included —
  fades toward its mean once a segment exceeds half its wavelength (the
  mist LOD lesson; light-march taps too, so the lighting stays stable
  on tall slabs); plus a display-only 3×3 blur on the composite tap
  (the froxel lesson — the EMA history stays sharp). Measured (M1
  Release, coverage 0.3): ~0.3-2.3 ms depending on sky content.
- **Water**: global sea plane (planar mirror + oblique clip; culling
  frustum from the NON-oblique projection — Lengyel's trick corrupts the
  far plane), pool-depth bake killing foam on small ponds, submersion
  tint via effective-surface uniform. The pipeline carries a NEGATIVE
  depth bias (-4/-2.5): where the sheet lies centimetres over the
  ground (shallow shelves, flooded meadow dips) the two planes sit
  inside the far-field 0..1-depth quantization noise and the surface
  flickered per pixel — contour-line moiré fringes; the bias rides the
  format's local precision so near geometry is untouched (retire with
  reversed-Z, roadmap). **Placed water volumes**
  (`WaterVolumeForm`, spec decided): box volumes whose top face is the
  surface, per-volume tint/chop, sea keeps the mirror, volumes get a
  "deaf" sky-fresnel surface; per-cell water heights rejected (two
  mechanisms for one thing).
- **Stylized pass** (`stylized.glsl`): 2-step BotW ramp, snapped CSM
  pools, gated SSS, stepped rim, all behind `uAmbientColor.w` A/B.
  The cel ramp lives on the SUN only — local-light diffuse stays smooth
  by design ("stylized shadows, soft lighting": routing torch pools
  through a hard step cut them with a brutal edge). Key-light shadows
  de-detach via a receiver NORMAL offset + tiny z-bias (a constant
  z-bias in a perspective shadow map is meters of world detachment).
  **Grading**: analytic vibrance/split-tone/contrast in tonemap (LUT 3D
  someday — deferred spec in §6.1). **Auto-exposure**: log-luminance 64² → mip 1×1 → asymmetric
  adaptation ping-pong; slider is the EV bias. **Contact shadows**
  (Bend-style, ½-res, 12 steps toward the sun; toggle = white-cleared
  texture; NEAREST depth/color taps — linear taps at thin silhouettes
  blended near/far into phantom hits around grass-blade tips; fades out
  below ~0.3 sun elevation — the grazing march read the terrain as its
  own occluder, a half-res darkening film that SPARED the strip behind
  raised occluders: inverted bright "shadows" behind rocks at sunset;
  GATED to sunlit pixels via the STYLIZED CSM factor AND the stylized
  DIFFUSE ramp (receiver normal from depth derivatives) — the tonemap's
  fullscreen multiply otherwise darkened the ambient inside CSM shadow
  and below the terminator (down-sun slopes at grazing sun: unoccluded,
  so the CSM gate alone stayed open), a second darker shadow where
  receiver holes read as anti-shadows. The composition contract: the
  direct term = sun x stylizedDiffuse x stylizedShadow — contact must
  be neutral wherever ANY factor is already zero; it is sunlit-area
  detail, never a shadow of its own).
  **Terrain light map** (worker-baked 512², ~1.5 km): R = far
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
after round-robin+cull; upload stall re-measure (P6 — the
persistent-mapped staging ring IS built, §1.2; what remains is
re-measuring the terrain streamer and a byte budget if it still bites);
grass = knobs + measurement only until its visual redo; vegetation
imposters (deferred decision); per-queue GPU timestamps and the
transfer queue are BUILT (§1.2) — first rc* reading on M1: inject 3.0 +
build 2.5 + merge 3.3 ≈ 8.9 ms of async chain, which promotes the RC
tuning pass (targets 1–2 ms via the GI panel knobs) to the biggest
single lever on the table.

### 4.4 View distance at scale (chantier 2026-08-02)

The terrain ring is live-tunable **8–45 chunks (512–2880 m), default
30**. Everything that pinned the old 960 m ring now derives from the
radius — the checklist, for the next time a cap hides somewhere:

- **Camera far plane** follows the ring (`updateCameraFarPlane`,
  ≥ ring × 1.3; the fixed 1600 clipped everything past it).
- **Terrain LOD4** (4 quads/side) beyond 12 chunks; `lodForDistance`
  bands and the pool cores (`kLod3CoreSide`/`kLod4CoreSide`) are the
  same truth, `kMaxViewRadius` is THE single slider/pool/clamp cap.
  Pools ≈ 45 MB at radius 45 (LOD4 slots are 2.6× cheaper than LOD3).
- **ChunkOcclusion** rings/rays follow the ring (`configure`): reach =
  the full ring, fan doubles past 1 km so a ray still subtends < 1
  chunk. Horizon table cost grows with both.
- **GpuOcclusion** `kMaxCandidates` 49152: a candidate without an
  indirect command NEVER draws, so the list must never truncate —
  a clip logs loudly and falls back to the full CPU path.
- **Vegetation**: ring 4–24 chunks; the per-instance tree fade
  (`treeFadeEnd`, baked at scatter) and the far-impostor fade-in
  (`uFogLayerInfo.w`) track it together; instance pool sized for the
  max ring (~40 MB).
- **TerrainLightMap** span 3072 m (1024² keeps ~3 m texels).
- Request budget doubles past radius 24 (cold-start fill).
- Does NOT scale: grass (192 m by design), shadow cascades (800 m),
  FarTerrain (12 km, already past any ring), fog closure (reads the
  ring from the UBO).

M1 Air protocol at radius 30 and 45: F6 four-spot numbers + GPU memory
(pools + instance pool + Hi-Z buffers ≈ +75 MB vs the r15 build), and
the `GpuOcclusion … clip` warning must never appear.

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

### 6.0a Chantier REVERSED-Z (in progress)

Uniform depth precision to the horizon (0..1 non-reversed loses
~d²·1.2e-6 m; two workarounds already shipped — FarTerrain's
distance-growing sink, the water pipeline's negative bias). Scope: the
CAMERA-PERSPECTIVE path only. The whole shadow path (CSM ortho, rainOcc
ortho, keyShadow perspective-light, caster pipelines `Less`, PCF
samplers `LessEqual`, positive caster biases) and the 2D sprite path
stay non-reversed — ortho depth is linear, there is nothing to fix, and
the light-space conventions are self-consistent.

- **R1 — matrices/pipelines/clears**: `Camera3D::proj` swaps
  near/far in `glm::perspective` (the ZO idiom); the 12 camera
  pipelines flip `Less→Greater` (sky `LessEqual→GreaterEqual`); main +
  reflection passes clear depth to 0; water bias signs flip to
  positive; `SceneEditor::mouseRayDirection` swaps its near/far ndc z;
  `Frustum` near/far labels updated (volume unchanged — plane
  extraction is label-agnostic).
- **R2 — oblique clip rederived**: the mirror plane becomes the NEAR
  boundary (ndc z = 1, clip z = w); the preserved corner q sits at ndc
  z = 0. rowZ' = rowW − C/dot(C,q) with q = inv(P)·(±1, ±1, 0, 1).
- **R3 — Hi-Z & shader constants**: hiz_first/hiz_down max→min
  (farthest = smallest now), chunk_cull nearest=max / compare and
  guard flips, sky.vert far-pin 1→0, sky-detect epsilons in
  godrays/contactshadow/skyclouds.
  **Lesson — ray constructions hide depth constants.** Three shaders
  built per-pixel ray DIRECTIONS from two unprojected ndc depths
  (froxel_inject/cluster_cull at 0.1/0.9, rc_debug at 0/1) — not
  comparisons, so the audit grep missed them. Reversed, `far − near`
  pointed BACKWARD: the froxel fog injected for the opposite
  hemisphere (its sky ceiling tested altitudes behind the camera — fog
  shimmering in the sky) and the light clusters binned the wrong
  cells. Near is ndc z ≈ 1 now; the constants swap.
  **Lesson — sky epsilons do NOT mirror.** `depth >= 0.99995` flipped
  to `<= 0.00005` classified all terrain beyond ~900 m as sky (the
  reversed hyperbola flattens toward 0 far faster than the old one
  flattened toward 1): the sky clouds and god rays marched over the
  far hills and the epsilon frontier shimmered per pixel. Sky is
  EXACTLY the far clear (0.0, the dome writes no depth) — the correct
  reversed test is `depth < 1e-8`, effectively "the untouched clear".
- **R4 — validation**: sync validation 0 hazards; budget line
  unchanged; the readback occlusion verdict is the ORACLE for the
  flipped Hi-Z (same occluded ballpark, panel counter) — one reason I6
  runs after this chantier; dev visual pass, then optionally soften
  the two precision workarounds to confirm the root cause is gone
  (they stay: harmless and they cover the GL fallback if it ever runs
  non-reversed).

### 6.0 Chantier GPU-DRIVEN INDIRECT (in progress)

Replace the Hi-Z cull's CPU fence readback with GPU-written indirect
draws — the cull's verdict never leaves the GPU. Motive: PC scales with
scene density through submission cost; the readback machinery
(fence/staging/pendingKeys + per-chunk CPU loops) is the part that
doesn't. Same one-frame verdict latency as today (pyramid from frame N
depth, commands consumed frame N+1).

- **I1 — RHI bricks (DONE)**: `BufferUsage::Indirect` (SSBO-writable +
  indirect-readable, its own `bufferReadScope`),
  `CommandBuffer::drawIndexedIndirect(buffer, offset, drawCount,
  stride)` + `rhi::DrawIndexedIndirectCommand` (identical Vulkan/GL
  layout), `DeviceCaps::multiDrawIndirect`, `BarrierStage_Indirect`.
  GL46 = `glMultiDrawElementsIndirect`; GL41 caps off (CPU path
  remains, degraded-mode contract).
- **I2 — terrain vertex pool (DONE)**: per-chunk vertex buffers became
  fixed slots in one pooled buffer per LOD (slot index × slotVerts IS
  the `vertexOffset`; 4485/1221/357/117 verts per slot). Sized for the
  slider's max radius (~30 MB total, no growth path; a full pool drops
  the upload with a warning and the chunk re-streams). Uploads ride
  the upload queue — whose last-frame-graphics WAR wait is what makes
  overwriting a live slot legal. Freed slots COOL FOR TWO FRAMES
  before reuse (a stale command may still reference them: one frame of
  ping-pong + one of cull back-pressure).
  **Lesson — pool-full must STEAL, never just drop.** V1 dropped the
  upload and re-requested: in fast flight the trail of chunks awaiting
  their LOD swap holds every near-LOD slot, and the CENTER-OUT request
  budget (8/frame) is then entirely consumed by the near ring's doomed
  re-requests — the far swaps that would free the slots are never even
  asked for. A LIVELOCK: the terrain under the camera stops streaming
  for good. Fix: `stealFurthestSlot` frees the furthest resident chunk
  of that LOD (overdue for its swap anyway; the far mesh covers it) so
  the retry converges once cooling passes, plus LOD 0-2 headroom of
  several rings (64/128/384 slots, ~46 MB total).
- **I3 — chunk_cull writes commands (DONE)**: candidates carry
  group(=lod)/indexCount/vertexOffset, arrive counting-sorted by group;
  the dispatch writes one command per candidate (culled =
  `instanceCount 0` — no drawIndirectCount dependency), into a
  PING-PONG pair (the frame consuming side N-1 never races the write).
  The command adds a frustum test with a 1.15 guard band (commands are
  consumed with a one-frame-stale camera); the CPU-readback verdict
  stays pure occlusion (its consumer applies a fresh frustum).
- **I4 — terrain indirect draw (DONE)**: per LOD one
  `drawIndexedIndirect(drawCount = lodCandidates)`; the cull's barrier
  covers Transfer | Indirect. "Indirect draw" checkbox next to the
  Hi-Z one (A/B); falls back to the per-chunk loop whenever the
  commands aren't fresh (interiors, back-pressure > 1 frame, GL 4.1).
  Sync validation: 0 hazards, 75 s. **Pending dev visual A/B** (valley
  floor + fast rotation + LOD-swap watch: a wrong-mesh flash would
  mean the slot cooling window is too short).
- **I5 — vegetation indirect (DONE)**: per-chunk instance buffers pool
  into ONE buffer (variable-size blocks, first-fit + coalescing,
  two-frame cooling like the terrain slots; ~12 MB, overflow drops the
  chunk's scatter with a warning and it re-streams). Candidates: one
  per chunk×variant, group = 4 + variant*3 + level — the LOD level is
  picked at candidate time exactly like draw() picks it, consumed one
  frame later. The command carries
  `instanceCount = counts[v]` and `firstInstance = poolOffset +
  firstInstance[v]` (the cull entry grew a second uvec4;
  kMaxCandidates 8192, kMaxGroups 40 — a clipped candidate list makes
  run() report the commands unfit and the frame falls back to the CPU
  loops, since a missing command would be silently missing geometry).
  The vegetation readback keys only ever ADD verdict entries the
  terrain AABBs already imply (bigger boxes). Showcase mode and the
  reflection/shadow passes stay on the legacy path. Sync validation:
  0 hazards, 75 s. **Pending dev visual A/B** (forest belts at every
  LOD ring boundary + chunk streaming while flying).
  **Lesson — the command frustum needs TWO tests.** V1 kept the Hi-Z
  verdict's "near-plane corner → visible" conservatism: the whole back
  half of the ring drew (the vegetation pads make every nearby box
  near-plane-adjacent) and mainPass DOUBLED (6.6 → 13.7 ms on M1).
  Boxes the projection can judge use the proportional NDC guard band
  (1.15 — covers a frame of rotation at any distance); near-plane
  straddlers use the frustum PLANES with a 16 m world margin (they are
  close, a frame of motion is small in meters). mainPass 13.7 → 8.0;
  the remaining ~+1 ms vs the CPU loop is the guard band's overdraw —
  the GPU-driven trade (M1 pays a little vertex work, the PC sheds its
  per-chunk submission cost), tunable via the guard scale if it bites.
- **I6 — readback retirement (DONE)**: stagingBuffer/fence/pendingKeys/
  collectResults and the chunk_cull visibility SSBO are gone — the
  cull's verdict lives ONLY in the indirect commands and never crosses
  the CPU. The legacy draw paths (interiors, GL 4.1, candidate
  overflow) keep `ChunkOcclusion` (CPU horizon) + fresh frustum; the
  panel's "occluded GPU" counter went with the readback (the culling
  now shows in `drawn` when the indirect path is on). The chantier is
  COMPLETE: the Hi-Z pipeline is pyramid → cull → commands, one
  dispatch, zero round-trips.

Out of scope (deliberate): shadow/rain/reflection passes keep their
Chebyshev/frustum CPU culling (they never consulted the occlusion
verdict); grass and FarTerrain are outside the verdict entirely.

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

**Perf**: RC tuning pass (~8.9 ms of measured async chain → 1–2 ms
targets — the biggest lever since the per-queue timestamps landed);
reversed-Z (glClipControl already 0..1 by design, "one switch away"):
the distance z-fight class — FarTerrain vs sea/flats needed a
distance-growing sink (far_terrain.vert) that reversed-Z would retire,
along with every other far-field depth-precision workaround;
P5b CSM static/dynamic split (only if shadows still dominate);
P6 terrain upload stall (staging ring + upload queue built — re-measure,
byte budget if it still bites); P7 grass knobs+measure (structural work
waits for the visual redo); vegetation imposters (decision deferred —
the V8e geometry counters are the input); PC return bundle:
re-baseline, GL 4.6 parity smoke, sync validation on the PC driver,
CONCURRENT→EXCLUSIVE sharing refinement.

**Features**: placed water volumes (brique-32 spec in archive:
surface+submersion first, no foam v1, swimming = gameplay chantier);
cumulonimbus/rain polish as WeatherForm content; GL 4.1 degraded mode;
TAA, 3D LUT, caustics, Gerstner water — backlog.

**Ground-mist deferred (reviewed 2026-07-29, end of chantier)**: the
optimization bricks (NoiseVolume, temporal EMA, coverage-lerp) were
built INTO the chantier, not deferred. Still open: (1) bilateral ½-res
upsample — conditional on the dev SEEING silhouette halos in the tuning
session (recipe: depth-weighted 4-tap in tonemap.frag + a ½-res depth
fetch); (2) gameplay coupling of the mist (region signal for the sim —
danger/visibility/progression) = a sim chantier, Forms + References,
headless (§2.10); (3) volumetric sky clouds on the shared socle (§8
contract — see "Chantiers ready to start"); (4) TerrainLightMap.R as
far sun visibility in the mist march if lit far mist in mountain shadow
reads wrong; (5) GL 4.6 runtime parity smoke on PC (macOS runs GL 4.1
only — compile-verified here, runtime check rides the PC return
bundle).

**Chantiers ready to start**: the new clouds implementation against the
§8 contract (RENDERER-EXTRACT §7 is complete — follow-ups: tree types →
forest scatter; AnimPreviewPanel onto a configured WorldRenderer;
postFx-less blit fallback hardening on Vulkan). The ground-mist chantier
(§3.5, 2026-07-29) pre-built the sky-clouds socle: `volumetric_media.glsl`,
`NoiseVolume`, `temporal_resolve.glsl` and the ½-res+temporal pattern.

### 6.1 Post & grading — deferred ideas (survey of SH2 "New Dawn", 2026-08-12)

Reviewed a state-of-the-art Skyrim ENB stack; most of it is cinematic
garnish or already covered (SSAO, bloom, volumetrics, GI). Three ideas
retained as cheap, high-leverage deferrals — all tonemap-pass-local,
none blocks anything:

1. **3D LUT grading, blended per state** — upgrades the "(LUT 3D
   someday)" note in §3.6: one tetrahedral-interpolated LUT fetch in
   the tonemap replaces/augments the analytic vibrance/split-tone. The
   leverage is making LUTs DATA (§5): a base LUT plus per-weather /
   day-night / interior variants layered like any Form, blended by
   states the frame already knows (`interiorMode`, quantized sun
   elevation, `buriedBelowY`, `WeatherForm`). First candidate if the
   terrain-texturing DA verdict calls for a global color-coherence
   tool.
2. **Blue-noise dither before quantization** — a gaussian-distributed
   blue-noise nudge in the tonemap output. Trivial, and our stylized
   flat-albedo gradients (sky domes, fog banks) are the worst case for
   8-bit banding.
3. **Log-space grading hygiene** — when the tonemapper is next
   touched: run grain/grading math in log space before the output
   curve (stabler highlight compression, better-behaved artistic
   controls, LUTs included).

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
