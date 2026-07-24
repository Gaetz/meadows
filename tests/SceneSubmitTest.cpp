#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include "game/SceneSubmit.hpp"

// The render bridge's GPU path (texture resolution, submission) needs a live GL
// context, so it is exercised by running the game (brick e). What is pure and
// unit-testable is the component → sprite mapping, including the 2D yaw.

TEST_CASE("scene submit: maps transform + sprite render to a 2D sprite") {
    world::Transform transform;
    transform.position = { 3.0f, 4.0f, 9.0f }; // z ignored in 2D
    transform.scale = { 2.0f, 3.0f, 1.0f };

    world::SpriteRender sprite;
    sprite.size = { 1.5f, 2.0f };
    sprite.tint = { 0.5f, 0.6f, 0.7f, 1.0f };

    const rhi::TextureHandle texture { 42 };
    const auto out = game::spriteFor(transform, sprite, texture);

    CHECK(out.position.x == 3.0f);
    CHECK(out.position.y == 4.0f);
    CHECK(out.size.x == doctest::Approx(3.0f)); // size.x * scale.x = 1.5 * 2
    CHECK(out.size.y == doctest::Approx(6.0f)); // size.y * scale.y = 2.0 * 3
    CHECK(out.tint.x == 0.5f);
    CHECK(out.tint.w == 1.0f);
    CHECK(out.texture.id == 42);
    CHECK(out.rotation == doctest::Approx(0.0f)); // identity quaternion
}

TEST_CASE("scene submit: 2D rotation is the yaw of the 3D quaternion") {
    world::Transform transform;
    const f32 angle = 1.2f;
    transform.rotation = glm::angleAxis(angle, Vec3 { 0.0f, 0.0f, 1.0f });

    const auto out = game::spriteFor(transform, world::SpriteRender {}, {});
    CHECK(out.rotation == doctest::Approx(angle));
}

// --- The headless halves of the extract ---------------------------------------------
// extractMeshes and collectLights are the snapshot's 3D feeders — pure guids
// and PODs, no GPU — locked headless while LandscapeScene::render() is
// rebuilt onto them.

#include "engine/ecs/World.hpp"
#include "world/scene/AnimBridge.hpp" // registerSceneComponents

using core::Guid;

TEST_CASE("scene submit: extractMeshes composes world transforms from guids") {
    ecs::World world;
    world::registerSceneComponents(world);

    const Guid model = *Guid::fromString("aaaa0000-0000-4000-8000-000000000001");
    const Guid material =
        *Guid::fromString("aaaa0000-0000-4000-8000-000000000002");

    ecs::Entity e = world.create();
    world::Transform transform;
    transform.position = { 10.0f, 2.0f, -3.0f };
    transform.scale = { 2.0f, 2.0f, 2.0f };
    e.set<world::Transform>(transform);
    e.set<world::MeshRender>({ model, material });

    // An entity without MeshRender must not extract.
    ecs::Entity bare = world.create();
    bare.set<world::Transform>({});

    game::RenderSnapshot snapshot;
    game::extractMeshes(world, snapshot);

    REQUIRE(snapshot.meshes.size() == 1);
    CHECK(snapshot.meshes[0].model == model);
    CHECK(snapshot.meshes[0].material == material);
    const Mat4& m = snapshot.meshes[0].transform;
    CHECK(m[3].x == doctest::Approx(10.0f)); // translation column
    CHECK(m[3].y == doctest::Approx(2.0f));
    CHECK(m[0].x == doctest::Approx(2.0f)); // scale on the basis vectors
}

TEST_CASE("scene submit: collectLights returns the nearest lights first") {
    ecs::World world;
    world::registerSceneComponents(world);

    const auto placeLight = [&](f32 x, f32 intensity) {
        ecs::Entity e = world.create();
        world::Transform transform;
        transform.position = { x, 0.0f, 0.0f };
        e.set<world::Transform>(transform);
        world::LightSource light;
        light.intensity = intensity;
        e.set<world::LightSource>(light);
    };
    placeLight(50.0f, 3.0f); // far
    placeLight(5.0f, 1.0f);  // nearest
    placeLight(20.0f, 2.0f); // middle

    const auto lights =
        game::collectLights(world, Vec3 { 0.0f, 0.0f, 0.0f }, 2);
    REQUIRE(lights.size() == 2); // capped at maxLights
    CHECK(lights[0].position.x == doctest::Approx(5.0f));
    CHECK(lights[0].intensity == doctest::Approx(1.0f));
    CHECK(lights[1].position.x == doctest::Approx(20.0f));
}

