#include "engine/ui/UiSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>

#include <glm/glm.hpp>

#include <RmlUi/Core.h>

#include "engine/assets/Image.hpp"
#include "engine/core/Assert.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

// RmlUi adapters over the RHI (H4). Design notes for the verticals:
//  - Geometry is COMPILED (static VB/IB per handle): RmlUi compiles once
//    and re-renders many frames — a perfect fit for retained buffers.
//  - Colors arrive premultiplied; the pipeline blends ONE/1-SRC_ALPHA
//    (BlendMode::PremultipliedAlpha added to the RHI for this).
//  - The advanced 6.x features (clip masks, layers, filters) keep their
//    default no-op implementations for now: box-shadow/filter effects
//    will render as nothing until that vertical lands.
//  - RmlUi's global interfaces mean ONE UiSystem instance per process
//    (asserted); fine for a game.

namespace ui {

namespace {

struct UiVertex {
    Vec2 position;
    Vec4 color; // premultiplied
    Vec2 uv;
};

constexpr const char* kUiShader = "ui";

// std140 mirror of the UiUbo block in ui.vert.
struct UiUniforms {
    Vec4 transform; // xy = translation (px), zw = viewport size (px)
};

u32 gInstances = 0;

} // namespace

// --- Render interface ---------------------------------------------------------

class RhiRenderInterface final : public Rml::RenderInterface {
public:
    rhi::Device* device { nullptr };
    rhi::CommandBuffer* cmd { nullptr }; // valid during render() only
    render::ShaderLibrary* shaders { nullptr };
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
    rhi::BufferHandle ubo {};
    rhi::SamplerHandle sampler {};
    rhi::TextureHandle whiteTexture {};
    rhi::BindGroupHandle whiteGroup {};
    u32 viewportWidth { 0 };
    u32 viewportHeight { 0 };
    vector<std::filesystem::path> roots;

    struct Geometry {
        rhi::BufferHandle vertices {};
        rhi::BufferHandle indices {};
        u32 indexCount { 0 };
    };
    std::unordered_map<uintptr_t, Geometry> geometries;
    struct Texture {
        rhi::TextureHandle texture {};
        rhi::BindGroupHandle group {};
    };
    std::unordered_map<uintptr_t, Texture> textures;
    uintptr_t nextHandle { 1 };

