#include "game/ui/AnimPreviewPanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "data/forms/AnimForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "engine/FrameContext.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/Log.hpp"
#include "engine/reflect/Visit.hpp"
#include "engine/rhi/Device.hpp"
#include "world/scene/AnimBridge.hpp"

namespace game {

namespace {

constexpr u32 kStageSide = 640; // fixed square target; fit-scaled in ImGui

// Minimal preview shading: skinned vertex path identical to skinned.vert,
// flat N.L in the fragment — the tool judges MOTION, the game judges look
// (feedback rule: never restyle the game's rendering from a tool).
constexpr const char* kPreviewVert = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(std140, binding = 1) uniform PreviewUbo {
    mat4 uViewProj;
    mat4 uModel;
    vec4 uTint;
};

layout(std430, binding = 2) readonly buffer BonePalette {
    mat4 uBones[];
};

out vec3 vNormal;
out vec3 vColor;

void main() {
    mat4 skin = aWeights.x * uBones[int(aJoints.x)] +
                aWeights.y * uBones[int(aJoints.y)] +
                aWeights.z * uBones[int(aJoints.z)] +
                aWeights.w * uBones[int(aJoints.w)];
    const vec4 world = uModel * skin * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * mat3(skin) * aNormal;
    vColor = aColor * uTint.rgb;
    gl_Position = uViewProj * world;
}
)";

constexpr const char* kPreviewFrag = R"(#version 460 core
in vec3 vNormal;
in vec3 vColor;
out vec4 fragColor;

void main() {
    const vec3 n = normalize(vNormal);
    const float diffuse = max(dot(n, normalize(vec3(0.45, 0.8, 0.35))), 0.0);
    fragColor = vec4(vColor * (0.35 + 0.7 * diffuse), 1.0);
}
)";

u64 hashCombine(u64 h, u64 v) {
    return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

u64 floatBits(f32 x) {
    u32 bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    return bits;
}

// Field-value hash: the graph preview rebuilds when ANY reflected field of
// the graph's records changes (the FxPanel "edits apply live" contract,
// without re-copying clips every frame).
u64 hashValue(const reflect::Value& value) {
    return reflect::visit(value, reflect::overloaded {
        [](bool b) -> u64 { return b ? 1u : 2u; },
        [](i32 x) -> u64 { return static_cast<u64>(static_cast<u32>(x)); },
        [](u32 x) -> u64 { return x; },
        [](f32 x) -> u64 { return floatBits(x); },
        [](f64 x) -> u64 {
            u64 bits = 0;
            std::memcpy(&bits, &x, sizeof(bits));
            return bits;
        },
        [](const str& s) -> u64 { return std::hash<str> {}(s); },
        [](const Vec2& v) -> u64 {
            return hashCombine(floatBits(v.x), floatBits(v.y));
        },
        [](const Vec3& v) -> u64 {
            return hashCombine(hashCombine(floatBits(v.x), floatBits(v.y)),
                               floatBits(v.z));
        },
        [](const Vec4& v) -> u64 {
            return hashCombine(
                hashCombine(hashCombine(floatBits(v.x), floatBits(v.y)),
                            floatBits(v.z)),
                floatBits(v.w));
        },
        [](const Quat& q) -> u64 {
            return hashCombine(
                hashCombine(hashCombine(floatBits(q.x), floatBits(q.y)),
                            floatBits(q.z)),
                floatBits(q.w));
        },
        [](const core::Guid& g) -> u64 { return std::hash<core::Guid> {}(g); },
    });
}

u64 hashForm(const data::Form& form, const reflect::TypeInfo& type) {
    u64 h = std::hash<core::Guid> {}(form.id);
    reflect::forEachField(type, [&](const reflect::FieldInfo& field) {
        h = hashCombine(h, hashValue(field.get(&form)));
    });
    return h;
}

struct PreviewUniforms {
    Mat4 viewProj { 1.0f };
    Mat4 model { 1.0f };
    Vec4 tint { 1.0f };
};

} // namespace

const AnimPreviewPanel::Rig* AnimPreviewPanel::loadRig(
    const core::Guid& asset) {
    if (!asset.isValid()) {
        return nullptr;
    }
    if (const auto it = rigs.find(asset); it != rigs.end()) {
        return it->second.valid ? &it->second : nullptr;
    }
    Rig& rig = rigs[asset]; // empty entry = negative cache
    const auto path = assetDb.resolve(asset);
    if (!path) {
        LOG_WARN("anim preview: no asset registered for rig {}",
                 asset.toString());
        return nullptr;
    }
    auto loaded = assets::loadGltfSkeleton(*path);
    if (!loaded) {
        return nullptr;
    }
    rig.skeleton = std::move(*loaded);
    rig.clips = assets::loadGltfAnimations(*path, rig.skeleton);
    rig.valid = true;
    return &rig;
}

