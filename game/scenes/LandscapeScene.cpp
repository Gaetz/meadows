#include "game/scenes/LandscapeScene.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace game {

namespace {

// Placeholder daylight sky until the SkySystem paints a real gradient.
constexpr rhi::Color kSkyBlue { 0.45f, 0.71f, 0.95f, 1.0f };

// Unit cube, 24 vertices (position + per-face shade), CCW front faces so
// CullMode::Back is exercised. Interleaved: pos.xyz, shade.rgb.
constexpr f32 kCubeVertices[] = {
    // +Z
    -0.5f, -0.5f,  0.5f,  0.90f, 0.90f, 0.90f,
     0.5f, -0.5f,  0.5f,  0.90f, 0.90f, 0.90f,
     0.5f,  0.5f,  0.5f,  0.90f, 0.90f, 0.90f,
    -0.5f,  0.5f,  0.5f,  0.90f, 0.90f, 0.90f,
    // -Z
     0.5f, -0.5f, -0.5f,  0.55f, 0.55f, 0.55f,
    -0.5f, -0.5f, -0.5f,  0.55f, 0.55f, 0.55f,
    -0.5f,  0.5f, -0.5f,  0.55f, 0.55f, 0.55f,
     0.5f,  0.5f, -0.5f,  0.55f, 0.55f, 0.55f,
    // +X
     0.5f, -0.5f,  0.5f,  0.75f, 0.75f, 0.75f,
     0.5f, -0.5f, -0.5f,  0.75f, 0.75f, 0.75f,
     0.5f,  0.5f, -0.5f,  0.75f, 0.75f, 0.75f,
     0.5f,  0.5f,  0.5f,  0.75f, 0.75f, 0.75f,
    // -X
    -0.5f, -0.5f, -0.5f,  0.65f, 0.65f, 0.65f,
    -0.5f, -0.5f,  0.5f,  0.65f, 0.65f, 0.65f,
    -0.5f,  0.5f,  0.5f,  0.65f, 0.65f, 0.65f,
    -0.5f,  0.5f, -0.5f,  0.65f, 0.65f, 0.65f,
    // +Y
    -0.5f,  0.5f,  0.5f,  1.00f, 1.00f, 1.00f,
     0.5f,  0.5f,  0.5f,  1.00f, 1.00f, 1.00f,
     0.5f,  0.5f, -0.5f,  1.00f, 1.00f, 1.00f,
    -0.5f,  0.5f, -0.5f,  1.00f, 1.00f, 1.00f,
    // -Y
    -0.5f, -0.5f, -0.5f,  0.40f, 0.40f, 0.40f,
     0.5f, -0.5f, -0.5f,  0.40f, 0.40f, 0.40f,
     0.5f, -0.5f,  0.5f,  0.40f, 0.40f, 0.40f,
    -0.5f, -0.5f,  0.5f,  0.40f, 0.40f, 0.40f,
};

constexpr u16 kCubeIndices[] = {
    0,  1,  2,  0,  2,  3,   // +Z
    4,  5,  6,  4,  6,  7,   // -Z
    8,  9,  10, 8,  10, 11,  // +X
    12, 13, 14, 12, 14, 15,  // -X
    16, 17, 18, 16, 18, 19,  // +Y
    20, 21, 22, 20, 22, 23,  // -Y
};

struct CubeInstance {
    Vec3 offset;
    Vec3 tint;
};

// Embedded until the ShaderLibrary brick; GLSL 460 — this scene is the GL 4.6
// path (a 4.1 context fails shader compile with a logged error, no crash).
const char* kVertexShader = R"glsl(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aShade;
layout(location = 2) in vec3 aOffset;
layout(location = 3) in vec3 aTint;
layout(std140, binding = 0) uniform FrameUbo {
    mat4 uViewProj;
    vec4 uCameraPos;
    vec4 uTime;
};
out vec3 vColor;
void main() {
    vColor = aShade * aTint;
    gl_Position = uViewProj * vec4(aPos + aOffset, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 460 core
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, 1.0);
}
)glsl";

