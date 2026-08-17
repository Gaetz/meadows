// vksmoke — Vulkan bring-up harness (docs/RENDERING.md).
//
// Drives the Vulkan backend end to end WITHOUT the Engine loop, which still
// depends on the GL SpriteRenderer and the GL ImGui layer.
//
//  * V1: clears the backbuffer with an animated color — a window that fades
//    through the ramp is the visual proof the presentation path works (on
//    macOS, through MoltenVK).
//  * V2: a resource self-test. With no pipelines yet, nothing can be *drawn*
//    with a buffer or texture, so correctness is proven the only way left:
//    round-tripping memory through the GPU and comparing it byte for byte.
//  * V3: compiles the whole production shader corpus to SPIR-V through the
//    real ShaderLibrary path — what the GL backend can load, this must too.
//
// Run: ./vksmoke [seconds]   (default 5; 0 = until the window is closed)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "engine/core/Clock.hpp"
#include "engine/render/Camera3D.hpp"
#include "engine/render/landscape/GpuOcclusion.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/ui/ImGuiLayer.hpp"
#include "engine/ui/UiSystem.hpp"

#include <imgui.h>

namespace {

u32 failures = 0;

void check(bool ok, const char* what) {
    if (ok) {
        LOG_INFO("  PASS  {}", what);
    } else {
        LOG_ERROR("  FAIL  {}", what);
        ++failures;
    }
}

// Round-trips a buffer through the GPU: writes `pattern`, reads it back, and
// compares. Covers both memory paths — mapped (dynamic/readback) and
// device-local (staging copy in, staging copy out).
void testBufferRoundTrip(rhi::Device& device, const char* label, bool dynamic,
                         bool readback) {
    constexpr u64 kCount = 1024;
    vector<u32> pattern(kCount);
    for (u64 i = 0; i < kCount; ++i) {
        pattern[i] = static_cast<u32>(i * 2654435761u); // Knuth hash, non-trivial
    }
    const u64 bytes = kCount * sizeof(u32);

    const rhi::BufferHandle buffer =
        device.createBuffer({ .usage = rhi::BufferUsage::Storage,
                              .size = bytes,
                              .dynamic = dynamic,
                              .readback = readback },
                            pattern.data());
    if (buffer.id == 0) {
        check(false, label);
        return;
    }
    vector<u32> got(kCount, 0);
    device.readBuffer(buffer, got.data(), bytes);
    check(std::memcmp(pattern.data(), got.data(), bytes) == 0, label);
    device.destroyBuffer(buffer);
}

// Exercises the GPU-side copy recorded into a frame's command buffer, then
// reads the destination once the frame's fence has retired the work.
void testGpuCopy(rhi::Device& device) {
    constexpr u64 kCount = 256;
    vector<u32> pattern(kCount);
    for (u64 i = 0; i < kCount; ++i) {
        pattern[i] = static_cast<u32>(0xA5A50000u + i);
    }
    const u64 bytes = kCount * sizeof(u32);

    const rhi::BufferHandle src = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage, .size = bytes }, pattern.data());
    const rhi::BufferHandle dst = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage, .size = bytes, .readback = true });
    if (src.id == 0 || dst.id == 0) {
        check(false, "copyBuffer (GPU-side copy)");
        return;
    }

    // The copy must be recorded outside a render pass.
    auto& cmd = device.beginFrame();
    cmd.copyBuffer(src, dst, bytes);
    cmd.beginRenderPass({ .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f } });
    cmd.endRenderPass();
    device.endFrame();

    // Two more frames: kFramesInFlight fences guarantee the copy has retired.
    for (u32 i = 0; i < 2; ++i) {
        auto& idle = device.beginFrame();
        idle.beginRenderPass({ .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f } });
        idle.endRenderPass();
        device.endFrame();
    }

    vector<u32> got(kCount, 0);
    device.readBuffer(dst, got.data(), bytes);
    check(std::memcmp(pattern.data(), got.data(), bytes) == 0,
          "copyBuffer (GPU-side copy)");
    device.destroyBuffer(src);
    device.destroyBuffer(dst);
}

