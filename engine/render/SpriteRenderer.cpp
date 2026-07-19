#include "engine/render/SpriteRenderer.hpp"

#include <cstddef>

#include "engine/core/Log.hpp"

namespace render {

namespace {

// Embedded until the asset system (Phase 1) provides shader files via the VFS.
// Written in GLSL 4.10 (compatible with both GL 4.1 and GL 4.6).
// Binding points are not declared in GLSL (that requires 4.2+); they are set
// explicitly via glUniformBlockBinding / glUniform1i after link (see ShaderDesc
// uniformBlocks / samplers fields).
const char* kVertexShader = R"glsl(
#version 410 core
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aPosSize;
layout(location = 3) in vec4 aUvRect;
layout(location = 4) in vec4 aTint;
layout(location = 5) in float aRotation;

layout(std140) uniform Camera {
    mat4 uViewProj;
};

out vec2 vUv;
out vec4 vTint;

void main() {
    vec2 local = aCorner * aPosSize.zw;
    float c = cos(aRotation);
    float s = sin(aRotation);
    vec2 world = aPosSize.xy + vec2(c * local.x - s * local.y,
                                    s * local.x + c * local.y);
    gl_Position = uViewProj * vec4(world, 0.0, 1.0);
    vUv = mix(aUvRect.xy, aUvRect.zw, aUv);
    vTint = aTint;
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 410 core
in vec2 vUv;
in vec4 vTint;

uniform sampler2D uTexture;

out vec4 fragColor;

void main() {
    fragColor = texture(uTexture, vUv) * vTint;
}
)glsl";

// Unit quad centered on the origin: corner.xy in [-0.5, 0.5], uv in [0, 1].
constexpr f32 kQuadVertices[] = {
    // corner       uv
    -0.5f, -0.5f,   0.0f, 0.0f,
     0.5f, -0.5f,   1.0f, 0.0f,
    -0.5f,  0.5f,   0.0f, 1.0f,
     0.5f,  0.5f,   1.0f, 1.0f,
};

constexpr u16 kQuadIndices[] = { 0, 1, 2, 2, 1, 3 };

} // namespace

SpriteRenderer::SpriteRenderer(rhi::Device& device) : device { device } {
}

uptr<SpriteRenderer> SpriteRenderer::create(rhi::Device& device) {
    const rhi::ShaderHandle shader = device.createShader({
        .debugName = "sprite",
        .vertexSource = kVertexShader,
        .fragmentSource = kFragmentShader,
        .uniformBlocks = { { "Camera", 0 } },
        .samplers      = { { "uTexture", 0 } },
    });
    if (shader.id == 0) {
        return nullptr;
    }

    auto renderer = uptr<SpriteRenderer> { new SpriteRenderer(device) };
    renderer->shader = shader;

    renderer->quadVertices = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kQuadVertices) },
        kQuadVertices);
    renderer->quadIndices = device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kQuadIndices) },
        kQuadIndices);
    renderer->instanceBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = kMaxSprites * sizeof(Instance),
          .dynamic = true });
    renderer->cameraUbo = device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = sizeof(Mat4),
          .dynamic = true });

    const u32 white = 0xFFFFFFFF;
    renderer->whiteTexture =
        device.createTexture({ .width = 1, .height = 1 }, &white);

    renderer->pipeline = device.createPipeline({
        .shader = shader,
        .vertexBuffers = {
            // slot 0: quad geometry, per vertex
            { .stride = 4 * sizeof(f32),
              .stepMode = rhi::VertexStepMode::Vertex,
              .attributes = {
                  { .location = 0, .format = rhi::VertexFormat::F32x2,
                    .offset = 0 },
                  { .location = 1, .format = rhi::VertexFormat::F32x2,
                    .offset = 2 * sizeof(f32) },
              } },
            // slot 1: sprite data, per instance
            { .stride = sizeof(Instance),
              .stepMode = rhi::VertexStepMode::Instance,
              .attributes = {
                  { .location = 2, .format = rhi::VertexFormat::F32x4,
                    .offset = offsetof(Instance, posSize) },
                  { .location = 3, .format = rhi::VertexFormat::F32x4,
                    .offset = offsetof(Instance, uvRect) },
                  { .location = 4, .format = rhi::VertexFormat::F32x4,
                    .offset = offsetof(Instance, tint) },
                  { .location = 5, .format = rhi::VertexFormat::F32x1,
                    .offset = offsetof(Instance, rotation) },
              } },
        },
        .blend = rhi::BlendMode::Alpha,
    });
    if (renderer->pipeline.id == 0) {
        return nullptr;
    }

    renderer->instances.reserve(kMaxSprites);
    return renderer;
}

