#pragma once

#include "engine/core/Defines.hpp"

namespace rhi {

// Which graphics API drives the Device. Chosen at runtime (§2.1); one binary
// holds every backend it was compiled with (which ones are compiled is a
// compile-time CMake gate — MEADOWS_RHI_VULKAN / _GL46 / _GL41). The startup
// selection is a preference/fallback chain (Vulkan -> GL); see Device::create.
enum class Backend {
    OpenGL, // GL 4.6 (DSA) with a GL 4.1 fallback inside createGlDevice
    Vulkan, // Vulkan 1.x (+ MoltenVK on macOS), behind the same interface
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

struct BufferHandle      { u32 id { 0 }; };
struct TextureHandle     { u32 id { 0 }; };
struct SamplerHandle     { u32 id { 0 }; };
struct ShaderHandle      { u32 id { 0 }; };
struct PipelineHandle    { u32 id { 0 }; };
struct BindGroupHandle   { u32 id { 0 }; };
struct FramebufferHandle { u32 id { 0 }; };
struct FenceHandle       { u32 id { 0 }; }; // single-use GPU marker (P1)
struct TimestampHandle   { u32 id { 0 }; }; // single-use GPU clock (GPU-PERF P0)

// --- Capabilities --------------------------------------------------------------

// Per-feature flags, granular on purpose: a future GL 4.1 degraded mode
// lights features up one by one without changing this contract. Renderer
// systems check the flags they need and disable themselves gracefully.
struct DeviceCaps {
    bool offscreenTargets { false }; // framebuffers + RenderPassDesc.framebuffer
    bool textureArrays { false };    // TextureDesc::arrayLayers > 1
    bool hdrFormats { false };       // RGBA16F / R16F attachments
    bool samplerObjects { false };   // createSampler + BindGroupEntry::sampler
    bool mipmapGeneration { false }; // Device::generateMipmaps
    bool copyTexture { false };      // CommandBuffer::copyTexture
    bool computeShaders { false };   // compute pipelines + dispatch + SSBOs
                                     // (GL >= 4.3; absent on the 4.1 path)
    bool timerQueries { false };     // insertTimestamp/timestampReady
                                     // (GL >= 3.3 timer queries)
    bool volumeTextures { false };   // TextureDesc::depth > 1 (3D textures —
                                     // GI voxel clipmap / radiance cascades)
};

// --- Buffers -------------------------------------------------------------------

enum class BufferUsage {
    Vertex,
    Index,
    Uniform,
    Storage, // SSBO: read/written by compute shaders (caps.computeShaders)
};

struct BufferDesc {
    BufferUsage usage { BufferUsage::Vertex };
    u64 size { 0 };
    // True for buffers rewritten every frame (instance data, per-frame
    // uniforms); lets the backend pick an upload strategy.
    bool dynamic { false };
    // True for GPU-written buffers read back via Device::readBuffer every
    // frame (compute culling results): the backend keeps them host-visible
    // (Vulkan: readback heap) instead of bouncing VRAM->RAM per read.
    bool readback { false };
};

// --- Textures ------------------------------------------------------------------

enum class TextureFormat {
    RGBA8,
    SRGBA8,   // sRGB-decoded on sample (albedo/splat tiles)
    RGBA16F,  // HDR color target
    R16F,     // single-channel (AO, masks)
    R32F,     // full-precision single channel (Hi-Z depth pyramid)
    Depth32F, // depth attachment, sampleable (shadow maps, scene depth)
};

enum class FilterMode {
    Nearest,
    Linear,
};

enum class AddressMode {
    ClampToEdge,
    Repeat,
};

// Bitmask. Attachment textures stay sampleable afterwards (shadow maps,
// post-process chains).
enum TextureUsage : u32 {
    TextureUsage_Sampled          = 1u << 0,
    TextureUsage_RenderAttachment = 1u << 1,
};

// `pixels` at creation covers the base mip of every layer, tightly packed
// and contiguous (width*height*bpp*arrayLayers). Only RGBA8/SRGBA8 accept
// initial pixels; render-target formats are created empty.
struct TextureDesc {
    u32 width { 0 };
    u32 height { 0 };
    u32 arrayLayers { 1 }; // > 1 = texture array (splat layers, CSM cascades)
    // > 1 = 3D VOLUME texture (Vulkan VK_IMAGE_TYPE_3D) — GI voxel clipmap
    // and radiance cascades (chantier RC, G0). Exclusive with arrayLayers;
    // GPU-written via storageImage, never uploaded; caps().volumeTextures.
    u32 depth { 1 };
    u32 mipLevels { 1 };   // storage levels; fill base, then generateMipmaps()
    TextureFormat format { TextureFormat::RGBA8 };
    FilterMode filter { FilterMode::Nearest };
    AddressMode wrap { AddressMode::ClampToEdge };
    u32 usage { TextureUsage_Sampled };
};

// --- Samplers --------------------------------------------------------------------

enum class CompareFunc {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

// Immutable sampler object (Vulkan VkSampler). When CompareFunc != Never the
// sampler is a COMPARISON sampler (COMPARE_REF_TO_TEXTURE) for shadow PCF.
struct SamplerDesc {
    FilterMode minFilter { FilterMode::Linear };
    FilterMode magFilter { FilterMode::Linear };
    bool mipmapFilter { false }; // trilinear across mips when true
    AddressMode addressU { AddressMode::ClampToEdge };
    AddressMode addressV { AddressMode::ClampToEdge };
    AddressMode addressW { AddressMode::ClampToEdge }; // 3D volumes (G0)
    CompareFunc compare { CompareFunc::Never }; // Never = regular sampler
    f32 maxAnisotropy { 1.0f };
};

// --- Shaders -------------------------------------------------------------------

// Sources are backend-specific for now (GLSL for GL). Offline SPIR-V
// cross-compilation becomes the path once a second backend exists.
//
// uniformBlocks / samplers: explicit binding assignments applied after link via
// glUniformBlockBinding / glUniform1i. Required on GL 4.1 (no layout(binding=N)
// in GLSL 4.10); harmless on GL 4.6 (overrides the layout qualifier).
struct UniformBlockBinding {
    str name;
    u32 binding { 0 };
};
struct SamplerBinding {
    str name;
    u32 unit { 0 };
};
struct ShaderDesc {
    str debugName;
    str vertexSource;
    str fragmentSource;
    // Compute program when non-empty (vertex/fragment must be empty);
    // requires caps.computeShaders.
    str computeSource;
    vector<UniformBlockBinding> uniformBlocks;
    vector<SamplerBinding>      samplers;
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
    // Premultiplied alpha (ONE, ONE_MINUS_SRC_ALPHA) — RmlUi's output.
    PremultipliedAlpha,
};

enum class PrimitiveTopology {
    Triangles,
    TriangleStrip,
    Lines,
};

// Depth test/write state, part of the immutable pipeline (Vulkan
// VkPipelineDepthStencilState). Defaults keep every 2D pipeline unchanged.
struct DepthState {
    bool testEnable { false };
    bool writeEnable { false };
    CompareFunc compare { CompareFunc::Less };
};

enum class CullMode {
    None,
    Back,
    Front,
};

// Winding of front faces (Vulkan VK_DYNAMIC_STATE_FRONT_FACE). Mirrored
// passes (planar reflections) flip it instead of duplicating pipelines.
enum class FrontFace {
    CounterClockwise, // default
    Clockwise,
};

// Whole-pipeline state object, immutable once created (maps 1:1 to a Vulkan
// pipeline later; the GL backend translates it to program + VAO + state).
// The backend applies ALL state — enables and disables — on every setPipeline,
// so no state leaks between passes (2D after 3D, ImGui after either).
struct PipelineDesc {
    ShaderHandle shader;
    vector<VertexBufferLayout> vertexBuffers; // index = buffer slot
    BlendMode blend { BlendMode::Opaque };
    PrimitiveTopology topology { PrimitiveTopology::Triangles };
    DepthState depth {};
    CullMode cull { CullMode::None };
    // Polygon offset for shadow casters (Vulkan depthBias*). Applied when
    // either value is non-zero.
    f32 depthBias { 0.0f };
    f32 depthBiasSlope { 0.0f };
    // Debug rasterization (Vulkan polygonMode = LINE).
    bool wireframe { false };
    // Bytes of push-constant data the shader declares (0 = none). Vulkan
    // needs the range at pipeline-layout creation, which is why this is
    // pipeline state and not a draw-time argument. Keep it <= 128: that is
    // the minimum every Vulkan implementation guarantees.
    u32 pushConstantSize { 0 };
};

// Reserved GL uniform-block binding backing push constants (see
// MEADOWS_PUSH_CONSTANTS in compat.glsl). Bind groups must not use it.
constexpr u32 kPushConstantBinding = 15;

// A compute pipeline is just its program (Vulkan VkComputePipeline): no
// vertex layout, no raster state. Dispatched via CommandBuffer::dispatch.
struct ComputePipelineDesc {
    ShaderHandle shader;
    // Same contract as PipelineDesc::pushConstantSize: per-dispatch constants
    // that stick to the dispatches following each setPushConstants.
    u32 pushConstantSize { 0 };
};

// --- Bind groups -----------------------------------------------------------------

// The resources a draw reads, bound as one immutable unit (a Vulkan
// descriptor set later). Exactly one of buffer/texture is set per entry;
// `binding` matches the shader's binding index. `sampler` is only valid
// alongside `texture` (combined image-sampler); when unset, the texture's
// own creation-time parameters apply (legacy 2D path). `storage` marks a
// buffer entry as an SSBO (Vulkan STORAGE_BUFFER descriptor type) instead
// of a uniform block.
// `storageImage` binds one mip of the texture for imageLoad/imageStore in a
// compute shader (Vulkan STORAGE_IMAGE) instead of sampling — the Hi-Z
// pyramid builds level N from level N-1 this way, no feedback loop.
struct BindGroupEntry {
    u32 binding { 0 };
    BufferHandle buffer {};
    TextureHandle texture {};
    SamplerHandle sampler {};
    bool storage { false };
    bool storageImage { false };
    u32 imageMip { 0 };
};

struct BindGroupDesc {
    vector<BindGroupEntry> entries;
};

// --- Framebuffers ----------------------------------------------------------------

// One mip level of one layer of a texture, used as a render target (a Vulkan
// image view). arrayLayer selects CSM cascades; mipLevel selects bloom mips.
struct FramebufferAttachment {
    TextureHandle texture {};
    u32 mipLevel { 0 };
    u32 arrayLayer { 0 };
};

// Fixed attachment set (a Vulkan VkFramebuffer). Empty colorAttachments with
// a depth attachment = depth-only pass (shadow maps). All attachments must
// share the same dimensions at their selected mip.
struct FramebufferDesc {
    vector<FramebufferAttachment> colorAttachments; // 0..4
    FramebufferAttachment depthAttachment {};       // texture.id 0 = none
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

// framebuffer 0 targets the swapchain backbuffer (existing callers
// unchanged). beginRenderPass binds the target and sets the viewport to its
// full size. Depth defaults clear to the far plane, which is a no-op for
// depth-less 2D passes and the right start for 3D ones.
struct RenderPassDesc {
    FramebufferHandle framebuffer {};
    LoadOp loadOp { LoadOp::Clear };
    Color clearColor {};
    LoadOp depthLoadOp { LoadOp::Clear };
    f32 clearDepth { 1.0f };
};

} // namespace rhi
