#include "engine/ui/ImGuiLayer.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

#include <glm/glm.hpp>

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

// Dev-UI renderer written on the RHI, mirroring ui::RhiRenderInterface (the
// RmlUi adapter). One path serves every backend, so there is no
// imgui_impl_opengl3 / imgui_impl_vulkan pair to keep in sync and no native
// handle escapes the RHI (§3.1). Only the PLATFORM half (imgui_impl_sdl3)
// remains stock — it is genuinely SDL's business, and platform selection is
// compile-time anyway (§3.1).
//
// ImTextureID is an rhi::TextureHandle id (NOT a native GL name): callers
// pass the handle straight through, which is what makes ImGui::Image work
// identically on GL and Vulkan.

namespace ui {

namespace {

// ImGui ships colors as packed RGBA8 and the RHI has no normalized-byte
// vertex format, so colors are widened to float here — the same conversion
// RhiRenderInterface does for RmlUi.
struct ImGuiVertex {
    Vec2 position;
    Vec2 uv;
    Vec4 color;
};

// std140 mirror of the ImGuiPush block below.
struct ImGuiUniforms {
    Vec4 scaleTranslate; // xy = scale, zw = translate (clip space)
};

const char* kVertexShader = R"glsl(
#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
#ifdef VULKAN
layout(push_constant) uniform ImGuiPush {
#else
layout(std140, binding = 15) uniform ImGuiPush {
#endif
    vec4 uScaleTranslate;
};
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
void main() {
    vUv = aUv;
    vColor = aColor;
    gl_Position = vec4(aPos * uScaleTranslate.xy + uScaleTranslate.zw,
                       0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 460 core
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(binding = 0) uniform sampler2D uTexture;
layout(location = 0) out vec4 oColor;
void main() {
    oColor = vColor * texture(uTexture, vUv);
}
)glsl";

} // namespace

// Per-frame scratch, reused so the flatten below allocates nothing steady-state.
struct ImGuiLayer::Scratch {
    vector<ImGuiVertex> vertices;
    vector<ImDrawIdx> indices;
    struct ListBase {
        u32 vertex { 0 };
        u32 index { 0 };
    };
    vector<ListBase> listBases;
};

ImGuiLayer::ImGuiLayer(platform::Window& window, rhi::Device& device)
    : window { window }, device { device },
      scratch { std::make_unique<Scratch>() } {
}

ImGuiLayer::~ImGuiLayer() {
    window.setEventHook(nullptr);
    for (auto& [id, group] : textureGroups) {
        device.destroyBindGroup(group);
    }
    device.destroyTexture(fontTexture);
    device.destroySampler(sampler);
    device.destroyPipeline(pipeline);
    device.destroyShader(shader);
    device.destroyBuffer(vertexBuffer);
    device.destroyBuffer(indexBuffer);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

uptr<ImGuiLayer> ImGuiLayer::create(platform::Window& window,
                                    rhi::Device& device) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Docking (chantier 8.7b): the DB editor is a dockspace. No
    // multi-viewport — one OS window.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    auto* sdlWindow = static_cast<SDL_Window*>(window.nativeHandle());
    // InitForOther: the renderer is ours, so ImGui's SDL backend must not
    // assume a GL context (there is none on Vulkan).
    if (!ImGui_ImplSDL3_InitForOther(sdlWindow)) {
        LOG_ERROR("ImGui SDL3 platform backend initialization failed");
        ImGui::DestroyContext();
        return nullptr;
    }

    uptr<ImGuiLayer> layer { new ImGuiLayer(window, device) };
    if (!layer->createDeviceObjects()) {
        // Degrade, don't fail: the game must start even where the dev-UI
        // shader cannot compile (GL 4.1 / macOS lacks GLSL 420 explicit
        // bindings). Frames stay legal — beginFrame/render run, render just
        // records nothing. On macOS the dev UI runs on the Vulkan backend;
        // GL 4.1 is the low-spec fallback and keeps only the game itself.
        LOG_WARN("ImGui: dev UI unavailable on this backend (needs GLSL "
                 "420+); running without dev panels");
    }
    // The Engine installs the window event hook (fan-out to ImGui + Input).
    return layer;
}

bool ImGuiLayer::createDeviceObjects() {
    // Font atlas FIRST and unconditionally: ImGui::NewFrame asserts on an
    // unbuilt atlas, and the degraded path below must keep frames legal
    // (beginFrame/render still run, they just draw nothing).
    u8* pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    fontTexture = device.createTexture({ .width = static_cast<u32>(width),
                                         .height = static_cast<u32>(height),
                                         .format = rhi::TextureFormat::RGBA8 },
                                       pixels);
    ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(fontTexture.id));
    sampler = device.createSampler({});

    shader = device.createShader({ .vertexSource = kVertexShader,
                                   .fragmentSource = kFragmentShader,
                                   .debugName = "imgui" });
    if (shader.id == 0) {
        // Expected on GL 4.1 (macOS): the shader needs GLSL 420+ for its
        // explicit uniform/sampler bindings. Not an error — see create().
        return false;
    }
    pipeline = device.createPipeline(
        { .shader = shader,
          .vertexBuffers =
              { { .stride = sizeof(ImGuiVertex),
                  .attributes =
                      { { .location = 0,
                          .format = rhi::VertexFormat::F32x2,
                          .offset = static_cast<u32>(offsetof(ImGuiVertex, position)) },
                        { .location = 1,
                          .format = rhi::VertexFormat::F32x2,
                          .offset = static_cast<u32>(offsetof(ImGuiVertex, uv)) },
                        { .location = 2,
                          .format = rhi::VertexFormat::F32x4,
                          .offset = static_cast<u32>(offsetof(ImGuiVertex, color)) } } } },
          // ImGui colors are straight (non-premultiplied) alpha.
          .blend = rhi::BlendMode::Alpha,
          .pushConstantSize = sizeof(ImGuiUniforms) });
    if (pipeline.id == 0) {
        LOG_ERROR("ImGui: pipeline creation failed");
        return false;
    }
    return true;
}

rhi::BindGroupHandle ImGuiLayer::groupFor(u32 textureId) {
    if (auto it = textureGroups.find(textureId); it != textureGroups.end()) {
        return it->second;
    }
    const rhi::BindGroupHandle group = device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = rhi::TextureHandle { textureId },
                         .sampler = sampler } } });
    textureGroups.emplace(textureId, group);
    return group;
}

