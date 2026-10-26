#include "engine/Game.hpp"

#include "engine/FrameContext.hpp"
#include "engine/rhi/CommandBuffer.hpp"

namespace engine {

void Game::render(FrameContext& frame) {
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = frame.clearColor });
    frame.cmd.endRenderPass();
}

} // namespace engine