bool AnimPreviewPanel::ensureMesh(const core::Guid& skeletonAsset) {
    if (meshFor == skeletonAsset && meshTried) {
        return meshValid;
    }
    meshFor = skeletonAsset;
    meshTried = true;
    meshValid = false;
    indexCount = 0;
    bindGroup.reset(); // rebuilt in render() (palette may resize with the rig)
    vertices.reset();
    indices.reset();
    tint = Vec4 { 1.0f };
    meshSource.clear();

    // 1) the rig's own glTF may carry the skinned body already.
    std::optional<render::SkinnedMeshData> mesh;
    if (const auto path = assetDb.resolve(skeletonAsset)) {
        mesh = assets::loadGltfSkinnedMesh(*path);
        meshSource = "mesh: clip asset";
    }
    // 2) else borrow the body of the first actor riding this skeleton.
    if (!mesh || mesh->vertices.empty()) {
        mesh.reset();
        data::forEach<data::ActorForm>(db, [&](const data::ActorForm& actor) {
            if (mesh) {
                return;
            }
            const auto visual = world::resolveActorVisual(db, actor);
            if (!visual || visual->skeleton != skeletonAsset) {
                return;
            }
            const auto meshPath = assetDb.resolve(visual->mesh);
            auto skinned = meshPath ? assets::loadGltfSkinnedMesh(*meshPath)
                                    : std::nullopt;
            if (skinned && !skinned->vertices.empty()) {
                mesh = std::move(skinned);
                tint = visual->tint;
                meshSource = "mesh: actor '" + actor.editorId + "'";
            }
        });
    }
    if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) {
        return false;
    }

    Vec3 lo { FLT_MAX };
    Vec3 hi { -FLT_MAX };
    for (const render::SkinnedVertex& vertex : mesh->vertices) {
        lo = glm::min(lo, vertex.position);
        hi = glm::max(hi, vertex.position);
    }
    boundsCenter = 0.5f * (lo + hi);
    boundsRadius = std::max(glm::length(hi - lo) * 0.5f, 0.25f);

    vertices = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh->vertices.size() * sizeof(render::SkinnedVertex) },
        mesh->vertices.data()) };
    indices = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh->indices.size() * sizeof(u32) },
        mesh->indices.data()) };
    indexCount = static_cast<u32>(mesh->indices.size());
    meshValid = true;
    return true;
}

void AnimPreviewPanel::drawEditor(const core::Guid& itemSelected) {
    wantRender = false;
    const reflect::TypeInfo* type =
        itemSelected.isValid() ? session.viewType(itemSelected) : nullptr;
    const bool isClip =
        type && type->isA(data::AnimClipForm::staticTypeInfo().id);
    const bool isGraph =
        type && type->isA(data::AnimGraphForm::staticTypeInfo().id);
    if (!isClip && !isGraph) {
        ImGui::TextDisabled(
            "(select an anim clip or an anim graph in the Browser)");
        return;
    }
    if (shown != itemSelected) {
        shown = itemSelected;
        time = 0.0f;
        playing = true;
        graphInstance.reset();
        graphDesc.reset();
        graphFp = 0;
        eventLog.clear();
    }
    if (isClip) {
        drawClip(itemSelected);
    } else {
        drawGraph(itemSelected);
    }
}

