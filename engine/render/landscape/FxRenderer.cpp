#include "engine/render/landscape/FxRenderer.hpp"

#include "engine/FrameContext.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kShader = "fxparticle";
constexpr const char* kFlameShader = "fxflame";
constexpr u32 kMinCapacity = 1024;
} // namespace

void FxRenderer::create(rhi::Device& device, ShaderLibrary& shaders) {
    shaders.load(kShader, { { "FrameUbo", 0 } });
    shaders.load(kFlameShader, { { "FrameUbo", 0 } },
                 { { "uFlameSheet", 3 }, { "uSceneDepth", 1 } });
    sheetSampler = { device, device.createSampler({}) }; // linear clamp
    ensurePipelines(device, shaders);
}

void FxRenderer::destroy(rhi::Device&) {
    alphaPipeline.reset();
    additivePipeline.reset();
    flamePipeline.reset();
    sheetGroup.reset();
    sheetSampler.reset();
    boundSheet = {};
    group.reset();
    instances.reset();
    capacity = 0;
    shaderWatch = {};
}

void FxRenderer::ensurePipelines(rhi::Device& device,
                                 ShaderLibrary& shaders) {
    if (!shaderWatch.changed(shaders) && alphaPipeline.id() != 0) {
        return;
    }
    shaders.beginWatch();
    const auto make = [&](rhi::BlendMode blend, const char* shader) {
        return rhi::UniquePipeline { device, device.createPipeline(
            { .shader = shaders.get(shader),
              .blend = blend,
              // Transparents: tested against the opaques, never writing.
              .depth = { .testEnable = true,
                         .writeEnable = false,
                         .compare = rhi::CompareFunc::Greater }, // reversed-Z
              .cull = rhi::CullMode::None,
              // ivec4 uFxBase: this batch's first particle in the shared SSBO.
              .pushConstantSize = 16 }) };
    };
    alphaPipeline = make(rhi::BlendMode::Alpha, kShader);
    additivePipeline = make(rhi::BlendMode::Additive, kShader);
    flamePipeline = make(rhi::BlendMode::Alpha, kFlameShader);
    shaderWatch = shaders.endWatch();
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
                      const vector<FxInstance>& additive,
                      const vector<FxInstance>& flames,
                      rhi::TextureHandle flameSheet,
                      rhi::BindGroupHandle sceneDepthGroup) {
    if (alpha.empty() && additive.empty() && flames.empty()) {
        return;
    }
    ensurePipelines(frame.device, shaders);
    if (!flames.empty() && flameSheet.id != 0 &&
        (sheetGroup.id() == 0 || boundSheet.id != flameSheet.id)) {
        sheetGroup = { frame.device, frame.device.createBindGroup(
            { .entries = { { .binding = 3,
                             .texture = flameSheet,
                             .sampler = sheetSampler } } }) };
        boundSheet = flameSheet;
    }
    // The batches live in the SSBO at once (alpha, additive, flames), so
    // it is written ONCE per frame and never rewritten between draws.
    const u32 needed =
        static_cast<u32>(alpha.size() + additive.size() + flames.size());
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
    if (!flames.empty()) {
        frame.device.updateBuffer(
            instances, flames.data(), flames.size() * sizeof(FxInstance),
            (alpha.size() + additive.size()) * sizeof(FxInstance));
    }
    // Alpha first, far-to-near (the caller sorted); flames (alpha too,
    // sorted) next; additive last — order-free over the blended layers.
    drawBatch(frame, alpha, 0, alphaPipeline, frameGroup);
    if (!flames.empty() && sheetGroup.id() != 0 && flameSheet.id != 0) {
        frame.cmd.setBindGroup(2, sheetGroup);
    }
    if (!flames.empty() && sceneDepthGroup.id != 0) {
        // The scene's depth (the water pass's snapshot group, unit 1):
        // the flames fade where they cross the ground.
        frame.cmd.setBindGroup(3, sceneDepthGroup);
    }
    drawBatch(frame, flames, static_cast<u32>(alpha.size() + additive.size()),
              // Flames need their sheet; without one they are plain sprites.
              flameSheet.id != 0 ? flamePipeline : alphaPipeline, frameGroup);
    drawBatch(frame, additive, static_cast<u32>(alpha.size()),
              additivePipeline, frameGroup);
}

} // namespace render
