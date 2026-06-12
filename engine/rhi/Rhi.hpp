#pragma once

#include "engine/core/Defines.hpp"

namespace rhi {

// Which graphics API drives the Device. Chosen at runtime (§2.1); one binary
// holds every backend it was compiled with.
enum class Backend {
    OpenGL,
    // Vulkan comes later, behind the same interface.
};

struct Color {
    f32 r { 0.0f };
    f32 g { 0.0f };
    f32 b { 0.0f };
    f32 a { 1.0f };
};

// --- Resource handles --------------------------------------------------------
// Opaque ids owned by the Device: cheap to copy and safe to pass across
// subsystem boundaries (§8). 0 = invalid.

struct BufferHandle    { u32 id { 0 }; };
struct TextureHandle   { u32 id { 0 }; };
struct SamplerHandle   { u32 id { 0 }; };
struct ShaderHandle    { u32 id { 0 }; };
struct PipelineHandle  { u32 id { 0 }; };
struct BindGroupHandle { u32 id { 0 }; };

// --- Buffers -------------------------------------------------------------------

enum class BufferUsage {
    Vertex,
    Index,
    Uniform,
};

struct BufferDesc {
    BufferUsage usage { BufferUsage::Vertex };
    u64 size { 0 };
    // True for buffers rewritten every frame (instance data, per-frame
    // uniforms); lets the backend pick an upload strategy.
    bool dynamic { false };
};

// --- Textures ------------------------------------------------------------------

enum class TextureFormat {
    RGBA8,
};

enum class FilterMode {
    Nearest,
    Linear,
};

struct TextureDesc {
    u32 width { 0 };
    u32 height { 0 };
    TextureFormat format { TextureFormat::RGBA8 };
    FilterMode filter { FilterMode::Nearest };
};

// --- Shaders -------------------------------------------------------------------

// Sources are backend-specific for now (GLSL 460 for GL). Offline SPIR-V
// cross-compilation becomes the path once a second backend exists.
struct ShaderDesc {
    str debugName;
    str vertexSource;
    str fragmentSource;
};

// --- Pipelines -------------------------------------------------------------------

enum class VertexFormat {
    F32x1,
    F32x2,
    F32x3,
    F32x4,
};

struct VertexAttribute {
    u32 location { 0 };
    VertexFormat format { VertexFormat::F32x4 };
    u32 offset { 0 };
};

enum class VertexStepMode {
    Vertex,   // advance per vertex
    Instance, // advance per instance
};

// Layout of one vertex buffer slot (see CommandBuffer::setVertexBuffer).
struct VertexBufferLayout {
    u32 stride { 0 };
    VertexStepMode stepMode { VertexStepMode::Vertex };
    vector<VertexAttribute> attributes;
};

enum class BlendMode {
    Opaque,
    Alpha,
    Additive,
};

enum class PrimitiveTopology {
    Triangles,
    TriangleStrip,
    Lines,
};

// Whole-pipeline state object, immutable once created (maps 1:1 to a Vulkan
// pipeline later; the GL backend translates it to program + VAO + state).
struct PipelineDesc {
    ShaderHandle shader;
    vector<VertexBufferLayout> vertexBuffers; // index = buffer slot
    BlendMode blend { BlendMode::Opaque };
    PrimitiveTopology topology { PrimitiveTopology::Triangles };
};

// --- Bind groups -----------------------------------------------------------------

// The resources a draw reads, bound as one immutable unit (a Vulkan
// descriptor set later). Exactly one of buffer/texture is set per entry;
// `binding` matches the shader's binding index.
struct BindGroupEntry {
    u32 binding { 0 };
    BufferHandle buffer {};
    TextureHandle texture {};
};

struct BindGroupDesc {
    vector<BindGroupEntry> entries;
};

// --- Draw / render pass ------------------------------------------------------------

enum class IndexFormat {
    U16,
    U32,
};

enum class LoadOp {
    Clear,
    Load,
    DontCare,
};

// Targets the swapchain backbuffer for now; off-screen color/depth attachments
// arrive with the 3D phase.
struct RenderPassDesc {
    LoadOp loadOp { LoadOp::Clear };
    Color clearColor {};
};

} // namespace rhi