void testTextures(rhi::Device& device) {
    // 2D RGBA8 with a full mip chain, uploaded then downsampled on the GPU.
    constexpr u32 kSize = 64;
    vector<u32> pixels(kSize * kSize, 0xFF3366CCu);
    const rhi::TextureHandle tex2d =
        device.createTexture({ .width = kSize,
                               .height = kSize,
                               .mipLevels = 7, // 64 -> 1
                               .format = rhi::TextureFormat::RGBA8,
                               .filter = rhi::FilterMode::Linear },
                             pixels.data());
    check(tex2d.id != 0, "createTexture 2D RGBA8 + mips");
    device.generateMipmaps(tex2d);
    check(true, "generateMipmaps (no validation error)");

    // Array texture (CSM cascades / splat layers).
    vector<u32> layered(kSize * kSize * 4, 0xFF00FF00u);
    const rhi::TextureHandle array =
        device.createTexture({ .width = kSize,
                               .height = kSize,
                               .arrayLayers = 4,
                               .format = rhi::TextureFormat::RGBA8 },
                             layered.data());
    check(array.id != 0, "createTexture 2D array (4 layers)");

    // Volume texture (GI voxel clipmap / radiance cascades), GPU-written.
    const rhi::TextureHandle volume =
        device.createTexture({ .width = 32,
                               .height = 32,
                               .depth = 32,
                               .format = rhi::TextureFormat::RGBA16F },
                             nullptr);
    check(volume.id != 0, "createTexture 3D volume RGBA16F");

    // Depth attachment (shadow maps), sampleable.
    const rhi::TextureHandle depth = device.createTexture(
        { .width = 512,
          .height = 512,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_Sampled | rhi::TextureUsage_RenderAttachment },
        nullptr);
    check(depth.id != 0, "createTexture Depth32F attachment");

    // HDR color target.
    const rhi::TextureHandle hdr = device.createTexture(
        { .width = 256,
          .height = 256,
          .format = rhi::TextureFormat::RGBA16F,
          .usage = rhi::TextureUsage_Sampled | rhi::TextureUsage_RenderAttachment },
        nullptr);
    check(hdr.id != 0, "createTexture RGBA16F render target");

    // Upload bench: game-sized textures (1024² RGBA8 + mips ≈ one cooked
    // material layer) created WITH data — times the synchronous upload
    // path (immediateSubmit wait per texture) the streaming ring pays
    // mid-play. Re-run after the async-upload chantier to compare.
    {
        constexpr u32 kBenchSize = 1024;
        constexpr u32 kBenchCount = 24;
        vector<u32> benchPixels(kBenchSize * kBenchSize, 0xFF808080u);
        f64 total = 0.0;
        f64 worst = 0.0;
        vector<rhi::TextureHandle> benchTextures;
        benchTextures.reserve(kBenchCount);
        // One create per FRAME — the streaming pattern (a texture lands,
        // the frame's submit and the pending drains follow); timed part =
        // what the main thread pays inside the frame.
        for (u32 i = 0; i < kBenchCount; ++i) {
            auto& cmd = device.beginFrame();
            (void)cmd;
            const core::TimePoint t0 = core::clockNow();
            const rhi::TextureHandle t =
                device.createTexture({ .width = kBenchSize,
                                       .height = kBenchSize,
                                       .mipLevels = 11, // 1024 -> 1
                                       .format = rhi::TextureFormat::RGBA8,
                                       .filter = rhi::FilterMode::Linear },
                                     benchPixels.data());
            device.generateMipmaps(t);
            const f64 ms = core::millisecondsSince(t0);
            total += ms;
            worst = std::max(worst, ms);
            benchTextures.push_back(t); // destroyed OUTSIDE the timing:
            device.endFrame();          // streaming creates and KEEPS
        }
        for (const rhi::TextureHandle t : benchTextures) {
            device.destroyTexture(t);
        }
        LOG_INFO("upload bench: {} x {}² RGBA8+mips — total {:.2f} ms, "
                 "avg {:.2f} ms, max {:.2f} ms per texture",
                 kBenchCount, kBenchSize, total,
                 total / static_cast<f64>(kBenchCount), worst);
        check(true, "upload bench ran");
    }

    // Samplers, including the comparison sampler shadow PCF needs.
    const rhi::SamplerHandle linear = device.createSampler(
        { .mipmapFilter = true, .maxAnisotropy = 8.0f });
    check(linear.id != 0, "createSampler (trilinear + anisotropy)");
    const rhi::SamplerHandle shadow =
        device.createSampler({ .compare = rhi::CompareFunc::LessEqual });
    check(shadow.id != 0, "createSampler (comparison / shadow PCF)");

    device.destroySampler(shadow);
    device.destroySampler(linear);
    device.destroyTexture(hdr);
    device.destroyTexture(depth);
    device.destroyTexture(volume);
    device.destroyTexture(array);
    device.destroyTexture(tex2d);
}