void ImGuiLayer::processEvent(const void* nativeEvent) {
    ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(nativeEvent));
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render(rhi::CommandBuffer& cmd) {
    // ImGui::Render() must run even in degraded mode: it closes the frame
    // NewFrame opened (skipping it trips ImGui's frame-scope asserts).
    ImGui::Render();
    const ImDrawData* drawData = ImGui::GetDrawData();
    if (pipeline.id == 0 || drawData == nullptr ||
        drawData->TotalVtxCount == 0) {
        return;
    }

    // Flatten every draw list into ONE vertex and ONE index buffer, uploaded
    // once before any draw is recorded. Per-command slices are then selected
    // with a vertex-buffer offset and a first index — never by rewriting the
    // buffers between draws (that reads back the last write on Vulkan).
    vector<ImGuiVertex>& vertices = scratch->vertices;
    vector<ImDrawIdx>& indices = scratch->indices;
    vector<Scratch::ListBase>& listBases = scratch->listBases;
    vertices.clear();
    indices.clear();
    vertices.reserve(static_cast<size_t>(drawData->TotalVtxCount));
    indices.reserve(static_cast<size_t>(drawData->TotalIdxCount));
    listBases.clear();
    for (int i = 0; i < drawData->CmdListsCount; ++i) {
        const ImDrawList* list = drawData->CmdLists[i];
        listBases.push_back(Scratch::ListBase {
            static_cast<u32>(vertices.size()),
            static_cast<u32>(indices.size()) });
        for (const ImDrawVert& v : list->VtxBuffer) {
            const ImVec4 c = ImGui::ColorConvertU32ToFloat4(v.col);
            vertices.push_back({ { v.pos.x, v.pos.y },
                                 { v.uv.x, v.uv.y },
                                 { c.x, c.y, c.z, c.w } });
        }
        for (const ImDrawIdx idx : list->IdxBuffer) {
            indices.push_back(idx);
        }
    }

    growBuffers(vertices.size(), indices.size());
    device.updateBuffer(vertexBuffer, vertices.data(),
                        vertices.size() * sizeof(ImGuiVertex));
    device.updateBuffer(indexBuffer, indices.data(),
                        indices.size() * sizeof(ImDrawIdx));

    cmd.setPipeline(pipeline);
    cmd.setIndexBuffer(indexBuffer, rhi::IndexFormat::U16);
    // Pixel coordinates -> clip space. Constant for the frame, but it must be
    // pushed after setPipeline.
    const f32 left = drawData->DisplayPos.x;
    const f32 top = drawData->DisplayPos.y;
    const f32 right = left + drawData->DisplaySize.x;
    const f32 bottom = top + drawData->DisplaySize.y;
    const ImGuiUniforms uniforms { { 2.0f / (right - left),
                                     2.0f / (top - bottom),
                                     (right + left) / (left - right),
                                     (top + bottom) / (bottom - top) } };
    cmd.setPushConstants(&uniforms, sizeof(uniforms));

    const f32 height = drawData->DisplaySize.y;
    for (int i = 0; i < drawData->CmdListsCount; ++i) {
        const ImDrawList* list = drawData->CmdLists[i];
        const Scratch::ListBase base = listBases[static_cast<size_t>(i)];
        for (const ImDrawCmd& drawCmd : list->CmdBuffer) {
            if (drawCmd.UserCallback != nullptr) {
                drawCmd.UserCallback(list, &drawCmd);
                continue;
            }
            // ImGui clips top-left; the RHI scissor is bottom-left like the
            // viewport (the backend re-flips for Vulkan).
            const f32 clipX = drawCmd.ClipRect.x - left;
            const f32 clipY = drawCmd.ClipRect.y - top;
            const f32 clipW = drawCmd.ClipRect.z - drawCmd.ClipRect.x;
            const f32 clipH = drawCmd.ClipRect.w - drawCmd.ClipRect.y;
            if (clipW <= 0.0f || clipH <= 0.0f) {
                continue;
            }
            cmd.setScissor(static_cast<u32>(std::max(clipX, 0.0f)),
                           static_cast<u32>(std::max(
                               height - (clipY + clipH), 0.0f)),
                           static_cast<u32>(clipW),
                           static_cast<u32>(clipH));
            cmd.setBindGroup(
                0, groupFor(static_cast<u32>(drawCmd.GetTexID())));
            cmd.setVertexBuffer(
                0, vertexBuffer,
                static_cast<u64>(base.vertex + drawCmd.VtxOffset) *
                    sizeof(ImGuiVertex));
            cmd.drawIndexed(drawCmd.ElemCount, 1,
                            base.index + drawCmd.IdxOffset, 0);
        }
    }
    cmd.clearScissor();
}

void ImGuiLayer::growBuffers(size_t vertexCount, size_t indexCount) {
    if (vertexCount > vertexCapacity || vertexBuffer.id == 0) {
        device.destroyBuffer(vertexBuffer);
        vertexCapacity = vertexCount + vertexCount / 2 + 4096;
        vertexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = vertexCapacity * sizeof(ImGuiVertex),
              .dynamic = true },
            nullptr);
    }
    if (indexCount > indexCapacity || indexBuffer.id == 0) {
        device.destroyBuffer(indexBuffer);
        indexCapacity = indexCount + indexCount / 2 + 8192;
        indexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = indexCapacity * sizeof(ImDrawIdx),
              .dynamic = true },
            nullptr);
    }
}

} // namespace ui
