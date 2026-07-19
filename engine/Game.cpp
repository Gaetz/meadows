#include "engine/Game.hpp"

#include "engine/FrameContext.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/CommandBuffer.hpp"

namespace engine {

void Game::render(FrameContext& frame) {
    // Collect + upload before the pass (Vulkan: in-pass buffer writes race
    // the in-flight frame — see SpriteRenderer::upload).
    frame.sprites.begin(frame.camera2d, frame.aspect);
    draw(frame.sprites);
    frame.sprites.upload();
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = frame.clearColor });
    frame.sprites.end(frame.cmd);
    frame.cmd.endRenderPass();
}

} // namespace engine