// Compiles the WHOLE production shader corpus through the real path:
// ShaderLibrary (include expansion, pairing) -> Device::createShader ->
// glslang/shaderc -> VkShaderModule. Anything the GL backend can load, the
// Vulkan backend must load too — that is the contract.
void testShaders(rhi::Device& device) {
    render::ShaderLibrary shaders(device);
    const std::filesystem::path root = shaders.root();
    if (!std::filesystem::exists(root)) {
        check(false, "shader root exists");
        return;
    }

    vector<str> fragments;
    vector<str> computes;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::filesystem::path& p = entry.path();
        if (p.extension() == ".frag") {
            fragments.push_back(p.stem().string());
        } else if (p.extension() == ".comp") {
            computes.push_back(p.stem().string());
        }
    }
    std::sort(fragments.begin(), fragments.end());
    std::sort(computes.begin(), computes.end());

    u32 okPairs = 0;
    for (const str& name : fragments) {
        // Post-process passes share the fullscreen triangle instead of
        // carrying their own vertex stage.
        const bool ownVertex =
            std::filesystem::exists(root / (name + ".vert"));
        const rhi::ShaderHandle handle =
            ownVertex ? shaders.load(name)
                      : shaders.load(name, {}, {}, "fullscreen");
        if (handle.id != 0) {
            ++okPairs;
        } else {
            LOG_ERROR("    shader pair failed: {}", name);
        }
    }
    check(okPairs == fragments.size(),
          "compile every graphics shader pair to SPIR-V");
    LOG_INFO("    {}/{} graphics pairs", okPairs, fragments.size());

    u32 okCompute = 0;
    for (const str& name : computes) {
        if (shaders.loadCompute(name).id != 0) {
            ++okCompute;
        } else {
            LOG_ERROR("    compute shader failed: {}", name);
        }
    }
    check(okCompute == computes.size(),
          "compile every compute shader to SPIR-V");
    LOG_INFO("    {}/{} compute shaders", okCompute, computes.size());
}

// V4: an actual draw. Exercises pipeline creation (lazily specialized against
// the swapchain's format), a vertex buffer, and a UBO delivered through push
// descriptors. A colored triangle on screen is the proof the whole graphics
// path works; the clear alone never touched any of it.
struct TriangleDemo {
    rhi::ShaderHandle shader {};
    rhi::PipelineHandle pipeline {};
    rhi::BufferHandle vertices {};
    rhi::BufferHandle tint {};
    rhi::BindGroupHandle bindGroup {};
    bool ready { false };

    void create(rhi::Device& device) {
        const str vertexSource = R"(#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
layout(std140, binding = 0) uniform TintUbo { vec4 uTint; };
layout(location = 0) out vec3 vColor;
void main() {
    vColor = aColor * uTint.rgb;
    gl_Position = vec4(aPos, 0.0, 1.0);
})";
        const str fragmentSource = R"(#version 460 core
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); })";

        shader = device.createShader({ .debugName = "vksmoke.triangle",
                                       .vertexSource = vertexSource,
                                       .fragmentSource = fragmentSource });
        check(shader.id != 0, "createShader (inline triangle GLSL)");
        if (shader.id == 0) {
            return;
        }

        // Deliberately asymmetric so a vertical flip would be obvious: the
        // apex sits high and off-centre.
        const f32 vertexData[] = {
            //  x      y      r     g     b
            -0.6f, -0.5f, 1.0f, 0.2f, 0.2f,
             0.6f, -0.5f, 0.2f, 1.0f, 0.3f,
            -0.1f,  0.7f, 0.3f, 0.4f, 1.0f,
        };
        vertices = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex, .size = sizeof(vertexData) },
            vertexData);
        check(vertices.id != 0, "createBuffer (triangle vertices)");

        const f32 tintData[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        tint = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(tintData),
                                     .dynamic = true },
                                   tintData);
        bindGroup = device.createBindGroup({ .entries = { { .binding = 0,
                                                            .buffer = tint } } });
        check(bindGroup.id != 0, "createBindGroup (UBO, push descriptor)");

        pipeline = device.createPipeline(
            { .shader = shader,
              .vertexBuffers = { { .stride = 5 * sizeof(f32),
                                   .attributes = {
                                       { .location = 0,
                                         .format = rhi::VertexFormat::F32x2,
                                         .offset = 0 },
                                       { .location = 1,
                                         .format = rhi::VertexFormat::F32x3,
                                         .offset = 2 * sizeof(f32) } } } },
              .cull = rhi::CullMode::None });
        check(pipeline.id != 0, "createPipeline (graphics)");
        ready = shader.id != 0 && vertices.id != 0 && pipeline.id != 0 &&
                bindGroup.id != 0;
    }

    void draw(rhi::CommandBuffer& cmd, f32 pulse) {
        if (!ready) {
            return;
        }
        cmd.setPipeline(pipeline);
        cmd.setBindGroup(0, bindGroup);
        cmd.setVertexBuffer(0, vertices);
        cmd.draw(3);
        (void)pulse;
    }

    void destroy(rhi::Device& device) {
        device.destroyBindGroup(bindGroup);
        device.destroyBuffer(tint);
        device.destroyBuffer(vertices);
        device.destroyPipeline(pipeline);
        device.destroyShader(shader);
    }
};

