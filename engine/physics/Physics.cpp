#include "engine/physics/Physics.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "engine/core/Log.hpp"

// Jolt boilerplate kept to the minimum viable setup. Deliberate v1 choices
// (documented for the verticals):
//  - JobSystemSingleThreaded: deterministic and headless-friendly; swap to
//    a JobSystem bridged onto core::JobSystem when profiling asks (§8: the
//    swap must not change results — Jolt is deterministic per thread count
//    only with the single-threaded system, another reason to keep it).
//  - Two broadphase layers (STATIC / MOVING), the canonical Jolt example
//    mapping. Extend ObjectLayers when triggers/ragdolls arrive.
//  - Fixed 60 Hz substepping inside tick() with an accumulator.

namespace phys {

namespace {

constexpr JPH::ObjectLayer kLayerStatic = 0;
constexpr JPH::ObjectLayer kLayerMoving = 1;
constexpr u32 kLayerCount = 2;

namespace bp {
constexpr JPH::BroadPhaseLayer kStatic { 0 };
constexpr JPH::BroadPhaseLayer kMoving { 1 };
constexpr u32 kCount = 2;
} // namespace bp

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return bp::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer)
        const override {
        return layer == kLayerStatic ? bp::kStatic : bp::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer)
        const override {
        return layer == bp::kStatic ? "STATIC" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhase final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer,
                       JPH::BroadPhaseLayer broadPhase) const override {
        if (layer == kLayerStatic) {
            return broadPhase == bp::kMoving; // static vs static: never
        }
        return true;
    }
};

class ObjectVsObject final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return !(a == kLayerStatic && b == kLayerStatic);
    }
};

JPH::Vec3 toJph(const Vec3& v) { return { v.x, v.y, v.z }; }
JPH::Quat toJph(const Quat& q) { return { q.x, q.y, q.z, q.w }; }
Vec3 toGlm(JPH::Vec3Arg v) { return { v.GetX(), v.GetY(), v.GetZ() }; }

// Jolt's Factory/type registration is process-global; refcount it so
// several PhysicsWorlds (tests) coexist.
u32 gJoltUsers = 0;
void acquireJolt() {
    if (gJoltUsers++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}
void releaseJolt() {
    if (--gJoltUsers == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

constexpr f32 kFixedStep = 1.0f / 60.0f;

} // namespace

struct PhysicsWorld::Impl {
    BroadPhaseLayers broadPhaseLayers;
    ObjectVsBroadPhase objectVsBroadPhase;
    ObjectVsObject objectVsObject;
    uptr<JPH::TempAllocatorImpl> tempAllocator;
    uptr<JPH::JobSystemSingleThreaded> jobSystem;
    JPH::PhysicsSystem system;
    f32 accumulator { 0.0f };
};

PhysicsWorld::PhysicsWorld() {
    acquireJolt();
    pimpl = std::make_unique<Impl>();
    pimpl->tempAllocator =
        std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
    pimpl->jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(
        JPH::cMaxPhysicsJobs);
    pimpl->system.Init(4096, 0, 4096, 2048, pimpl->broadPhaseLayers,
                       pimpl->objectVsBroadPhase, pimpl->objectVsObject);
}

PhysicsWorld::~PhysicsWorld() {
    pimpl.reset();
    releaseJolt();
}

void PhysicsWorld::tick(f32 dt) {
    pimpl->accumulator += dt;
    while (pimpl->accumulator >= kFixedStep) {
        pimpl->system.Update(kFixedStep, 1, pimpl->tempAllocator.get(),
                             pimpl->jobSystem.get());
        pimpl->accumulator -= kFixedStep;
    }
}

BodyId PhysicsWorld::addStaticBox(const Vec3& halfExtents,
                                  const Vec3& position,
                                  const Quat& rotation) {
    JPH::BodyCreationSettings settings {
        new JPH::BoxShape(toJph(halfExtents)), toJph(position),
        toJph(rotation), JPH::EMotionType::Static, kLayerStatic
    };
    const JPH::BodyID body =
        pimpl->system.GetBodyInterface().CreateAndAddBody(
            settings, JPH::EActivation::DontActivate);
    return body.GetIndexAndSequenceNumber();
}

BodyId PhysicsWorld::addHeightField(const f32* samples, u32 sampleCount,
                                    const Vec3& origin, f32 spacing) {
    JPH::HeightFieldShapeSettings shape {
        samples, toJph(origin), JPH::Vec3(spacing, 1.0f, spacing),
        sampleCount
    };
    const JPH::ShapeSettings::ShapeResult result = shape.Create();
    if (result.HasError()) {
        LOG_ERROR("Physics: height field rejected: {}",
                  result.GetError().c_str());
        return 0;
    }
    // The origin is baked into the shape's offset; the body sits at zero.
    JPH::BodyCreationSettings settings { result.Get(), JPH::RVec3::sZero(),
                                         JPH::Quat::sIdentity(),
                                         JPH::EMotionType::Static,
                                         kLayerStatic };
    const JPH::BodyID body =
        pimpl->system.GetBodyInterface().CreateAndAddBody(
            settings, JPH::EActivation::DontActivate);
    return body.GetIndexAndSequenceNumber();
}

BodyId PhysicsWorld::addStaticMesh(const Vec3* vertices, u32 vertexCount,
                                   const u32* indices, u32 indexCount,
                                   const Vec3& position,
                                   const Quat& rotation, const Vec3& scale) {
    if (!vertices || !indices || vertexCount == 0 || indexCount < 3) {
        return 0;
    }
    JPH::VertexList jphVertices;
    jphVertices.reserve(vertexCount);
    for (u32 i = 0; i < vertexCount; ++i) {
        jphVertices.push_back({ vertices[i].x * scale.x,
                                vertices[i].y * scale.y,
                                vertices[i].z * scale.z });
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(indexCount / 3);
    for (u32 i = 0; i + 2 < indexCount; i += 3) {
        triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1],
                                                 indices[i + 2]));
    }
    JPH::MeshShapeSettings shape { std::move(jphVertices),
                                   std::move(triangles) };
    const JPH::ShapeSettings::ShapeResult result = shape.Create();
    if (result.HasError()) {
        LOG_ERROR("Physics: mesh shape rejected: {}",
                  result.GetError().c_str());
        return 0;
    }
    JPH::BodyCreationSettings settings { result.Get(), toJph(position),
                                         toJph(rotation),
                                         JPH::EMotionType::Static,
                                         kLayerStatic };
    const JPH::BodyID body =
        pimpl->system.GetBodyInterface().CreateAndAddBody(
            settings, JPH::EActivation::DontActivate);
    return body.GetIndexAndSequenceNumber();
}