SpriteRenderer::~SpriteRenderer() {
    for (auto& [textureId, group] : bindGroups) {
        device.destroyBindGroup(group);
    }
    device.destroyPipeline(pipeline);
    device.destroyShader(shader);
    device.destroyTexture(whiteTexture);
    device.destroyBuffer(cameraUbo);
    device.destroyBuffer(instanceBuffer);
    device.destroyBuffer(quadIndices);
    device.destroyBuffer(quadVertices);
}

void SpriteRenderer::begin(const Camera2D& camera, f32 aspect) {
    const Mat4 viewProj = camera.viewProj(aspect);
    device.updateBuffer(cameraUbo, &viewProj, sizeof(Mat4));
    instances.clear();
    batches.clear();
}

void SpriteRenderer::draw(const Sprite& sprite) {
    if (instances.size() >= kMaxSprites) {
        if (!overflowWarned) {
            LOG_WARN("SpriteRenderer: more than {} sprites in one frame, "
                     "extra sprites are dropped",
                     kMaxSprites);
            overflowWarned = true;
        }
        return;
    }

    const u32 textureId =
        sprite.texture.id != 0 ? sprite.texture.id : whiteTexture.id;
    if (batches.empty() || batches.back().textureId != textureId) {
        batches.push_back({ .textureId = textureId,
                            .firstInstance = static_cast<u32>(instances.size()),
                            .instanceCount = 0 });
    }
    batches.back().instanceCount++;

    instances.push_back({
        .posSize = { sprite.position.x, sprite.position.y, sprite.size.x,
                     sprite.size.y },
        .uvRect = sprite.uvRect,
        .tint = sprite.tint,
        .rotation = sprite.rotation,
    });
}

void SpriteRenderer::end(rhi::CommandBuffer& cmd) {
    if (instances.empty()) {
        return;
    }

    cmd.setPipeline(pipeline);
    cmd.setVertexBuffer(0, quadVertices);
    cmd.setIndexBuffer(quadIndices, rhi::IndexFormat::U16);

    // ONE upload for the whole frame, before any draw is recorded; each batch
    // then selects its slice with a vertex-buffer offset. Uploading per batch
    // (the previous shape) only worked because GL executes in order — on
    // Vulkan every recorded draw would read the LAST batch's instances.
    // The offset also replaces the firstInstance this used to avoid, so
    // GL_ARB_base_instance stays unnecessary (absent on macOS GL 4.1).
    device.updateBuffer(instanceBuffer, instances.data(),
                        instances.size() * sizeof(Instance));
    for (const Batch& batch : batches) {
        cmd.setVertexBuffer(1, instanceBuffer,
                            static_cast<u64>(batch.firstInstance) *
                                sizeof(Instance));
        cmd.setBindGroup(0, bindGroupFor({ batch.textureId }));
        cmd.drawIndexed(6, batch.instanceCount, 0, 0);
    }
}

rhi::BindGroupHandle SpriteRenderer::bindGroupFor(rhi::TextureHandle texture) {
    if (auto it = bindGroups.find(texture.id); it != bindGroups.end()) {
        return it->second;
    }
    const rhi::BindGroupHandle group = device.createBindGroup({
        .entries = {
            { .binding = 0, .buffer = cameraUbo },
            { .binding = 0, .texture = texture },
        },
    });
    bindGroups.emplace(texture.id, group);
    return group;
}

} // namespace render