    void createPipeline() {
        if (pipeline.id != 0) {
            device->destroyPipeline(pipeline);
        }
        pipeline = device->createPipeline(
            { .shader = shaders->get(kUiShader),
              .vertexBuffers =
                  { { .stride = sizeof(UiVertex),
                      .attributes =
                          { { .location = 0,
                              .format = rhi::VertexFormat::F32x2,
                              .offset = offsetof(UiVertex, position) },
                            { .location = 1,
                              .format = rhi::VertexFormat::F32x4,
                              .offset = offsetof(UiVertex, color) },
                            { .location = 2,
                              .format = rhi::VertexFormat::F32x2,
                              .offset = offsetof(UiVertex, uv) } } } },
              .blend = rhi::BlendMode::PremultipliedAlpha });
        shaderGeneration = shaders->generation(kUiShader);
    }

    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex> vertices,
        Rml::Span<const int> indices) override {
        vector<UiVertex> converted;
        converted.reserve(vertices.size());
        for (const Rml::Vertex& v : vertices) {
            converted.push_back(
                { { v.position.x, v.position.y },
                  { v.colour.red / 255.0f, v.colour.green / 255.0f,
                    v.colour.blue / 255.0f, v.colour.alpha / 255.0f },
                  { v.tex_coord.x, v.tex_coord.y } });
        }
        vector<u32> converted_indices { indices.begin(), indices.end() };
        Geometry geometry;
        geometry.indexCount = static_cast<u32>(converted_indices.size());
        geometry.vertices = device->createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = converted.size() * sizeof(UiVertex) },
            converted.data());
        geometry.indices = device->createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = converted_indices.size() * sizeof(u32) },
            converted_indices.data());
        const uintptr_t handle = nextHandle++;
        geometries.emplace(handle, geometry);
        return handle;
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        const auto it = geometries.find(handle);
        if (it == geometries.end() || !cmd) {
            return;
        }
        const UiUniforms uniforms { { translation.x, translation.y,
                                      static_cast<f32>(viewportWidth),
                                      static_cast<f32>(viewportHeight) } };
        device->updateBuffer(ubo, &uniforms, sizeof(uniforms), 0);
        const auto texIt = textures.find(texture);
        cmd->setPipeline(pipeline);
        cmd->setBindGroup(0, texIt != textures.end() ? texIt->second.group
                                                     : whiteGroup);
        cmd->setVertexBuffer(0, it->second.vertices);
        cmd->setIndexBuffer(it->second.indices, rhi::IndexFormat::U32);
        cmd->drawIndexed(it->second.indexCount);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        const auto it = geometries.find(handle);
        if (it == geometries.end()) {
            return;
        }
        device->destroyBuffer(it->second.vertices);
        device->destroyBuffer(it->second.indices);
        geometries.erase(it);
    }

    Rml::TextureHandle registerTexture(rhi::TextureHandle texture) {
        Texture entry;
        entry.texture = texture;
        entry.group = device->createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = texture,
                             .sampler = sampler } } });
        const uintptr_t handle = nextHandle++;
        textures.emplace(handle, entry);
        return handle;
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions,
                                   const Rml::String& source) override {
        // Resolve through the roots, last root first (mod override).
        std::filesystem::path resolved;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            const std::filesystem::path candidate = *it / source;
            if (std::filesystem::exists(candidate)) {
                resolved = candidate;
                break;
            }
        }
        if (resolved.empty()) {
            resolved = source; // absolute / working-dir path
        }
        const auto image = assets::loadImageFile(resolved);
        if (!image) {
            return 0;
        }
        dimensions = { static_cast<int>(image->width),
                       static_cast<int>(image->height) };
        const rhi::TextureHandle texture = device->createTexture(
            { .width = image->width,
              .height = image->height,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear },
            image->pixels.data());
        return registerTexture(texture);
    }

    Rml::TextureHandle GenerateTexture(
        Rml::Span<const Rml::byte> source,
        Rml::Vector2i dimensions) override {
        const rhi::TextureHandle texture = device->createTexture(
            { .width = static_cast<u32>(dimensions.x),
              .height = static_cast<u32>(dimensions.y),
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear },
            source.data());
        return registerTexture(texture);
    }

    void ReleaseTexture(Rml::TextureHandle handle) override {
        const auto it = textures.find(handle);
        if (it == textures.end()) {
            return;
        }
        device->destroyBindGroup(it->second.group);
        device->destroyTexture(it->second.texture);
        textures.erase(it);
    }

    void EnableScissorRegion(bool enable) override {
        if (cmd && !enable) {
            cmd->clearScissor();
        }
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        if (!cmd) {
            return;
        }
        // RmlUi is top-left origin; GL scissor is bottom-left.
        const i32 y = static_cast<i32>(viewportHeight) -
                      (region.Top() + region.Height());
        cmd->setScissor(static_cast<u32>(std::max(region.Left(), 0)),
                        static_cast<u32>(std::max(y, 0)),
                        static_cast<u32>(region.Width()),
                        static_cast<u32>(region.Height()));
    }

    void releaseAll() {
        for (auto& [handle, geometry] : geometries) {
            device->destroyBuffer(geometry.vertices);
            device->destroyBuffer(geometry.indices);
        }
        geometries.clear();
        for (auto& [handle, texture] : textures) {
            device->destroyBindGroup(texture.group);
            device->destroyTexture(texture.texture);
        }
        textures.clear();
    }
};

// --- System interface ---------------------------------------------------------

class RhiSystemInterface final : public Rml::SystemInterface {
public:
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();

    double GetElapsedTime() override {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message)
        override {
        if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) {
            LOG_ERROR("RmlUi: {}", message);
        } else if (type == Rml::Log::LT_WARNING) {
            LOG_WARN("RmlUi: {}", message);
        } else {
            LOG_INFO("RmlUi: {}", message);
        }
        return true;
    }
};

// --- File interface (plugin path overlay) --------------------------------------

class RootsFileInterface final : public Rml::FileInterface {
public:
    vector<std::filesystem::path> roots;

    Rml::FileHandle Open(const Rml::String& path) override {
        // Last root wins: a mod's ui/ dir overrides the base document.
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            const std::filesystem::path candidate = *it / path;
            if (FILE* file = std::fopen(candidate.string().c_str(), "rb")) {
                return reinterpret_cast<Rml::FileHandle>(file);
            }
        }
        if (FILE* file = std::fopen(path.c_str(), "rb")) {
            return reinterpret_cast<Rml::FileHandle>(file);
        }
        return 0;
    }
    void Close(Rml::FileHandle file) override {
        std::fclose(reinterpret_cast<FILE*>(file));
    }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return std::fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return std::fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
    }
    size_t Tell(Rml::FileHandle file) override {
        return static_cast<size_t>(
            std::ftell(reinterpret_cast<FILE*>(file)));
    }
};

// --- UiSystem -------------------------------------------------------------------

struct UiSystem::Impl {
    RhiRenderInterface renderInterface;
    RhiSystemInterface systemInterface;
    RootsFileInterface fileInterface;
    Rml::Context* context { nullptr };
};

