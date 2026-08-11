#include "physics/PhysicsWorld.hpp"

#include "core/Profiler.hpp"
#include "physics/JoltGlue.hpp"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#ifdef __EMSCRIPTEN__
#include <Jolt/Core/JobSystemSingleThreaded.h>
#endif
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollectFacesMode.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include "core/Log.hpp"

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace saida {

using namespace JPH;

namespace {

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mapping_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mapping_[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override {
        return mapping_[layer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer) const override { return "Layer"; }
#endif
private:
    BroadPhaseLayer mapping_[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer layer, BroadPhaseLayer bpLayer) const override {
        switch (layer) {
            case Layers::NON_MOVING: return bpLayer == BroadPhaseLayers::MOVING;
            case Layers::MOVING: return true;
            default: return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override {
        switch (a) {
            case Layers::NON_MOVING: return b == Layers::MOVING;  // static collides only with moving
            case Layers::MOVING: return true;
            default: return false;
        }
    }
};

int g_refCount = 0;

void globalInit() {
    if (g_refCount++ == 0) {
        RegisterDefaultAllocator();
        Factory::sInstance = new Factory();
        RegisterTypes();
    }
}

void globalShutdown() {
    if (--g_refCount == 0) {
        UnregisterTypes();
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
    }
}

EMotionType toMotionType(BodyMotion m) {
    switch (m) {
        case BodyMotion::Static: return EMotionType::Static;
        case BodyMotion::Kinematic: return EMotionType::Kinematic;
        default: return EMotionType::Dynamic;
    }
}

uint64 contactKey(BodyID a, BodyID b) {
    uint32 x = a.GetIndexAndSequenceNumber();
    uint32 y = b.GetIndexAndSequenceNumber();
    if (x > y) std::swap(x, y);
    return (static_cast<uint64>(x) << 32) | y;
}

} // namespace

// Records contact begin/end for BOTH solid collisions and sensor (trigger)
// overlaps, tagging each with whether a sensor was involved. Jolt calls these from
// worker threads, so all state is mutex-guarded and drained on the main thread.
class TriggerContactListener final : public JPH::ContactListener {
public:
    void OnContactAdded(const Body& b1, const Body& b2, const ContactManifold&,
                        ContactSettings&) override {
        bool sensor = b1.IsSensor() || b2.IsSensor();
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_.emplace(contactKey(b1.GetID(), b2.GetID()), sensor).second)
            events_.push_back({b1.GetID(), b2.GetID(), true, sensor});
    }

    void OnContactRemoved(const SubShapeIDPair& pair) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(contactKey(pair.GetBody1ID(), pair.GetBody2ID()));
        if (it != active_.end()) {
            bool sensor = it->second;
            active_.erase(it);
            events_.push_back({pair.GetBody1ID(), pair.GetBody2ID(), false, sensor});
        }
    }

    std::vector<PhysicsWorld::ContactEvent> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PhysicsWorld::ContactEvent> out;
        out.swap(events_);
        return out;
    }

private:
    std::mutex mutex_;
    std::unordered_map<uint64, bool> active_;  // active pair -> involved a sensor
    std::vector<PhysicsWorld::ContactEvent> events_;
};

struct PhysicsWorld::LayerState {
    BPLayerInterfaceImpl broadphase;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadphase;
    ObjectLayerPairFilterImpl objectVsObject;
};

PhysicsWorld::PhysicsWorld() {
    globalInit();

    layers_ = std::make_unique<LayerState>();
    tempAllocator_ = std::make_unique<TempAllocatorImpl>(16 * 1024 * 1024);

#ifdef __EMSCRIPTEN__
    // wasm without pthreads: Jolt provides an equivalent single-threaded job system.
    jobSystem_ = std::make_unique<JobSystemSingleThreaded>(cMaxPhysicsJobs);
#else
    unsigned hw = std::thread::hardware_concurrency();
    int workerThreads = std::max(1, static_cast<int>(hw) - 1);
    jobSystem_ = std::make_unique<JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, workerThreads);
#endif

    system_ = std::make_unique<PhysicsSystem>();
    const uint cMaxBodies = 8192;
    const uint cNumBodyMutexes = 0;  // 0 = autodetect
    const uint cMaxBodyPairs = 8192;
    const uint cMaxContactConstraints = 4096;
    system_->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                  layers_->broadphase, layers_->objectVsBroadphase, layers_->objectVsObject);
    system_->SetGravity(Vec3(0.0f, -9.81f, 0.0f));

    contactListener_ = std::make_unique<TriggerContactListener>();
    system_->SetContactListener(contactListener_.get());
}

PhysicsWorld::~PhysicsWorld() {
    system_.reset();
    contactListener_.reset();
    jobSystem_.reset();
    tempAllocator_.reset();
    layers_.reset();
    globalShutdown();
}