void AnimPreviewPanel::drawClip(const core::Guid& clipId) {
    const auto* form =
        static_cast<const data::AnimClipForm*>(session.view(clipId));
    const Rig* rig = loadRig(form->asset);
    if (!rig) {
        ImGui::TextDisabled(
            "(clip asset not loadable — point 'asset' at a rigged glTF)");
        return;
    }
    const anim::AnimClip* clip = nullptr;
    for (const assets::GltfClip& candidate : rig->clips) {
        if (form->animationName.empty() ||
            candidate.name == form->animationName) {
            clip = &candidate.clip;
            break;
        }
    }
    if (!clip) {
        ImGui::TextDisabled("(no animation '%s' in the asset — %d available)",
                            form->animationName.c_str(),
                            static_cast<int>(rig->clips.size()));
        return;
    }
    skeleton = &rig->skeleton;
    ensureMesh(form->asset);

    if (ImGui::Button(playing ? "Pause" : "Play")) {
        playing = !playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        time = 0.0f;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##speed", &timeScale, 0.0f, 2.0f, "%.2fx");
    const f32 duration = std::max(clip->duration, 0.0001f);
    if (playing) {
        time += ImGui::GetIO().DeltaTime * timeScale * form->rate;
    }
    if (form->loop) {
        time = std::fmod(time, duration);
    } else {
        time = std::min(time, duration);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-80.0f);
    ImGui::SliderFloat("##scrub", &time, 0.0f, duration, "%.2fs");
    ImGui::SameLine();
    ImGui::TextDisabled("%.2fs", duration);

    // The clip's authored events, highlighted as the playhead crosses them.
    vector<std::pair<f32, str>> events;
    session.forEachVisible([&](const core::Guid&, const data::Form& child,
                               const reflect::TypeInfo& childType) {
        if (!childType.isA(data::AnimEventForm::staticTypeInfo().id)) {
            return;
        }
        const auto& event = static_cast<const data::AnimEventForm&>(child);
        if (event.parent == clipId) {
            events.emplace_back(event.time, event.name);
        }
    });
    std::sort(events.begin(), events.end());
    if (!events.empty()) {
        ImGui::TextDisabled("events:");
        for (const auto& [at, name] : events) {
            ImGui::SameLine();
            if (std::fabs(time - at) < 0.07f) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                                   "%.2f %s", at, name.c_str());
            } else {
                ImGui::TextDisabled("%.2f %s", at, name.c_str());
            }
        }
    }
    ImGui::TextDisabled("%s — %s",
                        clip->name.empty() ? "(first animation)"
                                           : clip->name.c_str(),
                        meshSource.empty() ? "no mesh" : meshSource.c_str());

    anim::bindPose(rig->skeleton, pose);
    anim::samplePose(*clip, time, pose);
    anim::skinMatrices(rig->skeleton, pose, palette);
    drawStage();
}

u64 AnimPreviewPanel::graphFingerprint(const core::Guid& graphId) const {
    // XOR-combined per-form hashes: order-independent, so session-created
    // drafts (unordered) can't flap the fingerprint.
    u64 h = 0;
    std::set<core::Guid> clipIds;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        bool relevant = id == graphId;
        if (type.isA(data::AnimStateForm::staticTypeInfo().id)) {
            const auto& state = static_cast<const data::AnimStateForm&>(form);
            if (state.parent == graphId) {
                relevant = true;
                clipIds.insert(state.clip);
            }
        } else if (type.isA(data::AnimTransitionForm::staticTypeInfo().id)) {
            relevant = static_cast<const data::AnimTransitionForm&>(form)
                           .parent == graphId;
        }
        if (relevant) {
            h ^= hashForm(form, type);
        }
    });
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (type.isA(data::AnimClipForm::staticTypeInfo().id)) {
            if (clipIds.contains(id)) {
                h ^= hashForm(form, type);
            }
        } else if (type.isA(data::AnimEventForm::staticTypeInfo().id)) {
            const auto& event = static_cast<const data::AnimEventForm&>(form);
            if (clipIds.contains(event.parent)) {
                h ^= hashForm(form, type);
            }
        }
    });
    return h != 0 ? h : 1; // 0 is the "no graph built" sentinel
}

bool AnimPreviewPanel::rebuildGraph(const core::Guid& graphId) {
    graphInstance.reset();
    graphDesc.reset();
    graphSkeleton = {};
    paramNames.clear();
    tagNames.clear();
    eventLog.clear();
    auto desc = world::buildAnimGraph(
        session, graphId,
        [this](const core::Guid& asset,
               const str& name) -> std::optional<anim::AnimClip> {
            const Rig* rig = loadRig(asset);
            if (!rig) {
                return std::nullopt;
            }
            if (!graphSkeleton.isValid()) {
                graphSkeleton = asset;
            }
            for (const assets::GltfClip& clip : rig->clips) {
                if (name.empty() || clip.name == name) {
                    return clip.clip;
                }
            }
            return std::nullopt;
        });
    if (!desc) {
        return false;
    }
    graphDesc = std::make_unique<anim::GraphDesc>(std::move(*desc));
    for (const anim::GraphTransition& transition : graphDesc->transitions) {
        if (!transition.param.empty() &&
            std::find(paramNames.begin(), paramNames.end(),
                      transition.param) == paramNames.end()) {
            paramNames.push_back(transition.param);
        }
        for (const str* tag :
             { &transition.requiredTag, &transition.blockedTag }) {
            if (!tag->empty() && std::find(tagNames.begin(), tagNames.end(),
                                           *tag) == tagNames.end()) {
                tagNames.push_back(*tag);
            }
        }
    }
    for (const str& name : paramNames) {
        paramValues.try_emplace(name, 0.0f);
    }
    graphInstance = std::make_unique<anim::GraphInstance>(*graphDesc);
    graphInstance->setTagCheck([this](std::string_view tag) {
        return activeTags.contains(str { tag });
    });
    graphInstance->setEventSink([this](std::string_view name) {
        eventLog.emplace_front(name);
        if (eventLog.size() > 6) {
            eventLog.pop_back();
        }
    });
    return true;
}

