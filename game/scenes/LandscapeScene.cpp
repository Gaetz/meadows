#include "game/scenes/LandscapeScene.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace game {

namespace {

// Placeholder daylight sky until the SkySystem paints a real gradient.
constexpr rhi::Color kSkyBlue { 0.45f, 0.71f, 0.95f, 1.0f };

// Unit cube, 24 vertices (position + per-face color), CCW front faces so
// CullMode::Back is exercised. Interleaved: pos.xyz, color.rgb.
constexpr f32 kCubeVertices[] = {
    // +Z blue
    -0.5f, -0.5f,  0.5f,  0.30f, 0.45f, 0.95f,
     0.5f, -0.5f,  0.5f,  0.30f, 0.45f, 0.95f,
     0.5f,  0.5f,  0.5f,  0.30f, 0.45f, 0.95f,
    -0.5f,  0.5f,  0.5f,  0.30f, 0.45f, 0.95f,
    // -Z dark blue
     0.5f, -0.5f, -0.5f,  0.15f, 0.22f, 0.50f,
    -0.5f, -0.5f, -0.5f,  0.15f, 0.22f, 0.50f,
    -0.5f,  0.5f, -0.5f,  0.15f, 0.22f, 0.50f,
     0.5f,  0.5f, -0.5f,  0.15f, 0.22f, 0.50f,
    // +X red
     0.5f, -0.5f,  0.5f,  0.90f, 0.30f, 0.25f,
     0.5f, -0.5f, -0.5f,  0.90f, 0.30f, 0.25f,
     0.5f,  0.5f, -0.5f,  0.90f, 0.30f, 0.25f,
     0.5f,  0.5f,  0.5f,  0.90f, 0.30f, 0.25f,
    // -X dark red
    -0.5f, -0.5f, -0.5f,  0.45f, 0.15f, 0.12f,
    -0.5f, -0.5f,  0.5f,  0.45f, 0.15f, 0.12f,
    -0.5f,  0.5f,  0.5f,  0.45f, 0.15f, 0.12f,
    -0.5f,  0.5f, -0.5f,  0.45f, 0.15f, 0.12f,
    // +Y green
    -0.5f,  0.5f,  0.5f,  0.35f, 0.80f, 0.35f,
     0.5f,  0.5f,  0.5f,  0.35f, 0.80f, 0.35f,
     0.5f,  0.5f, -0.5f,  0.35f, 0.80f, 0.35f,
    -0.5f,  0.5f, -0.5f,  0.35f, 0.80f, 0.35f,
    // -Y dark green
    -0.5f, -0.5f, -0.5f,  0.15f, 0.40f, 0.15f,
     0.5f, -0.5f, -0.5f,  0.15f, 0.40f, 0.15f,
     0.5f, -0.5f,  0.5f,  0.15f, 0.40f, 0.15f,
    -0.5f, -0.5f,  0.5f,  0.15f, 0.40f, 0.15f,
};

constexpr u16 kCubeIndices[] = {
    0,  1,  2,  0,  2,  3,   // +Z
    4,  5,  6,  4,  6,  7,   // -Z
    8,  9,  10, 8,  10, 11,  // +X
    12, 13, 14, 12, 14, 15,  // -X
    16, 17, 18, 16, 18, 19,  // +Y
    20, 21, 22, 20, 22, 23,  // -Y
};

// Embedded until the ShaderLibrary brick; GLSL 460 — this scene is the GL 4.6
// path (a 4.1 context fails shader compile with a logged error, no crash).
const char* kVertexShader = R"glsl(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(std140, binding = 0) uniform Cube { mat4 uMvp; };
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMvp * vec4(aPos, 1.0);
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

} // namespace

void LandscapeScene::onEnter() {
    rhi::Device& device = engine->getDevice();

    vertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kCubeVertices) },
        kCubeVertices);
    indexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kCubeIndices) },
        kCubeIndices);
    for (u32 i = 0; i < 2; ++i) {
        cubeUbo[i] = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                           .size = sizeof(Mat4),
                                           .dynamic = true },
                                         nullptr);
        cubeBindGroup[i] = device.createBindGroup(
            { .entries = { { .binding = 0, .buffer = cubeUbo[i] } } });
    }

    shader = device.createShader({ .debugName = "landscape-cube",
                                   .vertexSource = kVertexShader,
                                   .fragmentSource = kFragmentShader,
                                   .uniformBlocks = { { "Cube", 0 } } });
    pipeline = device.createPipeline(
        { .shader = shader,
          .vertexBuffers = { { .stride = 6 * sizeof(f32),
                               .attributes = { { .location = 0,
                                                 .format = rhi::VertexFormat::F32x3,
                                                 .offset = 0 },
                                               { .location = 1,
                                                 .format = rhi::VertexFormat::F32x3,
                                                 .offset = 3 * sizeof(f32) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back });
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    device.destroyPipeline(pipeline);
    device.destroyShader(shader);
    for (u32 i = 0; i < 2; ++i) {
        device.destroyBindGroup(cubeBindGroup[i]);
        device.destroyBuffer(cubeUbo[i]);
    }
    device.destroyBuffer(indexBuffer);
    device.destroyBuffer(vertexBuffer);
}

void LandscapeScene::update(f32 dt) {
    angle += dt * 0.6f;
}

void LandscapeScene::render(engine::FrameContext& frame) {
    const Mat4 view = glm::lookAt(Vec3 { 2.6f, 2.0f, 2.6f },
                                  Vec3 { 0.0f, 0.2f, 0.0f },
                                  Vec3 { 0.0f, 1.0f, 0.0f });
    const Mat4 proj =
        glm::perspective(glm::radians(45.0f), frame.aspect, 0.1f, 100.0f);
    const Mat4 viewProj = proj * view;

    // Two intersecting cubes: occlusion between them is only correct with a
    // working depth test, and the camera orbit shows culled backfaces.
    const Mat4 mvp[2] = {
        viewProj * glm::rotate(Mat4 { 1.0f }, angle, Vec3 { 0.0f, 1.0f, 0.0f }),
        viewProj *
            glm::rotate(
                glm::translate(Mat4 { 1.0f }, Vec3 { 0.55f, 0.35f, 0.0f }),
                -angle * 1.3f, Vec3 { 0.3f, 1.0f, 0.2f }),
    };
    frame.device.updateBuffer(cubeUbo[0], &mvp[0], sizeof(Mat4), 0);
    frame.device.updateBuffer(cubeUbo[1], &mvp[1], sizeof(Mat4), 0);

    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = kSkyBlue,
                                .depthLoadOp = rhi::LoadOp::Clear });
    frame.cmd.setPipeline(pipeline);
    frame.cmd.setVertexBuffer(0, vertexBuffer);
    frame.cmd.setIndexBuffer(indexBuffer, rhi::IndexFormat::U16);
    for (u32 i = 0; i < 2; ++i) {
        frame.cmd.setBindGroup(0, cubeBindGroup[i]);
        frame.cmd.drawIndexed(static_cast<u32>(std::size(kCubeIndices)));
    }
    frame.cmd.endRenderPass();
}

void LandscapeScene::drawUi() {
    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Brick 2: RHI depth test + backface culling.");
    ImGui::TextUnformatted("Two rotating cubes must occlude each other.");
    ImGui::End();
}

} // namespace game