std::vector<PhysicsWorld::ContactEvent> PhysicsWorld::drainContactEvents() {
    return contactListener_->drain();
}

void* PhysicsWorld::bodyUserData(JPH::BodyID id) const {
    if (id.IsInvalid()) return nullptr;
    return reinterpret_cast<void*>(system_->GetBodyInterface().GetUserData(id));
}

void PhysicsWorld::step(float dt) {
    SAIDA_PROFILE_FUNCTION();
    if (dt <= 0.0f) return;

    const float fixed = kFixedStep;
    accumulator_ += dt;
    if (accumulator_ > 0.25f) accumulator_ = 0.25f;  // avoid spiral of death after a hitch

    int steps = 0;
    while (accumulator_ >= fixed && steps < kMaxSubSteps) {
        SAIDA_PROFILE_SCOPE("Physics/JoltUpdate");
        system_->Update(fixed, 1, tempAllocator_.get(), jobSystem_.get());
        accumulator_ -= fixed;
        ++steps;
    }
    SAIDA_PROFILE_COUNTER("Physics/FixedSteps", steps);
}

JPH::BodyID PhysicsWorld::createBody(const BodyDesc& d) {
    if (!d.shape) return BodyID();

    ObjectLayer layer = (d.motion == BodyMotion::Static) ? Layers::NON_MOVING : Layers::MOVING;
    BodyCreationSettings settings(d.shape, RVec3(d.position.x, d.position.y, d.position.z),
                                  toJolt(d.rotation), toMotionType(d.motion), layer);
    settings.mIsSensor = d.isSensor;
    settings.mFriction = d.friction;
    settings.mRestitution = d.restitution;
    settings.mUserData = reinterpret_cast<uint64>(d.userData);

    if (d.motion == BodyMotion::Dynamic) {
        settings.mLinearDamping = d.linearDamping;
        settings.mAngularDamping = d.angularDamping;
        settings.mGravityFactor = d.gravityFactor;
        if (d.mass > 0.0f) {
            settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = d.mass;
        }
    }

    BodyInterface& bi = system_->GetBodyInterface();
    Body* body = bi.CreateBody(settings);
    if (!body) {
        Log::warn("PhysicsWorld: body limit reached, cannot create body");
        return BodyID();
    }
    EActivation activation = (d.motion == BodyMotion::Static) ? EActivation::DontActivate
                                                              : EActivation::Activate;
    bi.AddBody(body->GetID(), activation);
    return body->GetID();
}

void PhysicsWorld::removeBody(JPH::BodyID id) {
    if (id.IsInvalid()) return;
    // Drop any constraint still attached to this body first: destroying a body
    // referenced by a live constraint leaves Jolt with a dangling Body*. The
    // surviving body is woken up — a sleeping body would otherwise hover where
    // the constraint left it.
    for (std::size_t i = constraints_.size(); i-- > 0;) {
        const Ref<TwoBodyConstraint>& c = constraints_[i];
        const Body* b1 = c->GetBody1();
        const Body* b2 = c->GetBody2();
        if ((b1 && b1->GetID() == id) || (b2 && b2->GetID() == id)) {
            const Body* other = (b1 && b1->GetID() == id) ? b2 : b1;
            system_->RemoveConstraint(c.GetPtr());
            constraints_.erase(constraints_.begin() + static_cast<std::ptrdiff_t>(i));
            if (other && !other->GetID().IsInvalid() && other->GetID() != id &&
                !other->IsStatic())
                system_->GetBodyInterface().ActivateBody(other->GetID());
        }
    }
    BodyInterface& bi = system_->GetBodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
}

void PhysicsWorld::setBodyTransform(JPH::BodyID id, const glm::vec3& position,
                                    const glm::quat& rotation, bool activate) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().SetPositionAndRotation(
        id, RVec3(position.x, position.y, position.z), toJolt(rotation),
        activate ? EActivation::Activate : EActivation::DontActivate);
}

void PhysicsWorld::moveKinematic(JPH::BodyID id, const glm::vec3& position,
                                 const glm::quat& rotation, float dt) {
    if (id.IsInvalid() || dt <= 0.0f) return;
    system_->GetBodyInterface().MoveKinematic(
        id, RVec3(position.x, position.y, position.z), toJolt(rotation), dt);
}

void PhysicsWorld::setLinearVelocity(JPH::BodyID id, const glm::vec3& v) {
    if (id.IsInvalid()) return;
    BodyInterface& bi = system_->GetBodyInterface();
    bi.ActivateBody(id);
    bi.SetLinearVelocity(id, Vec3(v.x, v.y, v.z));
}

