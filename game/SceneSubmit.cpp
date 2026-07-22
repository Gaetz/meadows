
#include "game/SceneSubmit.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/VisualForms.hpp"
#include "engine/ecs/World.hpp"
#include "game/TextureCache.hpp"

namespace game {

render::Sprite spriteFor(const world::Transform& transform,
                         const world::SpriteRender& sprite,
                         rhi::TextureHandle texture) {
    render::Sprite out;
    out.position = { transform.position.x, transform.position.y };
    out.size = { sprite.size.x * transform.scale.x,
                 sprite.size.y * transform.scale.y };
    // Yaw of a quaternion: for a rotation about Z by θ, q = (cos θ/2, 0, 0,
    // sin θ/2), so θ = 2·atan2(z, w). 3D rotations collapse to their yaw in 2D.
    out.rotation = 2.0f * std::atan2(transform.rotation.z, transform.rotation.w);
    out.tint = sprite.tint;
    out.uvRect = sprite.uvRect;
    out.texture = texture;
    return out;
}

RenderSnapshot extractScene(const ecs::World& world, TextureCache& textures) {
    struct Drawable {
        i32 layer;
        render::Sprite sprite;
    };
    vector<Drawable> drawables;

    world.handle()
        .query<const world::Transform, const world::SpriteRender>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::SpriteRender& sprite) {
            drawables.push_back(
                { sprite.layer,
                  spriteFor(transform, sprite, textures.resolve(sprite.sprite)) });
        });

    // Painter's order: lower layers first. Stable so same-layer order stays
    // deterministic (query/handle order).
    std::stable_sort(drawables.begin(), drawables.end(),
                     [](const Drawable& a, const Drawable& b) {
                         return a.layer < b.layer;
                     });

    RenderSnapshot snapshot;
    snapshot.sprites.reserve(drawables.size());
    for (const Drawable& drawable : drawables) {
        snapshot.sprites.push_back(drawable.sprite);
    }

    extractMeshes(world, snapshot);
    return snapshot;
}

// Mesh section: composed world transforms + resource guids only.
// The 3D frontend resolves guids through its residency caches (§7:
// pending assets draw placeholders, never block).
void extractMeshes(const ecs::World& world, RenderSnapshot& out) {
    world.handle()
        .query<const world::Transform, const world::MeshRender>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::MeshRender& mesh) {
            const Mat4 world3d =
                glm::translate(Mat4 { 1.0f }, transform.position) *
                glm::mat4_cast(transform.rotation) *
                glm::scale(Mat4 { 1.0f }, transform.scale);
            out.meshes.push_back({ mesh.model, mesh.material, world3d });
        });
}

vector<SceneLight> collectLights(const ecs::World& world, const Vec3& focus,
                                 u32 maxLights) {
    struct Candidate {
        f32 distanceSq;
        u32 order; // query order — the stable tiebreak
        SceneLight light;
    };
    vector<Candidate> candidates;
    u32 order = 0;
    world.handle()
        .query<const world::Transform, const world::LightSource>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::LightSource& source) {
            const Vec3 d = transform.position - focus;
            candidates.push_back(
                { glm::dot(d, d), order++,
                  { transform.position, source.color, source.intensity,
                    source.radius, source.flicker,
                    transform.rotation * Vec3 { 0.0f, 0.0f, 1.0f },
                    source.spotAngle, source.sunLinked,
                    source.castsShadow } });
        });
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.distanceSq != b.distanceSq
                                    ? a.distanceSq < b.distanceSq
                                    : a.order < b.order;
                     });
    vector<SceneLight> lights;
    lights.reserve(glm::min<size_t>(candidates.size(), maxLights));
    for (const Candidate& candidate : candidates) {
        if (lights.size() >= maxLights) {
            break;
        }
        lights.push_back(candidate.light);
    }
    return lights;
}

void extractLights(const ecs::World& world, const Vec3& focus, u32 maxLights,
                   RenderSnapshot& out) {
    out.lights = collectLights(world, focus, maxLights);
    world.handle()
        .query<const world::Transform, const world::LightSource>()
        .each([&](flecs::entity e, const world::Transform& transform,
                  const world::LightSource& source) {
            const Vec3 forward =
                transform.rotation * Vec3 { 0.0f, 0.0f, 1.0f };
            if (source.castsShadow) {
                out.shadowLights.push_back(
                    { transform.position, source.color, source.intensity,
                      source.radius, source.flicker, forward,
                      source.spotAngle, source.sunLinked, true });
            }
            if (source.shaft) {
                out.shafts.push_back({ e.id(), transform.position, forward,
                                       source.sunLinked, source.color,
                                       source.intensity, source.spotAngle,
                                       source.shaftLength,
                                       source.shaftSoftness,
                                       source.dustDensity });
            }
        });
}

void extractWaterVolumes(const ecs::World& world, RenderSnapshot& out) {
    world.handle()
        .query<const world::Transform, const world::WaterVolume>()
        .each([&](flecs::entity e, const world::Transform& transform,
                  const world::WaterVolume& volume) {
            out.waterVolumes.push_back({ e.id(), transform.position,
                                         volume.halfExtents, volume.tint,
                                         volume.chop });
        });
}

void resolveMeshMaterials(const data::FormDatabase& forms,
                          RenderSnapshot& out) {
    for (RenderSnapshot::MeshInstance& mesh : out.meshes) {
        if (const auto* material =
                forms.find<data::MaterialForm>(mesh.material)) {
            mesh.tint = material->tint;
            mesh.emissive = material->emissive;
            mesh.albedoTexture = material->albedoTexture;
        }
    }
}

void submitSnapshot(const RenderSnapshot& snapshot,
                    render::SpriteRenderer& renderer) {
    for (const render::Sprite& sprite : snapshot.sprites) {
        renderer.draw(sprite);
    }
}

void prewarmTextures(const ecs::World& world, TextureCache& textures) {
    world.handle().query<const world::SpriteRender>().each(
        [&](flecs::entity, const world::SpriteRender& sprite) {
            textures.resolve(sprite.sprite); // first sighting kicks the decode
        });
}

} // namespace game