void PhysicsWorld::removeBody(BodyId body) {
    const JPH::BodyID id { static_cast<JPH::uint32>(body) };
    pimpl->system.GetBodyInterface().RemoveBody(id);
    pimpl->system.GetBodyInterface().DestroyBody(id);
}

RayHit PhysicsWorld::rayCast(const Vec3& from, const Vec3& direction,
                             f32 maxDistance) const {
    const JPH::RRayCast ray { toJph(from),
                              toJph(direction * maxDistance) };
    JPH::RayCastResult result;
    RayHit hit;
    if (pimpl->system.GetNarrowPhaseQuery().CastRay(ray, result)) {
        hit.hit = true;
        hit.distance = result.mFraction * maxDistance;
        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        hit.position = { static_cast<f32>(point.GetX()),
                         static_cast<f32>(point.GetY()),
                         static_cast<f32>(point.GetZ()) };
        hit.body = result.mBodyID.GetIndexAndSequenceNumber();
        JPH::BodyLockRead lock { pimpl->system.GetBodyLockInterface(),
                                 result.mBodyID };
        if (lock.Succeeded()) {
            hit.normal = toGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(
                result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
        }
    }
    return hit;
}

RayHit PhysicsWorld::sphereCast(const Vec3& from, const Vec3& direction,
                                f32 maxDistance, f32 radius) const {
    const JPH::SphereShape sphere { radius };
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        &sphere, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sTranslation(toJph(from)),
        toJph(direction * maxDistance));
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    pimpl->system.GetNarrowPhaseQuery().CastShape(
        cast, JPH::ShapeCastSettings {}, JPH::RVec3::sZero(), collector);
    RayHit hit;
    if (collector.HadHit()) {
        hit.hit = true;
        hit.distance = collector.mHit.mFraction * maxDistance;
        hit.position = toGlm(collector.mHit.mContactPointOn2);
        // Penetration axis points from the sphere INTO the surface.
        const JPH::Vec3 axis = collector.mHit.mPenetrationAxis;
        if (axis.LengthSq() > 1.0e-12f) {
            hit.normal = toGlm(-axis.Normalized());
        }
        hit.body = collector.mHit.mBodyID2.GetIndexAndSequenceNumber();
    }
    return hit;
}

// --- CharacterBody -----------------------------------------------------------

struct CharacterBody::Impl {
    PhysicsWorld& world;
    JPH::Ref<JPH::CharacterVirtual> character;
    f32 verticalVelocity { 0.0f };
    bool swimming { false }; // P0 D2b: gravity off, full-3D velocity
    bool crouched { false }; // sneak: the half-height capsule is active
    JPH::RefConst<JPH::Shape> standingShape;
    JPH::RefConst<JPH::Shape> crouchedShape;
    explicit Impl(PhysicsWorld& world) : world { world } {}
};