void PhysicsWorld::setAngularVelocity(JPH::BodyID id, const glm::vec3& v) {
    if (id.IsInvalid()) return;
    BodyInterface& bi = system_->GetBodyInterface();
    bi.ActivateBody(id);
    bi.SetAngularVelocity(id, Vec3(v.x, v.y, v.z));
}

glm::vec3 PhysicsWorld::linearVelocity(JPH::BodyID id) const {
    if (id.IsInvalid()) return glm::vec3(0.0f);
    return toGlm(system_->GetBodyInterface().GetLinearVelocity(id));
}

glm::vec3 PhysicsWorld::angularVelocity(JPH::BodyID id) const {
    if (id.IsInvalid()) return glm::vec3(0.0f);
    return toGlm(system_->GetBodyInterface().GetAngularVelocity(id));
}

glm::vec3 PhysicsWorld::pointVelocity(JPH::BodyID id, const glm::vec3& worldPoint) const {
    if (id.IsInvalid()) return glm::vec3(0.0f);
    return toGlm(system_->GetBodyInterface().GetPointVelocity(
        id, RVec3(worldPoint.x, worldPoint.y, worldPoint.z)));
}

float PhysicsWorld::effectiveMassAt(JPH::BodyID id, const glm::vec3& worldPoint,
                                    const glm::vec3& direction) const {
    if (id.IsInvalid()) return 0.0f;
    const float length = glm::length(direction);
    if (length < 1e-6f) return 0.0f;
    const glm::vec3 d = direction / length;

    JPH::BodyLockRead lock(system_->GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return 0.0f;
    const JPH::Body& body = lock.GetBody();
    if (!body.IsDynamic()) return 0.0f;
    const JPH::MotionProperties* motion = body.GetMotionProperties();
    if (!motion) return 0.0f;

    const float invMass = motion->GetInverseMass();
    if (invMass <= 0.0f) return 0.0f;

    const Vec3 r = Vec3(worldPoint.x, worldPoint.y, worldPoint.z) -
                   Vec3(body.GetCenterOfMassPosition());
    const Vec3 dir(d.x, d.y, d.z);
    const Vec3 rXd = r.Cross(dir);
    const Mat44 invInertia = motion->GetInverseInertiaForRotation(
        Mat44::sRotation(body.GetRotation()));
    const float angular = rXd.Dot(invInertia * rXd);
    return 1.0f / (invMass + angular);
}

// Jolt's AddImpulse/AddForce already ignore non-dynamic bodies and wake a
// sleeping one, so these only have to guard the invalid id.
void PhysicsWorld::applyImpulse(JPH::BodyID id, const glm::vec3& impulse) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().AddImpulse(id, Vec3(impulse.x, impulse.y, impulse.z));
}

void PhysicsWorld::applyImpulse(JPH::BodyID id, const glm::vec3& impulse,
                                const glm::vec3& worldPoint) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().AddImpulse(id, Vec3(impulse.x, impulse.y, impulse.z),
                                           RVec3(worldPoint.x, worldPoint.y, worldPoint.z));
}

void PhysicsWorld::applyAngularImpulse(JPH::BodyID id, const glm::vec3& angularImpulse) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().AddAngularImpulse(
        id, Vec3(angularImpulse.x, angularImpulse.y, angularImpulse.z));
}

void PhysicsWorld::applyForce(JPH::BodyID id, const glm::vec3& force) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().AddForce(id, Vec3(force.x, force.y, force.z));
}

void PhysicsWorld::applyForce(JPH::BodyID id, const glm::vec3& force,
                              const glm::vec3& worldPoint) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().AddForce(id, Vec3(force.x, force.y, force.z),
                                         RVec3(worldPoint.x, worldPoint.y, worldPoint.z));
}

void PhysicsWorld::applyTorque(JPH::BodyID id, const glm::vec3& torque) {
    if (id.IsInvalid()) return;
    system_->GetBodyInterface().AddTorque(id, Vec3(torque.x, torque.y, torque.z));
}

void PhysicsWorld::getBodyTransform(JPH::BodyID id, glm::vec3& position,
                                    glm::quat& rotation) const {
    if (id.IsInvalid()) return;
    const BodyInterface& bi = system_->GetBodyInterface();
    RVec3 p;
    Quat q;
    bi.GetPositionAndRotation(id, p, q);
    position = glm::vec3(p.GetX(), p.GetY(), p.GetZ());
    rotation = toGlm(q);
}

