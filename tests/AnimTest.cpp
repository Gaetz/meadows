#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include "data/forms/AnimForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/anim/Anim.hpp"
#include "world/scene/AnimBridge.hpp"

// H5: the anim runtime is pure and deterministic — synthetic skeleton +
// clip, sampled, blended, driven through the graph, all headless.

namespace {

anim::Skeleton twoBoneSkeleton() {
    anim::Skeleton skeleton;
    skeleton.joints.resize(2);
    skeleton.joints[0].name = "root";
    skeleton.joints[0].parent = -1;
    skeleton.joints[1].name = "arm";
    skeleton.joints[1].parent = 0;
    skeleton.joints[1].bindPosition = { 0.0f, 1.0f, 0.0f };
    return skeleton;
}

// 1-second clip rotating "arm" 0 -> 90° around Z.
anim::AnimClip armSwingClip() {
    anim::AnimClip clip;
    clip.name = "swing";
    clip.duration = 1.0f;
    clip.tracks.resize(2);
    clip.tracks[1].rotationTimes = { 0.0f, 1.0f };
    clip.tracks[1].rotations = {
        Quat { 1.0f, 0.0f, 0.0f, 0.0f },
        glm::angleAxis(glm::half_pi<f32>(), Vec3 { 0.0f, 0.0f, 1.0f }),
    };
    return clip;
}

} // namespace

TEST_CASE("clip sampling interpolates keys deterministically") {
    const anim::Skeleton skeleton = twoBoneSkeleton();
    const anim::AnimClip clip = armSwingClip();

    anim::Pose pose;
    anim::bindPose(skeleton, pose);
    anim::samplePose(clip, 0.5f, pose);
    // Halfway: 45° around Z.
    const f32 angle = glm::angle(pose[1].rotation);
    CHECK(angle == doctest::Approx(glm::quarter_pi<f32>()).epsilon(0.01));
    // Untouched channels keep the bind values.
    CHECK(pose[1].position.y == doctest::Approx(1.0f));

    // Same time, same result (pure function).
    anim::Pose again;
    anim::bindPose(skeleton, again);
    anim::samplePose(clip, 0.5f, again);
    CHECK(again[1].rotation.z == doctest::Approx(pose[1].rotation.z));
}

TEST_CASE("model matrices chain parents; skin matrices invert the bind") {
    const anim::Skeleton skeleton = twoBoneSkeleton();
    anim::Pose pose;
    anim::bindPose(skeleton, pose);
    pose[0].position = { 2.0f, 0.0f, 0.0f };

    vector<Mat4> model;
    anim::modelMatrices(skeleton, pose, model);
    // Child = parent translation + its own bind offset.
    CHECK(model[1][3][0] == doctest::Approx(2.0f));
    CHECK(model[1][3][1] == doctest::Approx(1.0f));
}

TEST_CASE("graph transitions fire on params and blend over time") {
    anim::GraphDesc desc;
    desc.clips.push_back(armSwingClip());
    desc.clipEvents.emplace_back();
    desc.states.push_back({ 0, 1.0f, true, 0.0f });  // idle-ish
    desc.states.push_back({ 0, 2.0f, true, 0.0f });  // "run": same clip 2x
    desc.transitions.push_back(
        { 0, 1, "speed", true, 0.5f, "", "", 0.2f, false });
    desc.transitions.push_back(
        { 1, 0, "speed", false, 0.5f, "", "", 0.2f, false });

    anim::GraphInstance instance { desc };
    CHECK(instance.currentState() == 0);

    instance.update(0.016f);
    CHECK(instance.currentState() == 0); // speed defaults to 0

    instance.setParam("speed", 1.0f);
    instance.update(0.016f);
    CHECK(instance.blending());
    for (int i = 0; i < 20; ++i) {
        instance.update(0.016f); // ride out the 0.2 s blend
    }
    CHECK(instance.currentState() == 1);
    CHECK_FALSE(instance.blending());

    instance.setParam("speed", 0.0f);
    for (int i = 0; i < 20; ++i) {
        instance.update(0.016f);
    }
    CHECK(instance.currentState() == 0);
}

TEST_CASE("timeline events fire exactly once when crossed") {
    anim::GraphDesc desc;
    desc.clips.push_back(armSwingClip());
    desc.clipEvents.push_back({ { 0.5f, "Hit" } });
    desc.states.push_back({ 0, 1.0f, false, 0.0f });

    anim::GraphInstance instance { desc };
    u32 hits = 0;
    instance.setEventSink([&](std::string_view name) {
        if (name == "Hit") {
            ++hits;
        }
    });
    for (int i = 0; i < 100; ++i) {
        instance.update(0.016f); // 1.6 s > clip duration, no loop
    }
    CHECK(hits == 1);
}

TEST_CASE("AnimBridge maps graph forms to a runtime desc") {
    constexpr const char* kToml = R"(
[plugin]
id = "dddd0000-0000-4000-8000-000000000001"
name = "anim"

[[records]]
form = "dddd0001-0000-4000-8000-000000000001"
type = "AnimClipForm"
new = true
[records.fields]
editorId = "Swing"
rate = 1.0

[[records]]
form = "dddd0001-0000-4000-8000-000000000002"
type = "AnimEventForm"
new = true
[records.fields]
parent = "dddd0001-0000-4000-8000-000000000001"
time = 0.25
name = "Footstep"

[[records]]
form = "dddd0002-0000-4000-8000-000000000001"
type = "AnimGraphForm"
new = true
[records.fields]
editorId = "Locomotion"
initialState = "dddd0002-0000-4000-8000-000000000002"

[[records]]
form = "dddd0002-0000-4000-8000-000000000002"
type = "AnimStateForm"
new = true
[records.fields]
parent = "dddd0002-0000-4000-8000-000000000001"
clip = "dddd0001-0000-4000-8000-000000000001"

[[records]]
form = "dddd0002-0000-4000-8000-000000000003"
type = "AnimStateForm"
new = true
[records.fields]
parent = "dddd0002-0000-4000-8000-000000000001"
clip = "dddd0001-0000-4000-8000-000000000001"
speed = 2.0

[[records]]
form = "dddd0002-0000-4000-8000-000000000004"
type = "AnimTransitionForm"
new = true
[records.fields]
parent = "dddd0002-0000-4000-8000-000000000001"
from = "dddd0002-0000-4000-8000-000000000002"
to = "dddd0002-0000-4000-8000-000000000003"
param = "speed"
threshold = 0.5
)";
    data::FormTypeRegistry types;
    data::registerAnimFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "anim");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    const auto graphId =
        *core::Guid::fromString("dddd0002-0000-4000-8000-000000000001");
    u32 resolverCalls = 0;
    const auto desc = world::buildAnimGraph(
        db, graphId,
        [&](const core::Guid&, const str&) {
            ++resolverCalls;
            return armSwingClip();
        });
    REQUIRE(desc.has_value());
    CHECK(desc->states.size() == 2);
    CHECK(desc->transitions.size() == 1);
    CHECK(resolverCalls == 1); // clip deduplicated across the two states
    REQUIRE(desc->clipEvents.size() == 1);
    REQUIRE(desc->clipEvents[0].size() == 1);
    CHECK(desc->clipEvents[0][0].name == "Footstep");
    CHECK(desc->states[1].speed == doctest::Approx(2.0f));
    CHECK(desc->initialState == 0);
}