void AnimPreviewPanel::drawGraph(const core::Guid& graphId) {
    const u64 fp = graphFingerprint(graphId);
    if (!graphInstance || fp != graphFp) {
        graphFp = fp;
        rebuildGraph(graphId);
    }
    if (!graphInstance) {
        ImGui::TextDisabled(
            "(graph has no usable states — check state clips and assets)");
        return;
    }
    const Rig* rig = loadRig(graphSkeleton);
    if (!rig) {
        ImGui::TextDisabled("(graph clips have no loadable rig)");
        return;
    }
    skeleton = &rig->skeleton;
    ensureMesh(graphSkeleton);

    if (ImGui::Button(playing ? "Pause" : "Play")) {
        playing = !playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        graphInstance = std::make_unique<anim::GraphInstance>(*graphDesc);
        graphInstance->setTagCheck([this](std::string_view tag) {
            return activeTags.contains(str { tag });
        });
        graphInstance->setEventSink([this](std::string_view name) {
            eventLog.emplace_front(name);
            if (eventLog.size() > 6) {
                eventLog.pop_back();
            }
        });
        eventLog.clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##speed", &timeScale, 0.0f, 2.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat("entity speed", &entitySpeed, 0.05f, 0.0f, 12.0f,
                     "%.2f m/s");

    // The graph's own control surface: every float param and tag gate its
    // transitions mention becomes a widget.
    for (const str& name : paramNames) {
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat(name.c_str(), &paramValues[name], 0.05f, -20.0f,
                         20.0f, "%.2f");
    }
    for (const str& tag : tagNames) {
        bool on = activeTags.contains(tag);
        if (ImGui::Checkbox(tag.c_str(), &on)) {
            if (on) {
                activeTags.insert(tag);
            } else {
                activeTags.erase(tag);
            }
        }
        ImGui::SameLine();
    }
    if (!tagNames.empty()) {
        ImGui::NewLine();
    }

    if (playing) {
        for (const str& name : paramNames) {
            graphInstance->setParam(name, paramValues[name]);
        }
        graphInstance->update(ImGui::GetIO().DeltaTime * timeScale,
                              entitySpeed);
    }

    const anim::GraphState& state =
        graphDesc->states[graphInstance->currentState()];
    const str& clipName = graphDesc->clips[state.clip].name;
    str events;
    for (const str& name : eventLog) {
        events += events.empty() ? name : ", " + name;
    }
    ImGui::TextDisabled("state: %s%s%s%s",
                        clipName.empty() ? "(unnamed clip)" : clipName.c_str(),
                        graphInstance->blending() ? " (blending)" : "",
                        events.empty() ? "" : " — events: ",
                        events.c_str());

    anim::bindPose(rig->skeleton, pose);
    graphInstance->evaluate(pose);
    anim::skinMatrices(rig->skeleton, pose, palette);
    drawStage();
}

void AnimPreviewPanel::drawStage() {
    if (unsupported) {
        ImGui::TextDisabled(
            "(offscreen targets unsupported on this backend)");
        return;
    }
    if (!meshValid) {
        ImGui::TextDisabled(
            "(no skinned mesh found: neither the clip's glTF nor any actor "
            "rides this skeleton)");
        return;
    }
    wantRender = skeleton != nullptr && !palette.empty() && indexCount > 0;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 side =
        std::clamp(std::min(avail.x, avail.y), 160.0f, 720.0f);
    const u64 native =
        colorTex ? device.nativeTextureId(colorTex.get()) : 0;
    if (native == 0) {
        ImGui::TextDisabled("(warming up)"); // first frame: pass not recorded yet
        return;
    }
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("anim-stage", ImVec2(side, side));
    // GL renders bottom-up: flip V so the character stands upright.
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(native), corner,
        ImVec2(corner.x + side, corner.y + side), ImVec2(0.0f, 1.0f),
        ImVec2(1.0f, 0.0f));
    if (ImGui::IsItemHovered()) {
        const f32 wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            zoom = std::clamp(zoom * std::pow(0.92f, wheel), 1.2f, 8.0f);
        }
    }
    if (ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        yaw -= delta.x * 0.01f;
        pitch = std::clamp(pitch + delta.y * 0.01f, -1.45f, 1.45f);
    }
}

