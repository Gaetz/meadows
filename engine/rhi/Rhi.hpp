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
// subsystem boundaries (§8). 0 = invalid. Creation/destruction functions
// appear on Device as each resource type is needed.

struct BufferHandle    { u32 id { 0 }; };
struct TextureHandle   { u32 id { 0 }; };
struct SamplerHandle   { u32 id { 0 }; };
struct ShaderHandle    { u32 id { 0 }; };
struct PipelineHandle  { u32 id { 0 }; };
struct BindGroupHandle { u32 id { 0 }; };

// --- Render pass ---------------------------------------------------------------

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