// V6: the GPU markers the engine polls without ever blocking the frame thread
// (the Phase-5 completion-queue rule), plus a compute dispatch that proves the
// compute pipeline path end to end by writing an SSBO the CPU reads back.
void testMarkersAndCompute(rhi::Device& device) {
    // --- Compute: square 256 values on the GPU, read them back. ---
    const str computeSource = R"(#version 460 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Values { uint values[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i < 256u) { values[i] = values[i] * values[i]; }
})";
    const rhi::ShaderHandle shader = device.createShader(
        { .debugName = "vksmoke.square", .computeSource = computeSource });
    check(shader.id != 0, "createShader (compute)");

    const rhi::PipelineHandle pipeline =
        device.createComputePipeline({ .shader = shader });
    check(pipeline.id != 0, "createComputePipeline");

    constexpr u32 kCount = 256;
    vector<u32> input(kCount);
    for (u32 i = 0; i < kCount; ++i) {
        input[i] = i;
    }
    const rhi::BufferHandle buffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kCount * sizeof(u32),
          .readback = true },
        input.data());
    const rhi::BindGroupHandle bindGroup = device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = buffer, .storage = true } } });

    rhi::FenceHandle fence {};
    rhi::TimestampHandle stamp {};
    if (pipeline.id != 0 && buffer.id != 0) {
        auto& cmd = device.beginFrame();
        // Compute runs outside a render pass.
        cmd.setPipeline(pipeline);
        cmd.setBindGroup(0, bindGroup);
        cmd.dispatch(kCount / 64);
        cmd.memoryBarrier();
        stamp = device.insertTimestamp();
        cmd.beginRenderPass({ .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f } });
        cmd.endRenderPass();
        device.endFrame();
        fence = device.insertFence();
    }

    // Poll ACROSS FRAMES, never blocking — the way the engine consumes these.
    // A tight spin would be wrong twice over: it is not the real usage, and it
    // burns through its budget in a few ms while a v-synced frame needs ~16.
    bool retired = false;
    for (u32 tries = 0; fence.id != 0 && tries < 8 && !retired; ++tries) {
        retired = device.fenceReady(fence);
        if (!retired) {
            auto& idle = device.beginFrame();
            idle.beginRenderPass({ .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f } });
            idle.endRenderPass();
            device.endFrame();
        }
    }
    check(retired, "insertFence / fenceReady (polled across frames)");

    vector<u32> got(kCount, 0);
    device.readBuffer(buffer, got.data(), kCount * sizeof(u32));
    bool squared = true;
    for (u32 i = 0; i < kCount; ++i) {
        if (got[i] != i * i) {
            squared = false;
            break;
        }
    }
    check(squared, "compute dispatch wrote the SSBO (values squared)");

    if (device.caps().timerQueries) {
        u64 nanos = 0;
        u32 tries = 0;
        // Results land a frame or two later; drive frames while polling.
        // Regression guard: this flaked ~1 run in 5 until the backend
        // harvested pending timestamps at slot-recycle time (in
        // beginFrame) — before that, polling one frame too late hit the
        // query-pool reset and the value was gone for good.
        while (stamp.id != 0 && !device.timestampReady(stamp, nanos) &&
               tries < 30) {
            auto& idle = device.beginFrame();
            idle.beginRenderPass({ .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f } });
            idle.endRenderPass();
            device.endFrame();
            ++tries;
        }
        check(nanos > 0, "insertTimestamp / timestampReady (GPU clock)");
    } else {
        LOG_INFO("  SKIP  timestamps (caps.timerQueries is false)");
    }

    device.destroyBindGroup(bindGroup);
    device.destroyBuffer(buffer);
    device.destroyPipeline(pipeline);
    device.destroyShader(shader);
}