UiSystem::UiSystem() = default;
UiSystem::~UiSystem() = default;

bool UiSystem::create(rhi::Device& device, render::ShaderLibrary& shaders,
                      vector<std::filesystem::path> documentRoots,
                      u32 width, u32 height) {
    ENGINE_ASSERT_MSG(gInstances == 0,
                      "RmlUi interfaces are global: one UiSystem only");
    ++gInstances;
    pimpl = std::make_unique<Impl>();
    auto& impl = *pimpl;

    impl.renderInterface.device = &device;
    impl.renderInterface.shaders = &shaders;
    impl.renderInterface.roots = documentRoots;
    impl.renderInterface.viewportWidth = width;
    impl.renderInterface.viewportHeight = height;
    impl.fileInterface.roots = std::move(documentRoots);

    shaders.load(kUiShader, { { "UiUbo", 0 } }, { { "uTexture", 0 } });
    impl.renderInterface.createPipeline();
    impl.renderInterface.ubo = device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = sizeof(UiUniforms),
          .dynamic = true },
        nullptr);
    impl.renderInterface.sampler = device.createSampler({});
    const u32 white[1] = { 0xffffffffu };
    impl.renderInterface.whiteTexture = device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::RGBA8 },
        white);
    impl.renderInterface.whiteGroup = device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = impl.renderInterface.whiteTexture,
                         .sampler = impl.renderInterface.sampler } } });

    Rml::SetRenderInterface(&impl.renderInterface);
    Rml::SetSystemInterface(&impl.systemInterface);
    Rml::SetFileInterface(&impl.fileInterface);
    if (!Rml::Initialise()) {
        LOG_ERROR("RmlUi: Initialise failed");
        return false;
    }
    impl.context = Rml::CreateContext(
        "main", { static_cast<int>(width), static_cast<int>(height) });
    created = impl.context != nullptr;
    return created;
}

void UiSystem::destroy(rhi::Device& device) {
    if (!pimpl) {
        return;
    }
    Rml::Shutdown();
    pimpl->renderInterface.releaseAll();
    device.destroyBindGroup(pimpl->renderInterface.whiteGroup);
    device.destroyTexture(pimpl->renderInterface.whiteTexture);
    device.destroySampler(pimpl->renderInterface.sampler);
    device.destroyBuffer(pimpl->renderInterface.ubo);
    device.destroyPipeline(pimpl->renderInterface.pipeline);
    pimpl.reset();
    created = false;
    --gInstances;
}

bool UiSystem::loadFont(const std::filesystem::path& path) {
    return Rml::LoadFontFace(path.string());
}

bool UiSystem::showDocument(const str& path) {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    Rml::ElementDocument* document = pimpl->context->LoadDocument(path);
    if (!document) {
        return false;
    }
    document->Show();
    return true;
}

void UiSystem::closeDocuments() {
    if (pimpl && pimpl->context) {
        pimpl->context->UnloadAllDocuments();
    }
}

void UiSystem::resize(u32 width, u32 height) {
    if (pimpl && pimpl->context) {
        pimpl->renderInterface.viewportWidth = width;
        pimpl->renderInterface.viewportHeight = height;
        pimpl->context->SetDimensions(
            { static_cast<int>(width), static_cast<int>(height) });
    }
}

void UiSystem::update(f32) {
    if (pimpl && pimpl->context) {
        pimpl->context->Update();
    }
}

void UiSystem::render(rhi::CommandBuffer& cmd, rhi::Device& device,
                      u32 width, u32 height) {
    if (!pimpl || !pimpl->context) {
        return;
    }
    auto& ri = pimpl->renderInterface;
    if (ri.shaders->generation(kUiShader) != ri.shaderGeneration) {
        ri.createPipeline();
    }
    ri.cmd = &cmd;
    ri.device = &device;
    ri.viewportWidth = width;
    ri.viewportHeight = height;
    pimpl->context->Render();
    cmd.clearScissor();
    ri.cmd = nullptr;
}

void UiSystem::processMouseMove(i32 x, i32 y) {
    if (pimpl && pimpl->context) {
        pimpl->context->ProcessMouseMove(x, y, 0);
    }
}

void UiSystem::processMouseButton(i32 button, bool down) {
    if (pimpl && pimpl->context) {
        if (down) {
            pimpl->context->ProcessMouseButtonDown(button, 0);
        } else {
            pimpl->context->ProcessMouseButtonUp(button, 0);
        }
    }
}

void UiSystem::processMouseWheel(f32 delta) {
    if (pimpl && pimpl->context) {
        pimpl->context->ProcessMouseWheel(-delta, 0);
    }
}

} // namespace ui
