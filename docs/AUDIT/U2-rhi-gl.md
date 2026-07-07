# U2 — RHI + backend GL — Audit report

Scope: `engine/rhi/` (Device, CommandBuffer, Rhi interface + integrated compute)
and `engine/rhi/backends/gl/` (GlDeviceBase + GlDevice41/GlDevice46 split).

## Verdict

**Healthy unit.** The interface genuinely delivers on §2.1: it is built around
explicit Vulkan-shaped concepts — `CommandBuffer`, `PipelineDesc` (PSO),
`BindGroupDesc` (descriptor set), `RenderPassDesc`, `FramebufferDesc`, immutable
samplers, `DeviceCaps` feature flags — with Vulkan-mapping comments throughout.
A Vulkan backend could be added without touching callers, with two documented
exceptions (GLSL shader source in `ShaderDesc`, ImGui's own GL backend). The
41/46 split is clean: `GlDeviceBase` owns all resource maps and shared logic;
the two subclasses override only the 7 methods that actually diverge
(DSA vs bind-first). Residual issues are small: some pipeline-creation
duplication, hot-path `unordered_map::at` (exceptions + hashing per draw), and
the cross-cutting handle-struct proliferation (H-d).

## Invariant checks (explicit pass/fail)

- **§2.1 — no GL calls outside the backend: PASS.** Grep for `gl[A-Z]…(` / `GL_`
  across the tree finds GL only in: `engine/rhi/backends/gl/` (correct),
  `engine/platform/common/GlContext.cpp` (legitimate — platform owns context
  creation), `engine/ui/ImGuiLayer.cpp` (Dear ImGui's own `imgui_impl_opengl3`
  backend — accepted dev-UI exception per CLAUDE.md §3), and `_old/` dead code.
  `engine/render/SpriteRenderer.cpp` matched a comment only — the renderer goes
  through `rhi::Device`. No gameplay/world/data GL leakage.
- **§7 asset-upload path behind RHI: PASS.** All uploads
  (`createTexture`/`updateBuffer`/`createBuffer`) are `Device` methods; the
  GL-thread caveat stays a backend detail. No caller touches GL to upload.

## Findings

| id | sev | axis | file:line | description | action | effort | inter-unit |
|----|-----|------|-----------|-------------|--------|--------|------------|
| U2-01 | med | qualité | GlDeviceBase.cpp:134,194,230,238,246,253,312 | Render hot path (setPipeline/setBindGroup/setVertexBuffer/draw/drawIndexed/copyTexture) uses `unordered_map::at`, which throws on a bad handle — exceptions in a hot path (§8) plus a hash lookup per draw call and per bind-group entry per frame. | Resolve handles via `find`+`ENGINE_ASSERT`/early-return, or store resources in flat slot arrays indexed by handle id (removes hashing too). | M | false |
| U2-02 | med | réutil | Rhi.hpp:25-31 | 7 near-identical handle structs (`{ u32 id{0}; }`) with no shared type, no `operator==`, no `explicit operator bool`; callers compare `.id != 0` by hand everywhere. Same pattern recurs in reflect/GameplayTags/Form (H-d). | Introduce a `core::Handle<Tag>` (id + `valid()`/`==`); alias the RHI + Form/Body/Subscription handles to it. | M | **true** (H-d) |
| U2-03 | med | factor | GlDevice41.cpp:105-113 & GlDevice46.cpp:256-264 | `createPipeline` copies the same 8 `PipelineDesc`→`GlPipeline` fields identically in both backends; only the VAO/attrib setup differs. | Extract a shared `fillCommonPipelineState(GlPipeline&, const PipelineDesc&)` in `GlDeviceBase`. | S | false |
| U2-04 | low | archi | Rhi.hpp:139-162 | `ShaderDesc` carries raw GLSL `vertexSource`/`fragmentSource`/`computeSource` — the one real GL-ism in the interface; a Vulkan backend needs SPIR-V. Documented as the intended future path, but it is the seam a second backend will hit first. | Track as known Vulkan-readiness debt; when a 2nd backend lands, move to offline SPIR-V cross-compile as the comment already anticipates. | L | false |
| U2-05 | low | propreté | GlDeviceBase.cpp:46-63 | `compileStage` hard-codes the log label to "vertex"/"fragment" (`stage == GL_VERTEX_SHADER ? … : "fragment"`), but it is also called for `GL_COMPUTE_SHADER` (:422); a compute compile error is mislabeled "fragment". | Map the stage enum to its real name for the log. | S | false |
| U2-06 | low | factor | GlDevice41.cpp:9-12 & GlDevice46.cpp:11-15 | The shared free helpers (`glVertexFormatComponents`, `glToTopology`, `glToCompare`) are exposed from `GlDeviceBase.cpp` and hand-re-declared in each subclass `.cpp` — fragile (a signature change silently mismatches). | Put the declarations in a small internal header (e.g. `GlHelpers.hpp`) included by all three. | S | false |
| U2-07 | low | archi | engine/ui/ImGuiLayer.cpp:4,31,46,53 | The ImGui dev UI bypasses the RHI entirely via `imgui_impl_opengl3`; accepted per CLAUDE.md (Dear ImGui) but hardcodes GL, so a Vulkan backend would need the ImGui Vulkan impl wired in parallel. | Leave as-is; note in the backend-swap checklist that ImGui has its own backend to switch. | S | **true** (U8 seams) |
| U2-08 | low | propreté | Rhi.hpp:44 | `DeviceCaps::copyTexture` comment says "(later brick)" but `copyTexture` is implemented (GlDeviceBase.cpp:306, GL46 caps). Stale. | Drop the "(later brick)" note. | S | false |
| U2-09 | low | propreté | _old/renderer_test/ (multiple) | Dead pre-RHI renderer with raw GL/shader code still in the tree (surfaces in the §2.1 grep as noise). | Delete `_old/` (already flagged repo-wide). | S | **true** |

## Notes / non-issues

- `setBindGroup(u32 index, …)` ignoring `index` (CommandBuffer.hpp:33) is a
  documented forward-design for Vulkan descriptor-set slots, not a leak.
- `readBuffer` synchronous readback (Device.hpp:76) is designed with a Vulkan
  staging-copy+fence in mind (comment) — fine.
- `new GlDevice46(...)` wrapped immediately into `uptr` (GlDevice.cpp:104-110)
  is RAII-safe; could be `make_unique` but the return-to-base makes the raw form
  reasonable.
- Core aliases (`u32`, `uptr`, `str`, `vector`, `f32`) are used consistently —
  no `std::uint32_t`/raw-type drift in this unit.