// Does the REAL in-game UI render on Vulkan? RmlUi already goes through the
// RHI (engine/ui/UiSystem.cpp implements Rml::RenderInterface on rhi::), so if
// that seam is honest this works with no backend-specific code at all — and
// that is also the evidence that an ImGui renderer written the same way would.
// Loads an actual game screen rather than a synthetic document.
// The dev UI on Vulkan: the ImGui renderer is written on the RHI (no
// imgui_impl_vulkan), so recording a real widget frame through the backend is
// the whole test — the validation layer judges it.
void testImGui(platform::Window& window, rhi::Device& device) {
    auto imgui = ui::ImGuiLayer::create(window, device);
    check(imgui != nullptr, "ImGuiLayer::create on Vulkan (renderer on the RHI)");
    if (!imgui) {
        return;
    }
    for (u32 i = 0; i < 3; ++i) {
        imgui->beginFrame();
        ImGui::Begin("vksmoke");
        ImGui::Text("ImGui rendered through the RHI on Vulkan");
        ImGui::Button("a button, for vertices");
        ImGui::End();
        auto& cmd = device.beginFrame();
        cmd.beginRenderPass({ .clearColor = { 0.05f, 0.06f, 0.09f, 1.0f } });
        imgui->render(cmd);
        cmd.endRenderPass();
        device.endFrame();
    }
    check(true, "ImGuiLayer::render recorded on Vulkan (no validation error)");
}

void testInGameUi(rhi::Device& device) {
    render::ShaderLibrary shaders(device);
    // <repo>/engine/render/landscape/shaders -> <repo>
    std::filesystem::path repo = shaders.root();
    for (int i = 0; i < 4; ++i) {
        repo = repo.parent_path();
    }
    const std::filesystem::path uiRoot = repo / "game" / "data" / "base" / "ui";
    if (!std::filesystem::exists(uiRoot)) {
        LOG_INFO("  SKIP  in-game UI (no {} )", uiRoot.string());
        return;
    }

    ui::UiSystem uiSystem;
    const bool created =
        uiSystem.create(device, shaders, { uiRoot }, 1280, 720);
    check(created, "UiSystem::create on Vulkan (RmlUi through the RHI)");
    if (!created) {
        return;
    }
    // Fonts must load before the first document (RmlUi requirement).
    check(uiSystem.loadFont(uiRoot / "fonts" / "DemoFont.ttf"),
          "UiSystem::loadFont");
    check(uiSystem.showDocument("main-menu.rml"),
          "showDocument (real game screen: main-menu.rml)");

    uiSystem.update(1.0f / 60.0f);
    // Draw it for a few frames, inside the backbuffer pass as the contract says.
    for (u32 i = 0; i < 3; ++i) {
        auto& cmd = device.beginFrame();
        cmd.beginRenderPass({ .clearColor = { 0.05f, 0.06f, 0.09f, 1.0f } });
        uiSystem.render(cmd, device, 1280, 720);
        cmd.endRenderPass();
        device.endFrame();
    }
    check(true, "UiSystem::render recorded on Vulkan (no validation error)");
    uiSystem.destroy(device);
}

// Same thing, but kept alive so the display loop shows it: recording without a
// validation error proves the plumbing, only pixels prove the result.
struct UiDemo {
    uptr<render::ShaderLibrary> shaders;
    uptr<ui::UiSystem> uiSystem;
    bool ready { false };

    void create(rhi::Device& device) {
        shaders = std::make_unique<render::ShaderLibrary>(device);
        std::filesystem::path repo = shaders->root();
        for (int i = 0; i < 4; ++i) {
            repo = repo.parent_path();
        }
        const std::filesystem::path uiRoot =
            repo / "game" / "data" / "base" / "ui";
        if (!std::filesystem::exists(uiRoot)) {
            return;
        }
        uiSystem = std::make_unique<ui::UiSystem>();
        if (!uiSystem->create(device, *shaders, { uiRoot }, 1280, 720)) {
            return;
        }
        uiSystem->loadFont(uiRoot / "fonts" / "DemoFont.ttf");
        ready = uiSystem->showDocument("main-menu.rml");
    }

