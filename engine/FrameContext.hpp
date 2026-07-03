#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Camera2D.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}
namespace render {
class SpriteRenderer;
}

namespace engine {

// Everything one frame's render step may touch, handed by the Engine to
// Game::render. The frame owner records its render passes into `cmd` and must
// leave the backbuffer holding the finished scene (ImGui draws after). The
// default Game::render reproduces the classic sprite path; a 3D frame owner
// records its own pass chain instead.
struct FrameContext {
    rhi::Device& device;
    rhi::CommandBuffer& cmd;
    render::SpriteRenderer& sprites;
    render::Camera2D& camera2d;
    rhi::Color clearColor {};
    f32 dt { 0.0f };
    f32 aspect { 1.0f };
    u32 width { 0 };
    u32 height { 0 };
};

} // namespace engine