// --- The extract extensions ------------------------------------------------------------
// render() consumes ONLY these packet sections instead of querying the live
// World: lights UBO, key-shadow candidates, water volumes and
// resolved mesh materials.

TEST_CASE("scene submit: extractLights splits UBO / shadow roles") {
    ecs::World world;
    world::registerSceneComponents(world);

    // A plain light, a shadow caster, and a sun-linked window light.
    const auto place = [&](f32 x, auto&& configure) {
        ecs::Entity e = world.create();
        world::Transform transform;
        transform.position = { x, 0.0f, 0.0f };
        e.set<world::Transform>(transform);
        world::LightSource light;
        configure(light);
        e.set<world::LightSource>(light);
    };
    place(1.0f, [](world::LightSource&) {});
    place(2.0f, [](world::LightSource& l) {
        l.castsShadow = true;
        l.spotAngle = 60.0f;
        l.radius = 12.0f;
    });
    place(3.0f, [](world::LightSource& l) {
        l.sunLinked = true;
        l.windowHalfWidth = 0.5f;
    });

    game::RenderSnapshot snapshot;
    game::extractLights(world, Vec3 { 0.0f, 0.0f, 0.0f }, 16, snapshot);

    CHECK(snapshot.lights.size() == 3); // every light feeds the UBO pick
    REQUIRE(snapshot.shadowLights.size() == 1);
    CHECK(snapshot.shadowLights[0].position.x == doctest::Approx(2.0f));
    CHECK(snapshot.shadowLights[0].spotAngle == doctest::Approx(60.0f));
    CHECK(snapshot.shadowLights[0].castsShadow);
    CHECK(snapshot.lights[2].sunLinked);
    CHECK(snapshot.lights[2].windowHalfWidth == doctest::Approx(0.5f));
}

TEST_CASE("scene submit: extractWaterVolumes carries the submersion inputs") {
    ecs::World world;
    world::registerSceneComponents(world);

    ecs::Entity e = world.create();
    world::Transform transform;
    transform.position = { 4.0f, 10.0f, -2.0f };
    e.set<world::Transform>(transform);
    world::WaterVolume volume;
    volume.halfExtents = { 6.0f, 1.5f, 6.0f };
    volume.chop = 0.8f;
    e.set<world::WaterVolume>(volume);

    game::RenderSnapshot snapshot;
    game::extractWaterVolumes(world, snapshot);
    REQUIRE(snapshot.waterVolumes.size() == 1);
    CHECK(snapshot.waterVolumes[0].position.y == doctest::Approx(10.0f));
    CHECK(snapshot.waterVolumes[0].halfExtents.y == doctest::Approx(1.5f));
    CHECK(snapshot.waterVolumes[0].chop == doctest::Approx(0.8f));
    CHECK(snapshot.waterVolumes[0].entityId == e.id());
}

#include "data/forms/FormDatabase.hpp"
#include "data/forms/VisualForms.hpp"

TEST_CASE("scene submit: resolveMeshMaterials folds material fields in") {
    data::FormDatabase forms;
    auto material = std::make_unique<data::MaterialForm>();
    material->id = *Guid::fromString("aaaa0000-0000-4000-8000-0000000000aa");
    material->tint = { 0.2f, 0.4f, 0.6f, 1.0f };
    material->emissive = 2.5f;
    material->albedoTexture =
        *Guid::fromString("aaaa0000-0000-4000-8000-0000000000bb");
    const Guid materialId = material->id;
    const Guid albedo = material->albedoTexture;
    forms.add(std::move(material), data::MaterialForm::staticTypeInfo());

    game::RenderSnapshot snapshot;
    snapshot.meshes.push_back({ Guid {}, materialId, Mat4 { 1.0f } });
    snapshot.meshes.push_back({ Guid {}, Guid {}, Mat4 { 1.0f } }); // no mat

    game::resolveMeshMaterials(forms, snapshot);
    CHECK(snapshot.meshes[0].tint.x == doctest::Approx(0.2f));
    CHECK(snapshot.meshes[0].emissive == doctest::Approx(2.5f));
    CHECK(snapshot.meshes[0].albedoTexture == albedo);
    // The no-material fallback keeps the plain-tint defaults.
    CHECK(snapshot.meshes[1].tint.x == doctest::Approx(1.0f));
    CHECK(snapshot.meshes[1].emissive == doctest::Approx(0.0f));
    CHECK_FALSE(snapshot.meshes[1].albedoTexture.isValid());
}
