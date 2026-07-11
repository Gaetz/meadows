#include "engine/render/landscape/FxRenderer.hpp"

#include "engine/FrameContext.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kShader = "fxparticle";
constexpr u32 kMinCapacity = 1024;
} // namespace

void FxRenderer::create(rhi::Device& device, ShaderLibrary& shaders) {
    shaders.load(kShader, { { "FrameUbo", 0 } });
    ensurePipelines(device, shaders);
}

void FxRenderer::destroy(rhi::Device&) {
    alphaPipeline.reset();
    additivePipeline.reset();
    group.reset();
    instances.reset();
    capacity = 0;
    shaderGeneration = 0;
}

void FxRenderer::ensurePipelines(rhi::Device& device,
                                 ShaderLibrary& shaders) {
    if (shaders.generation(kShader) == shaderGeneration &&
        alphaPipeline.id() != 0) {
        return;
    }
    const auto make = [&](rhi::BlendMode blend) {
        return rhi::UniquePipeline { device, device.createPipeline(
            { .shader = shaders.get(kShader),
              .blend = blend,
              // Transparents: tested against the opaques, never writing.
              .depth = { .testEnable = true,
                         .writeEnable = false,
                         .compare = rhi::CompareFunc::Less },
              .cull = rhi::CullMode::None }) };
    };
    alphaPipeline = make(rhi::BlendMode::Alpha);
    additivePipeline = make(rhi::BlendMode::Additive);
    shaderGeneration = shaders.generation(kShader);
}

void FxRenderer::drawBatch(engine::FrameContext& frame,
                           const vector<FxInstance>& batch,
                           rhi::PipelineHandle pipeline,
                           rhi::BindGroupHandle frameGroup) {
    if (batch.empty()) {
        return;
    }
    frame.device.updateBuffer(instances, batch.data(),
                              batch.size() * sizeof(FxInstance), 0);
    frame.cmd.setPipeline(pipeline);
    frame.cmd.setBindGroup(0, frameGroup);
    frame.cmd.setBindGroup(1, group);
    frame.cmd.draw(static_cast<u32>(batch.size()) * 6);
}

void FxRenderer::draw(engine::FrameContext& frame, ShaderLibrary& shaders,
                      rhi::BindGroupHandle frameGroup,
                      const vector<FxInstance>& alpha,
                      const vector<FxInstance>& additive) {
    if (alpha.empty() && additive.empty()) {
        return;
    }
    ensurePipelines(frame.device, shaders);
    // Size the SSBO for the frame's BIGGEST batch before any draw is
    // recorded — never recreate it between the two (the first draw
    // still references it).
    const u32 needed = static_cast<u32>(
        glm::max(alpha.size(), additive.size()));
    if (needed > capacity || instances.id() == 0) {
        capacity = glm::max(needed, kMinCapacity);
        instances = { frame.device, frame.device.createBuffer(
            { .usage = rhi::BufferUsage::Storage,
              .size = capacity * sizeof(FxInstance),
              .dynamic = true },
            nullptr) };
        group = { frame.device, frame.device.createBindGroup(
            { .entries = { { .binding = 2,
                             .buffer = instances,
                             .storage = true } } }) };
    }
    // Alpha first, far-to-near (the caller sorted); additive after —
    // order-free over the already-blended alpha layer. GL keeps the two
    // sequential upload+draw pairs coherent.
    drawBatch(frame, alpha, alphaPipeline, frameGroup);
    drawBatch(frame, additive, additivePipeline, frameGroup);
}

} // namespace render
