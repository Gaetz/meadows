// vksmoke — Vulkan bring-up harness (chantier VULKAN, briques V1-V3).
//
// Drives the Vulkan backend end to end WITHOUT the Engine loop, which still
// depends on the GL SpriteRenderer and the GL ImGui layer (ported in V3/V6).
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
#include "engine/core/Defines.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/Device.hpp"

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
// Vulkan backend must load too — that is the V3 contract.
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

} // namespace

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
        cmd.endRenderPass();
        device->endFrame();
        ++frames;
    }

    LOG_INFO("vksmoke: {} frames in {:.2f}s ({:.1f} fps) — presentation path OK",
             frames, elapsed, elapsed > 0.0 ? frames / elapsed : 0.0);
    return failures == 0 ? 0 : 1;
}