JPH::Ref<JPH::CharacterVirtual> PhysicsWorld::createCharacter(
    const JPH::Shape* shape, const glm::vec3& position, const glm::quat& rotation,
    float mass, float maxSlopeAngleRad, void* userData) {
    if (!shape) return nullptr;

    Ref<CharacterVirtualSettings> settings = new CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mMass = mass;
    settings->mMaxSlopeAngle = maxSlopeAngleRad;
    settings->mUp = Vec3::sAxisY();
    // A CharacterVirtual doesn't exist in the broadphase: without an inner
    // body, sensors (Area) and raycasts never see the character. The
    // kinematic inner body follows the character and inherits its userData
    // (so trigger dispatch can find the CharacterBodyNode).
    settings->mInnerBodyShape = shape;
    settings->mInnerBodyLayer = Layers::MOVING;

    return new CharacterVirtual(settings, RVec3(position.x, position.y, position.z),
                                toJolt(rotation), reinterpret_cast<uint64>(userData),
                                system_.get());
}

void PhysicsWorld::updateCharacter(JPH::CharacterVirtual& character, float dt) {
    // ExtendedUpdate = move/slide + WalkStairs + StickToFloor with Jolt's defaults
    // (step up 0.4 m, stick down 0.5 m).
    CharacterVirtual::ExtendedUpdateSettings settings;
    character.ExtendedUpdate(dt, system_->GetGravity(), settings,
                             system_->GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                             system_->GetDefaultLayerFilter(Layers::MOVING),
                             {}, {}, *tempAllocator_);
}

namespace {

// Shared body-level filter for the scene queries: skips one explicit body and,
// unless requested, every sensor (Area trigger) body.
class QueryBodyFilter final : public BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& filter) : filter_(filter) {}

    bool ShouldCollide(const BodyID& id) const override {
        return filter_.ignore.IsInvalid() || id != filter_.ignore;
    }
    bool ShouldCollideLocked(const Body& body) const override {
        return filter_.hitSensors || !body.IsSensor();
    }

private:
    const QueryFilter& filter_;
};

} // namespace

RaycastHit PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction,
                                 float maxDistance, const QueryFilter& filter) const {
    RaycastHit out;
    RRayCast ray{RVec3(origin.x, origin.y, origin.z), toJolt(direction) * maxDistance};
    QueryBodyFilter bodyFilter(filter);
    // ClosestHitCollisionCollector + filter: the closest hit among the
    // admitted bodies (the plain CastRay doesn't take a BodyFilter).
    ClosestHitCollisionCollector<CastRayCollector> collector;
    system_->GetNarrowPhaseQuery().CastRay(ray, {}, collector, {}, {}, bodyFilter);
    if (collector.HadHit()) {
        const RayCastResult& result = collector.mHit;
        out.hit = true;
        out.body = result.mBodyID;
        out.distance = result.mFraction * maxDistance;
        RVec3 hitPos = ray.GetPointOnRay(result.mFraction);
        out.point = glm::vec3(hitPos.GetX(), hitPos.GetY(), hitPos.GetZ());

        BodyLockRead lock(system_->GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) {
            Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hitPos);
            out.normal = toGlm(n);
        }
    }
    return out;
}

std::vector<JPH::BodyID> PhysicsWorld::overlapSphere(const glm::vec3& center, float radius,
                                                     const QueryFilter& filter) const {
    std::vector<BodyID> out;
    if (radius <= 0.0f) return out;

    SphereShape sphere(radius);
    sphere.SetEmbedded();  // stack-owned: opt out of ref-counted destruction
    CollideShapeSettings settings;
    AllHitCollisionCollector<CollideShapeCollector> collector;
    QueryBodyFilter bodyFilter(filter);
    system_->GetNarrowPhaseQuery().CollideShape(
        &sphere, Vec3::sReplicate(1.0f),
        RMat44::sTranslation(RVec3(center.x, center.y, center.z)), settings,
        RVec3::sZero(), collector, {}, {}, bodyFilter);

    out.reserve(collector.mHits.size());
    for (const CollideShapeResult& hit : collector.mHits) {
        if (std::find(out.begin(), out.end(), hit.mBodyID2) == out.end())
            out.push_back(hit.mBodyID2);  // each body only once (compounds)
    }
    return out;
}

void PhysicsWorld::addConstraint(JPH::Ref<JPH::TwoBodyConstraint> constraint) {
    if (!constraint) return;
    system_->AddConstraint(constraint.GetPtr());
    constraints_.push_back(std::move(constraint));
}

void PhysicsWorld::removeConstraint(const JPH::Ref<JPH::TwoBodyConstraint>& constraint) {
    if (!constraint) return;
    auto it = std::find(constraints_.begin(), constraints_.end(), constraint);
    if (it == constraints_.end()) return;
    system_->RemoveConstraint(constraint.GetPtr());
    constraints_.erase(it);
    // Wake both bodies: a sleeping body would keep hovering where the
    // constraint held it instead of resuming under gravity.
    BodyInterface& bi = system_->GetBodyInterface();
    for (const Body* body : {constraint->GetBody1(), constraint->GetBody2()}) {
        if (body && !body->GetID().IsInvalid() && !body->IsStatic())
            bi.ActivateBody(body->GetID());
    }
}

} // namespace saida