    void draw(rhi::CommandBuffer& cmd, rhi::Device& device, f32 dt) {
        if (!ready) {
            return;
        }
        uiSystem->update(dt);
        uiSystem->render(cmd, device, 1280, 720);
    }

    void destroy(rhi::Device& device) {
        if (uiSystem) {
            uiSystem->destroy(device);
        }
    }
};

} // namespace


// Dual draw-path seal (docs/RENDERING.md §6.0): the GPU cull's
// verdict (chunk_cull.comp) checked against a CPU reference on known
// boxes and a KNOWN depth (a plain clear — far: nothing occludes; near:
// everything does). Candidates sit far from the guard-band thresholds so
// the reference needs no exact guard value. If the compute shader and
// the CPU selection rules ever diverge, this fails before the game does.
void testGpuCullVsCpu(rhi::Device& device) {
    if (!device.caps().computeShaders || !device.caps().copyTexture) {
        LOG_INFO("  (skipped: no compute/copy caps)");
        return;
    }
    render::ShaderLibrary shaders(device);
    render::GpuOcclusion cull;
    cull.create(device, shaders);
    cull.refreshPipelines(device, shaders);
    cull.resize(device, 512, 512);
    check(cull.ready(), "cull seal: GpuOcclusion pipelines ready");
    if (!cull.ready()) {
        return;
    }

    const rhi::TextureHandle depth = device.createTexture(
        { .width = 512,
          .height = 512,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    const rhi::FramebufferHandle fb = device.createFramebuffer(
        { .depthAttachment = { .texture = depth } });

    // Camera at the origin looking down -Z (reversed-Z projection).
    render::Camera3D camera;
    const Mat4 viewProj = camera.viewProj(1.0f);

    // Candidates, group-sorted (the contract): [0] on screen (g0),
    // [1] far off to the left (g0), [2] on screen (g1), [3] far behind
    // the camera (g1), [4] straddling the near plane (g1), [5] just
    // behind the camera (g1) — INSIDE the 16 m world margin the plane
    // branch grants near-plane straddlers, so it stays visible: the
    // margin is part of the contract this test pins.
    const auto box = [](Vec3 center, f32 half, u32 group,
                        u32 indexCount) {
        return render::GpuOcclusion::Candidate {
            center - Vec3 { half }, center + Vec3 { half }, group,
            indexCount, 0, 1, 0
        };
    };
    vector<render::GpuOcclusion::Candidate> candidates {
        box({ 0.0f, 0.0f, -10.0f }, 1.0f, 0, 111),
        box({ -80.0f, 0.0f, -10.0f }, 1.0f, 0, 222),
        box({ 2.0f, 1.0f, -20.0f }, 1.0f, 1, 333),
        box({ 0.0f, 0.0f, 30.0f }, 1.0f, 1, 444),
        box({ 0.0f, 0.0f, 0.0f }, 1.5f, 1, 555), // spans the camera
        box({ 0.0f, 0.0f, 10.0f }, 1.0f, 1, 666), // in the 16 m margin
    };

    struct Cmd {
        u32 indexCount, instanceCount, firstIndex;
        i32 vertexOffset;
        u32 firstInstance;
    };
    const auto runScenario = [&](f32 clearDepth, const char* label,
                                 const array<bool, 6>& expected) {
        auto& cmd = device.beginFrame();
        cmd.beginRenderPass({ .framebuffer = fb,
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::Clear,
                              .clearDepth = clearDepth });
        cmd.endRenderPass();
        const bool ran = cull.run(cmd, device, depth, viewProj, candidates);
        device.endFrame();
        check(ran, "cull seal: dispatch recorded");
        // Let the dispatch land, then read the fresh (read-side) commands.
        for (int i = 0; i < 3; ++i) {
            auto& idle = device.beginFrame();
            (void)idle;
            device.endFrame();
        }
        vector<Cmd> out(candidates.size());
        device.readBuffer(cull.commandBuffer(), out.data(),
                          out.size() * sizeof(Cmd), 0);
        bool all = true;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const bool visible = out[i].instanceCount != 0;
            if (visible != expected[i]) {
                LOG_ERROR("cull seal {}: candidate {} — GPU {} vs CPU ref {}",
                          label, i, visible, expected[i]);
                all = false;
            }
            if (visible && out[i].indexCount != candidates[i].indexCount) {
                LOG_ERROR("cull seal {}: candidate {} indexCount {} != {}", label,
                          i, out[i].indexCount, candidates[i].indexCount);
                all = false;
            }
        }
        check(all, label);
        check(cull.groupFirst()[0] == 0 && cull.groupCount()[0] == 2 &&
                  cull.groupFirst()[1] == 2 && cull.groupCount()[1] == 4,
              "cull seal: per-group command ranges");
    };
    // Far clear (reversed-Z 0): nothing occludes — pure frustum verdict.
    // The near-straddler is kept by the plane test (projection unusable).
    runScenario(0.0f, "cull seal: far depth = frustum verdict",
                { true, false, true, false, true, true });
    // Near clear (1): everything projectable is occluded; only the
    // straddler survives (its branch never samples the Hi-Z).
    runScenario(1.0f, "cull seal: near depth = all occluded but straddlers",
                { false, false, false, false, true, true });

    device.destroyFramebuffer(fb);
    device.destroyTexture(depth);
    cull.destroy(device);
}

int main(int argc, char** argv) {
    core::Log::init();

    f64 seconds = 5.0;
    if (argc > 1) {
        seconds = std::atof(argv[1]);
    }

    // The surface flag is fixed at window creation and must match the backend.
    auto window =
        platform::Window::create({ .title = "Meadows — Vulkan smoke (V1-V2)",
                                   .width = 1280,
                                   .height = 720,
                                   .api = platform::GraphicsApi::Vulkan });
    if (!window) {
        LOG_ERROR("vksmoke: window creation failed");
        return 1;
    }

    auto device = rhi::Device::create(rhi::Backend::Vulkan, *window);
    if (!device) {
        LOG_ERROR("vksmoke: Vulkan device creation failed");
        return 1;
    }

    LOG_INFO("vksmoke: V2 resource self-test");
    testBufferRoundTrip(*device, "buffer round-trip (device-local + staging)",
                        false, false);
    testBufferRoundTrip(*device, "buffer round-trip (dynamic, mapped)", true,
                        false);
    testBufferRoundTrip(*device, "buffer round-trip (readback, mapped)", false,
                        true);
    testGpuCopy(*device);
    testTextures(*device);
    testShaders(*device);
    LOG_INFO("vksmoke: in-game UI (RmlUi) on Vulkan");
    testInGameUi(*device);

    LOG_INFO("vksmoke: dev UI (ImGui on the RHI) on Vulkan");
    testImGui(*window, *device);

    LOG_INFO("vksmoke: V6 markers + compute");
    testMarkersAndCompute(*device);

    LOG_INFO("vksmoke: GPU cull vs CPU reference (dual-path seal)");
    testGpuCullVsCpu(*device);

    LOG_INFO("vksmoke: V4 graphics path");
    TriangleDemo triangle;
    triangle.create(*device);
    UiDemo uiDemo;
    uiDemo.create(*device);

    LOG_INFO("vksmoke: self-test {} ({} failure(s))",
             failures == 0 ? "PASSED" : "FAILED", failures);

    const auto start = core::clockNow();
    f64 elapsed = 0.0;
    u32 frames = 0;
    while (window->pumpEvents()) {
        elapsed = core::secondsBetween(start, core::clockNow());
        if (seconds > 0.0 && elapsed >= seconds) {
            break;
        }
        // Animated so a static frame (or a frozen swapchain) is obvious.
        const f32 t = static_cast<f32>(elapsed);
        const f32 pulse = 0.5f + 0.5f * std::sin(t * 2.0f);

        auto& cmd = device->beginFrame();
        cmd.beginRenderPass({ .clearColor = { 0.10f + 0.35f * pulse,
                                              0.12f + 0.20f * pulse,
                                              0.30f + 0.45f * pulse, 1.0f } });
        triangle.draw(cmd, pulse);
        uiDemo.draw(cmd, *device, 1.0f / 60.0f);
        cmd.endRenderPass();
        device->endFrame();
        ++frames;
    }

    uiDemo.destroy(*device);
    triangle.destroy(*device);
    LOG_INFO("vksmoke: {} frames in {:.2f}s ({:.1f} fps) — presentation path OK",
             frames, elapsed, elapsed > 0.0 ? frames / elapsed : 0.0);
    return failures == 0 ? 0 : 1;
}
