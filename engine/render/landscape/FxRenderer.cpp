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
                         .compare = rhi::CompareFunc::Greater }, // reversed-Z
              .cull = rhi::CullMode::None,
              // ivec4 uFxBase: this batch's first particle in the shared SSBO.
              .pushConstantSize = 16 }) };
    };
    alphaPipeline = make(rhi::BlendMode::Alpha);
    additivePipeline = make(rhi::BlendMode::Additive);
    shaderGeneration = shaders.generation(kShader);
}

void FxRenderer::drawBatch(engine::FrameContext& frame,
                           const vector<FxInstance>& batch, u32 baseInstance,
                           rhi::PipelineHandle pipeline,
                           rhi::BindGroupHandle frameGroup) {
    if (batch.empty()) {
        return;
    }
    frame.cmd.setPipeline(pipeline);
    // The batch's slice was already uploaded; the shader offsets into it.
    // Rewriting the SSBO between the two draws would read back the LAST
    // batch on Vulkan (recorded draws share the buffer's final contents).
    const i32 push[4] = { static_cast<i32>(baseInstance), 0, 0, 0 };
    frame.cmd.setPushConstants(push, sizeof(push));
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
    // Both batches live in the SSBO at once (alpha then additive), so it is
    // written ONCE per frame and never rewritten between the two draws.
    const u32 needed = static_cast<u32>(alpha.size() + additive.size());
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
    // One upload for both slices, before any draw is recorded.
    if (!alpha.empty()) {
        frame.device.updateBuffer(instances, alpha.data(),
                                  alpha.size() * sizeof(FxInstance), 0);
    }
    if (!additive.empty()) {
        frame.device.updateBuffer(instances, additive.data(),
                                  additive.size() * sizeof(FxInstance),
                                  alpha.size() * sizeof(FxInstance));
    }
    // Alpha first, far-to-near (the caller sorted); additive after —
    // order-free over the already-blended alpha layer.
    drawBatch(frame, alpha, 0, alphaPipeline, frameGroup);
    drawBatch(frame, additive, static_cast<u32>(alpha.size()),
              additivePipeline, frameGroup);
}

} // namespace render