vector<CubeInstance> buildCubeGrid() {
    // A field of cubes with deterministic height/tint variation: enough
    // parallax to judge fly-camera motion and depth from any direction.
    constexpr i32 kHalfExtent = 8;
    constexpr f32 kSpacing = 4.0f;
    vector<CubeInstance> instances;
    for (i32 gz = -kHalfExtent; gz <= kHalfExtent; ++gz) {
        for (i32 gx = -kHalfExtent; gx <= kHalfExtent; ++gx) {
            const f32 x = static_cast<f32>(gx) * kSpacing;
            const f32 z = static_cast<f32>(gz) * kSpacing;
            const f32 y = std::sin(x * 0.35f) * std::cos(z * 0.25f) * 2.0f;
            const f32 hue = 0.5f + 0.5f * std::sin(x * 0.15f + z * 0.1f);
            instances.push_back({
                .offset = { x, y, z },
                .tint = { 0.35f + 0.55f * hue, 0.55f, 0.85f - 0.55f * hue },
            });
        }
    }
    return instances;
}

} // namespace

void LandscapeScene::onEnter() {
    rhi::Device& device = engine->getDevice();

    vertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kCubeVertices) },
        kCubeVertices);
    indexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kCubeIndices) },
        kCubeIndices);

    const vector<CubeInstance> instances = buildCubeGrid();
    instanceCount = static_cast<u32>(instances.size());
    instanceBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = instances.size() * sizeof(CubeInstance) },
        instances.data());

    frameUbo = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(render::FrameUniforms),
                                     .dynamic = true },
                                   nullptr);
    frameBindGroup = device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = frameUbo } } });

    shader = device.createShader({ .debugName = "landscape-cubes",
                                   .vertexSource = kVertexShader,
                                   .fragmentSource = kFragmentShader,
                                   .uniformBlocks = { { "FrameUbo", 0 } } });
    pipeline = device.createPipeline(
        { .shader = shader,
          .vertexBuffers =
              { { .stride = 6 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = 0 },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = 3 * sizeof(f32) } } },
                { .stride = sizeof(CubeInstance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 2,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(CubeInstance, offset) },
                                  { .location = 3,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(CubeInstance, tint) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back });

    flyCamera.camera.position = { 0.0f, 6.0f, 26.0f };
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    engine->getWindow().setRelativeMouseMode(false);
    device.destroyPipeline(pipeline);
    device.destroyShader(shader);
    device.destroyBindGroup(frameBindGroup);
    device.destroyBuffer(frameUbo);
    device.destroyBuffer(instanceBuffer);
    device.destroyBuffer(indexBuffer);
    device.destroyBuffer(vertexBuffer);
}

void LandscapeScene::update(f32 dt) {
    timeSeconds += dt;
    flyCamera.update(engine->getInput(), engine->getWindow(), dt);
}

void LandscapeScene::render(engine::FrameContext& frame) {
    const render::Camera3D& camera = flyCamera.camera;
    const render::FrameUniforms uniforms {
        .viewProj = camera.viewProj(frame.aspect),
        .cameraPos = { camera.position, 1.0f },
        .time = { timeSeconds, 0.0f, 0.0f, 0.0f },
    };
    frame.device.updateBuffer(frameUbo, &uniforms, sizeof(uniforms), 0);

    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = kSkyBlue,
                                .depthLoadOp = rhi::LoadOp::Clear });
    frame.cmd.setPipeline(pipeline);
    frame.cmd.setVertexBuffer(0, vertexBuffer);
    frame.cmd.setVertexBuffer(1, instanceBuffer);
    frame.cmd.setIndexBuffer(indexBuffer, rhi::IndexFormat::U16);
    frame.cmd.setBindGroup(0, frameBindGroup);
    frame.cmd.drawIndexed(static_cast<u32>(std::size(kCubeIndices)),
                          instanceCount);
    frame.cmd.endRenderPass();
}

void LandscapeScene::drawUi() {
    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Brick 3: fly camera + FrameUniforms UBO.");
    ImGui::TextUnformatted(
        "Hold RMB: mouselook | WASD: move | E/Space: up | Q/Ctrl: down\n"
        "Shift: speed boost");
    const Vec3 p = flyCamera.camera.position;
    ImGui::Text("Position: %.1f  %.1f  %.1f", p.x, p.y, p.z);
    ImGui::Text("Yaw %.2f  Pitch %.2f", flyCamera.camera.yaw,
                flyCamera.camera.pitch);
    ImGui::End();
}

} // namespace game
