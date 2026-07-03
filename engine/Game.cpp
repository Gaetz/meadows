#include "engine/Game.hpp"

#include "engine/FrameContext.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/CommandBuffer.hpp"

namespace engine {

void Game::render(FrameContext& frame) {
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = frame.clearColor });
    frame.sprites.begin(frame.camera2d, frame.aspect);
    draw(frame.sprites);
    frame.sprites.end(frame.cmd);
    frame.cmd.endRenderPass();
}

} // namespace engine
