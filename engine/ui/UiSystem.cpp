#include "engine/ui/UiSystem.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>

#include <glm/glm.hpp>

#include <RmlUi/Core.h>

#include "engine/assets/Image.hpp"
#include "engine/core/Assert.hpp"
#include "engine/core/Clock.hpp"
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
        // The UiUbo rides in EVERY group: buffer binding 0 (UBO index)
        // and texture binding 0 (texture unit) are distinct GL namespaces.
        // Without it the vertex shader reads an unbound block — invisible
        // UI the moment another pass bound its own UBO at index 0.
        entry.group = device->createBindGroup(
            { .entries = { { .binding = 0, .buffer = ubo },
                           { .binding = 0,
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
    core::TimePoint start = core::clockNow();

    double GetElapsedTime() override { return core::secondsSince(start); }

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

namespace {

Rml::Input::KeyIdentifier rmlKeyFor(platform::Key key) {
    using platform::Key;
    using namespace Rml::Input;
    switch (key) {
    case Key::W: return KI_W;
    case Key::A: return KI_A;
    case Key::S: return KI_S;
    case Key::D: return KI_D;
    case Key::E: return KI_E;
    case Key::F: return KI_F;
    case Key::Q: return KI_Q;
    case Key::I: return KI_I;
    case Key::T: return KI_T;
    case Key::J: return KI_J;
    case Key::Up: return KI_UP;
    case Key::Down: return KI_DOWN;
    case Key::Left: return KI_LEFT;
    case Key::Right: return KI_RIGHT;
    case Key::Space: return KI_SPACE;
    case Key::Enter: return KI_RETURN;
    case Key::Escape: return KI_ESCAPE;
    case Key::Tab: return KI_TAB;
    case Key::Backspace: return KI_BACK;
    case Key::Delete: return KI_DELETE;
    case Key::Home: return KI_HOME;
    case Key::End: return KI_END;
    case Key::PageUp: return KI_PRIOR;
    case Key::PageDown: return KI_NEXT;
    case Key::Shift: return KI_LSHIFT;
    case Key::Ctrl: return KI_LCONTROL;
    case Key::Num1: return KI_1;
    case Key::Num2: return KI_2;
    case Key::Num3: return KI_3;
    case Key::Num4: return KI_4;
    case Key::Num5: return KI_5;
    case Key::Count: break;
    }
    return KI_UNKNOWN;
}

} // namespace

struct UiSystem::Impl {
    RhiRenderInterface renderInterface;
    RhiSystemInterface systemInterface;
    RootsFileInterface fileInterface;
    Rml::Context* context { nullptr };

    // Documents kept by the path they were shown with (screen stack).
    std::unordered_map<str, Rml::ElementDocument*> documents;

    // C9.5: key -> text, provided by the scene (a lambda over its
    // TextTable — this lib never sees data/). Applied to data-loc
    // elements on document load and on relocalize().
    std::function<str(std::string_view)> localizer;

    // Data models: map nodes give the bound values stable addresses.
    struct ModelStore {
        std::map<str, double> numbers;
        std::map<str, Rml::String> strings;
        std::map<str, bool> bools;
        vector<UiRow> rows;
        Rml::DataModelHandle handle;
    };
    std::unordered_map<str, uptr<ModelStore>> models;
    UiModelEventHandler eventHandler;
    bool rowTypeRegistered { false };

    // Modifier state for Rml key events (tracked from processKey).
    bool shiftDown { false };
    bool ctrlDown { false };

    int rmlModifiers() const {
        return (shiftDown ? Rml::Input::KM_SHIFT : 0) |
               (ctrlDown ? Rml::Input::KM_CTRL : 0);
    }
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
                         .buffer = impl.renderInterface.ubo },
                       { .binding = 0,
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

namespace {

// C9.5: the data-loc pass. Every element carrying data-loc="key" gets its
// inner RML replaced by localizer(key) — the authored English text is the
// fallback the localizer overrides. A localized element is a LEAF (its
// content was just replaced): no recursion below it. RmlUi silently
// ignores unregistered data-* view types, so the attribute is free.
void localizeTree(Rml::Element* element,
                  const std::function<str(std::string_view)>& localizer) {
    const Rml::Variant* key = element->GetAttribute("data-loc");
    if (key) {
        element->SetInnerRML(Rml::String { localizer(
            key->Get<Rml::String>()) });
        return;
    }
    const int count = element->GetNumChildren();
    for (int i = 0; i < count; ++i) {
        localizeTree(element->GetChild(i), localizer);
    }
}

} // namespace

void UiSystem::setLocalizer(std::function<str(std::string_view)> localizer) {
    if (pimpl) {
        pimpl->localizer = std::move(localizer);
    }
}

void UiSystem::relocalize() {
    if (!pimpl || !pimpl->localizer) {
        return;
    }
    for (auto& [path, document] : pimpl->documents) {
        localizeTree(document, pimpl->localizer);
    }
}

bool UiSystem::showDocument(const str& path) {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    if (const auto it = pimpl->documents.find(path);
        it != pimpl->documents.end()) {
        it->second->Show();
        return true;
    }
    Rml::ElementDocument* document = pimpl->context->LoadDocument(path);
    if (!document) {
        return false;
    }
    if (pimpl->localizer) {
        localizeTree(document, pimpl->localizer); // C9.5, before first show
    }
    document->Show();
    pimpl->documents.emplace(path, document);
    return true;
}

void UiSystem::closeDocument(const str& path) {
    if (!pimpl || !pimpl->context) {
        return;
    }
    const auto it = pimpl->documents.find(path);
    if (it == pimpl->documents.end()) {
        return;
    }
    pimpl->context->UnloadDocument(it->second);
    pimpl->documents.erase(it);
}

void UiSystem::closeDocuments() {
    if (pimpl && pimpl->context) {
        pimpl->context->UnloadAllDocuments();
        pimpl->documents.clear();
    }
}

void UiSystem::resize(u32 width, u32 height) {
    if (pimpl && pimpl->context) {
        if (pimpl->renderInterface.viewportWidth == width &&
            pimpl->renderInterface.viewportHeight == height) {
            return; // callers may resize() every frame
        }
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

void UiSystem::processKey(platform::Key key, bool down) {
    if (!pimpl || !pimpl->context) {
        return;
    }
    if (key == platform::Key::Shift) {
        pimpl->shiftDown = down;
    } else if (key == platform::Key::Ctrl) {
        pimpl->ctrlDown = down;
    }
    const Rml::Input::KeyIdentifier rmlKey = rmlKeyFor(key);
    if (rmlKey == Rml::Input::KI_UNKNOWN) {
        return;
    }
    if (down) {
        pimpl->context->ProcessKeyDown(rmlKey, pimpl->rmlModifiers());
    } else {
        pimpl->context->ProcessKeyUp(rmlKey, pimpl->rmlModifiers());
    }
}

void UiSystem::processTextInput(const str& utf8) {
    if (pimpl && pimpl->context && !utf8.empty()) {
        pimpl->context->ProcessTextInput(Rml::String { utf8 });
    }
}

bool UiSystem::textFieldFocused() const {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    const Rml::Element* focus = pimpl->context->GetFocusElement();
    if (!focus) {
        return false;
    }
    const Rml::String& tag = focus->GetTagName();
    return tag == "input" || tag == "textarea";
}

// --- Gamepad / keyboard focus navigation (C9.3) -------------------------------

namespace {

// A navigation target the way RmlUi's own spatial search defines one
// (ElementDocument's CanFocusElement): visible + `tab-index: auto`.
bool isNavigable(Rml::Element* element) {
    return element->IsVisible() &&
           element->GetComputedValues().tab_index() ==
               Rml::Style::TabIndex::Auto;
}

// First navigable element in document order; with preferSelected, only
// one that also carries the "selected" class (item rows keep the pad on
// the picked row across a data-for rebuild).
Rml::Element* findFocusable(Rml::Element* element, bool preferSelected) {
    const int count = element->GetNumChildren();
    for (int i = 0; i < count; ++i) {
        Rml::Element* child = element->GetChild(i);
        if (!child->IsVisible()) {
            continue; // display:none subtree (data-if off, hidden doc)
        }
        if (isNavigable(child) &&
            (!preferSelected || child->IsClassSet("selected"))) {
            return child;
        }
        if (Rml::Element* inner = findFocusable(child, preferSelected)) {
            return inner;
        }
    }
    return nullptr;
}

} // namespace

bool UiSystem::focusFirst(const str& documentPath) {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    const auto it = pimpl->documents.find(documentPath);
    if (it == pimpl->documents.end()) {
        return false;
    }
    Rml::Element* target = findFocusable(it->second, true);
    if (!target) {
        target = findFocusable(it->second, false);
    }
    if (!target || !target->Focus(true)) {
        return false; // screen with nothing focusable (plain text)
    }
    target->ScrollIntoView(Rml::ScrollAlignment::Nearest);
    return true;
}

bool UiSystem::hasNavigableFocus() const {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    // Walk up like RmlUi's arrow handling does (GetNearestFocusable):
    // a click can land the focus on a row's inner column div.
    for (Rml::Element* e = pimpl->context->GetFocusElement(); e;
         e = e->GetParentNode()) {
        if (isNavigable(e)) {
            return true;
        }
    }
    return false;
}

bool UiSystem::activateFocused() {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    for (Rml::Element* e = pimpl->context->GetFocusElement(); e;
         e = e->GetParentNode()) {
        if (isNavigable(e)) {
            e->Click(); // the same click event a mouse press dispatches
            return true;
        }
    }
    return false;
}

// --- Data models ------------------------------------------------------------

bool UiSystem::createModel(const UiModelDesc& desc) {
    if (!pimpl || !pimpl->context) {
        return false;
    }
    if (pimpl->models.contains(desc.name)) {
        return true; // idempotent (scene re-enter)
    }
    Rml::DataModelConstructor constructor =
        pimpl->context->CreateDataModel(desc.name);
    if (!constructor) {
        LOG_ERROR("RmlUi: data model '{}' creation failed", desc.name);
        return false;
    }
    auto store = std::make_unique<Impl::ModelStore>();
    for (const str& name : desc.numbers) {
        constructor.Bind(name, &store->numbers[name]);
    }
    for (const str& name : desc.strings) {
        constructor.Bind(name, &store->strings[name]);
    }
    for (const str& name : desc.bools) {
        constructor.Bind(name, &store->bools[name]);
    }
    if (desc.rows) {
        // The row struct/array types live in the context-wide register:
        // register them once, the first model that asks.
        if (!pimpl->rowTypeRegistered) {
            if (auto rowHandle = constructor.RegisterStruct<UiRow>()) {
                rowHandle.RegisterMember("id", &UiRow::id);
                rowHandle.RegisterMember("c0", &UiRow::c0);
                rowHandle.RegisterMember("c1", &UiRow::c1);
                rowHandle.RegisterMember("c2", &UiRow::c2);
                rowHandle.RegisterMember("c3", &UiRow::c3);
                rowHandle.RegisterMember("c4", &UiRow::c4);
                rowHandle.RegisterMember("selected", &UiRow::selected);
                rowHandle.RegisterMember("tag", &UiRow::tag);
            }
            constructor.RegisterArray<vector<UiRow>>();
            pimpl->rowTypeRegistered = true;
        }
        constructor.Bind("rows", &store->rows);
    }
    for (const str& event : desc.events) {
        const str modelName = desc.name;
        Impl* impl = pimpl.get();
        constructor.BindEventCallback(
            event,
            [impl, modelName, event](Rml::DataModelHandle, Rml::Event&,
                                     const Rml::VariantList& variants) {
                if (!impl->eventHandler) {
                    return;
                }
                vector<str> args;
                args.reserve(variants.size());
                for (const Rml::Variant& variant : variants) {
                    args.push_back(variant.Get<Rml::String>());
                }
                impl->eventHandler(modelName, event, args);
            });
    }
    store->handle = constructor.GetModelHandle();
    pimpl->models.emplace(desc.name, std::move(store));
    return true;
}

void UiSystem::setModelEventHandler(UiModelEventHandler handler) {
    if (pimpl) {
        pimpl->eventHandler = std::move(handler);
    }
}

void UiSystem::setNumber(const str& model, const str& slot, f64 value) {
    if (!pimpl) {
        return;
    }
    const auto it = pimpl->models.find(model);
    if (it == pimpl->models.end()) {
        return;
    }
    const auto slotIt = it->second->numbers.find(slot);
    if (slotIt == it->second->numbers.end() || slotIt->second == value) {
        return;
    }
    slotIt->second = value;
    it->second->handle.DirtyVariable(slot);
}

void UiSystem::setString(const str& model, const str& slot,
                         const str& value) {
    if (!pimpl) {
        return;
    }
    const auto it = pimpl->models.find(model);
    if (it == pimpl->models.end()) {
        return;
    }
    const auto slotIt = it->second->strings.find(slot);
    if (slotIt == it->second->strings.end() || slotIt->second == value) {
        return;
    }
    slotIt->second = value;
    it->second->handle.DirtyVariable(slot);
}

void UiSystem::setBool(const str& model, const str& slot, bool value) {
    if (!pimpl) {
        return;
    }
    const auto it = pimpl->models.find(model);
    if (it == pimpl->models.end()) {
        return;
    }
    const auto slotIt = it->second->bools.find(slot);
    if (slotIt == it->second->bools.end() || slotIt->second == value) {
        return;
    }
    slotIt->second = value;
    it->second->handle.DirtyVariable(slot);
}

str UiSystem::getString(const str& model, const str& slot) const {
    if (!pimpl) {
        return {};
    }
    const auto it = pimpl->models.find(model);
    if (it == pimpl->models.end()) {
        return {};
    }
    const auto slotIt = it->second->strings.find(slot);
    return slotIt != it->second->strings.end() ? str { slotIt->second }
                                               : str {};
}

void UiSystem::setRows(const str& model, vector<UiRow> rows) {
    if (!pimpl) {
        return;
    }
    const auto it = pimpl->models.find(model);
    if (it == pimpl->models.end()) {
        return;
    }
    if (it->second->rows == rows) {
        return; // dirtying rebuilds the data-for — not on unchanged data
    }
    it->second->rows = std::move(rows);
    it->second->handle.DirtyVariable("rows");
}

} // namespace ui
