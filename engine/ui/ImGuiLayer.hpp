#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace platform {
class Window;
}

namespace rhi {
class Device;
class CommandBuffer;
}

namespace ui {

// Dear ImGui integration for the dev UI. The RENDERER is written on the RHI
// (see ImGuiLayer.cpp), so one path serves every backend — no
// imgui_impl_opengl3 / imgui_impl_vulkan pair, no native handle escaping the
// abstraction. Only the platform half (imgui_impl_sdl3) is stock.
//
// ImTextureID is an rhi::TextureHandle id, so ImGui::Image works the same on
// every backend (pass the handle's id, not a native GL name).
class ImGuiLayer {
public:
    // Create the RHI device first: the layer builds its pipeline and font
    // atlas on it. Returns nullptr (with a logged error) on failure.
    static uptr<ImGuiLayer> create(platform::Window& window,
                                   rhi::Device& device);
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Starts an ImGui frame; emit widgets between beginFrame and render.
    void beginFrame();
    // Records the accumulated UI into `cmd`, inside the current render pass
    // (the Engine calls this after the scene, targeting the backbuffer).
    void render(rhi::CommandBuffer& cmd);

    // Feeds one raw platform event to the ImGui backend. The Engine owns
    // the window event hook (it fans out to ImGui AND platform::Input) and
    // calls this from it.
    static void processEvent(const void* nativeEvent);

private:
    ImGuiLayer(platform::Window& window, rhi::Device& device);
    bool createDeviceObjects();
    void growBuffers(size_t vertexCount, size_t indexCount);
    rhi::BindGroupHandle groupFor(u32 textureId);

    platform::Window& window;
    rhi::Device& device;

    rhi::ShaderHandle shader {};
    rhi::PipelineHandle pipeline {};
    rhi::SamplerHandle sampler {};
    rhi::TextureHandle fontTexture {};
    rhi::BufferHandle vertexBuffer {};
    rhi::BufferHandle indexBuffer {};
    size_t vertexCapacity { 0 };
    size_t indexCapacity { 0 };
    std::unordered_map<u32, rhi::BindGroupHandle> textureGroups;

    // Per-frame scratch (flattened vertices/indices/list bases). Its element
    // types are renderer-private, so it stays behind a pimpl (§3.1) and is
    // reused across frames rather than reallocated.
    struct Scratch;
    uptr<Scratch> scratch;
};

} // namespace ui