namespace {

// Feet-origin capsule (shared by the standing and crouched shapes).
JPH::RefConst<JPH::Shape> makeFeetCapsule(f32 radius, f32 height) {
    const f32 cylinderHalf = glm::max(height * 0.5f - radius, 0.01f);
    return JPH::RotatedTranslatedShapeSettings(
               JPH::Vec3(0.0f, cylinderHalf + radius, 0.0f),
               JPH::Quat::sIdentity(),
               new JPH::CapsuleShape(cylinderHalf, radius))
        .Create()
        .Get();
}

} // namespace

CharacterBody::CharacterBody(PhysicsWorld& world, f32 radius, f32 height,
                             const Vec3& position) {
    pimpl = std::make_unique<Impl>(world);
    // Shape origin at the FEET; the crouched twin is HALF height (sneak).
    pimpl->standingShape = makeFeetCapsule(radius, height);
    pimpl->crouchedShape = makeFeetCapsule(radius, height * 0.5f);
    JPH::Ref<JPH::CharacterVirtualSettings> settings =
        new JPH::CharacterVirtualSettings();
    settings->mShape = pimpl->standingShape;
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    pimpl->character = new JPH::CharacterVirtual(
        settings, toJph(position), JPH::Quat::sIdentity(),
        &world.impl().system);
}

CharacterBody::~CharacterBody() = default;

void CharacterBody::move(const Vec3& desiredVelocity, f32 dt) {
    auto& impl = *pimpl;
    if (impl.swimming) {
        // P0 D2b: the water carries the body — no gravity, the caller's
        // 3D velocity is the whole story (buoyancy and surface clamping
        // are its job). Collisions still resolve (banks, the bottom).
        impl.verticalVelocity = desiredVelocity.y;
        impl.character->SetLinearVelocity(JPH::Vec3(
            desiredVelocity.x, desiredVelocity.y, desiredVelocity.z));
        JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
        impl.character->ExtendedUpdate(
            dt, JPH::Vec3::sZero(), updateSettings,
            impl.world.impl().system.GetDefaultBroadPhaseLayerFilter(
                kLayerMoving),
            impl.world.impl().system.GetDefaultLayerFilter(kLayerMoving),
            {}, {}, *impl.world.impl().tempAllocator);
        return;
    }
    const JPH::Vec3 gravity { 0.0f, -9.81f, 0.0f };
    if (onGround() && impl.verticalVelocity < 0.0f) {
        impl.verticalVelocity = 0.0f;
    } else {
        impl.verticalVelocity += gravity.GetY() * dt;
    }
    impl.character->SetLinearVelocity(
        JPH::Vec3(desiredVelocity.x, impl.verticalVelocity,
                  desiredVelocity.z));

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    impl.character->ExtendedUpdate(
        dt, gravity, updateSettings,
        impl.world.impl().system.GetDefaultBroadPhaseLayerFilter(
            kLayerMoving),
        impl.world.impl().system.GetDefaultLayerFilter(kLayerMoving), {},
        {}, *impl.world.impl().tempAllocator);
}

void CharacterBody::setSwimming(bool swimming) {
    if (pimpl->swimming != swimming) {
        pimpl->swimming = swimming;
        pimpl->verticalVelocity = 0.0f; // no gravity carry across modes
    }
}

bool CharacterBody::isSwimming() const { return pimpl->swimming; }

bool CharacterBody::setCrouched(bool crouched) {
    auto& impl = *pimpl;
    if (impl.crouched == crouched) {
        return true;
    }
    // Jolt refuses the swap when the new shape would penetrate (standing
    // up under a low ceiling) — the caller keeps the current stance.
    const bool swapped = impl.character->SetShape(
        crouched ? impl.crouchedShape : impl.standingShape,
        1.5f * impl.world.impl().system.GetPhysicsSettings()
                   .mPenetrationSlop,
        impl.world.impl().system.GetDefaultBroadPhaseLayerFilter(
            kLayerMoving),
        impl.world.impl().system.GetDefaultLayerFilter(kLayerMoving), {},
        {}, *impl.world.impl().tempAllocator);
    if (swapped) {
        impl.crouched = crouched;
    }
    return swapped;
}

bool CharacterBody::isCrouched() const { return pimpl->crouched; }

void CharacterBody::jump(f32 speed) {
    if (onGround()) {
        pimpl->verticalVelocity = speed;
    }
}

Vec3 CharacterBody::position() const {
    const JPH::RVec3 p = pimpl->character->GetPosition();
    return { static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
             static_cast<f32>(p.GetZ()) };
}

bool CharacterBody::onGround() const {
    return pimpl->character->GetGroundState() ==
           JPH::CharacterVirtual::EGroundState::OnGround;
}

} // namespace phys