void AnimPreviewPanel::render(engine::FrameContext& frame) {
    if (!wantRender) {
        return;
    }
    wantRender = false;
    if (!frame.device.caps().offscreenTargets) {
        unsupported = true;
        return;
    }
    if (!colorTex) {
        colorTex = { device, device.createTexture(
            { .width = kStageSide,
              .height = kStageSide,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr) };
        depthTex = { device, device.createTexture(
            { .width = kStageSide,
              .height = kStageSide,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_RenderAttachment },
            nullptr) };
        framebuffer = { device, device.createFramebuffer(
            { .colorAttachments = { { .texture = colorTex } },
              .depthAttachment = { .texture = depthTex } }) };
        shader = { device, device.createShader(
            { .debugName = "anim-preview",
              .vertexSource = kPreviewVert,
              .fragmentSource = kPreviewFrag,
              .uniformBlocks = { { "PreviewUbo", 1 } } }) };
        pipeline = { device, device.createPipeline(
            { .shader = shader,
              .vertexBuffers =
                  { { .stride = sizeof(render::SkinnedVertex),
                      .attributes =
                          { { .location = 0,
                              .format = rhi::VertexFormat::F32x3,
                              .offset = offsetof(render::SkinnedVertex,
                                                 position) },
                            { .location = 1,
                              .format = rhi::VertexFormat::F32x3,
                              .offset =
                                  offsetof(render::SkinnedVertex, normal) },
                            { .location = 2,
                              .format = rhi::VertexFormat::F32x2,
                              .offset = offsetof(render::SkinnedVertex, uv) },
                            { .location = 3,
                              .format = rhi::VertexFormat::F32x3,
                              .offset =
                                  offsetof(render::SkinnedVertex, color) },
                            { .location = 4,
                              .format = rhi::VertexFormat::F32x4,
                              .offset =
                                  offsetof(render::SkinnedVertex, joints) },
                            { .location = 5,
                              .format = rhi::VertexFormat::F32x4,
                              .offset = offsetof(render::SkinnedVertex,
                                                 weights) } } } },
              .depth = { .testEnable = true,
                         .writeEnable = true,
                         .compare = rhi::CompareFunc::Less },
              .cull = rhi::CullMode::Back }) };
        ubo = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Uniform,
              .size = sizeof(PreviewUniforms),
              .dynamic = true },
            nullptr) };
    }
    if (paletteCapacity < palette.size()) {
        bindGroup.reset();
        paletteSsbo = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Storage,
              .size = palette.size() * sizeof(Mat4),
              .dynamic = true },
            nullptr) };
        paletteCapacity = static_cast<u32>(palette.size());
    }
    if (!bindGroup) {
        bindGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 1, .buffer = ubo },
                           { .binding = 2,
                             .buffer = paletteSsbo,
                             .storage = true } } }) };
    }

    const f32 radius = std::max(boundsRadius, 0.25f);
    const f32 dist = radius * zoom;
    const Vec3 direction { std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                           std::cos(pitch) * std::cos(yaw) };
    const Vec3 eye = boundsCenter + direction * dist;
    const Mat4 view = glm::lookAt(eye, boundsCenter, Vec3 { 0.0f, 1.0f, 0.0f });
    const Mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.05f,
                                       dist + radius * 6.0f);
    PreviewUniforms uniforms;
    uniforms.viewProj = proj * view;
    uniforms.tint = tint;
    frame.device.updateBuffer(ubo, &uniforms, sizeof(uniforms), 0);
    frame.device.updateBuffer(paletteSsbo, palette.data(),
                              palette.size() * sizeof(Mat4), 0);

    frame.cmd.beginRenderPass(
        { .framebuffer = framebuffer,
          .loadOp = rhi::LoadOp::Clear,
          .clearColor = { 0.13f, 0.14f, 0.17f, 1.0f } });
    frame.cmd.setPipeline(pipeline);
    frame.cmd.setBindGroup(0, bindGroup);
    frame.cmd.setVertexBuffer(0, vertices);
    frame.cmd.setIndexBuffer(indices, rhi::IndexFormat::U32);
    frame.cmd.drawIndexed(indexCount);
    frame.cmd.endRenderPass();
}

} // namespace game
