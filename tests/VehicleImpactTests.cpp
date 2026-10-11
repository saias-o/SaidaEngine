#include "behaviours/SurfaceDriveBehaviour.hpp"
#include "physics/DeformationCage.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "scene/Scene.hpp"
#include "core/RangeAllocator.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace saida;
namespace {
int checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "[vehicle-impact] FAIL %s\n", message); std::abort(); }
}
void cageTests() {
    DeformationCage weak({-1,-1,-2}, {1,1,2}), strong({-1,-1,-2}, {1,1,2});
    const auto point = glm::vec3(0,0,-2);
    const auto binding = weak.bind(point);
    require(!weak.impact(point, {0,0,1}, 100), "below yield leaves geometry intact");
    require(weak.impact(point, {0,0,1}, 2000), "moderate hit deforms");
    require(strong.impact(point, {0,0,1}, 8000), "strong hit deforms");
    require(glm::length(strong.offset(binding)) > glm::length(weak.offset(binding)), "energy increases local displacement");
    require(strong.offset(binding).z > 0, "front hit moves inward");
    require(glm::length(strong.offset(strong.bind({0,0,2}))) < .01f, "rear remains intact");
    for (int hit = 0; hit < 20; ++hit) strong.impact(point, {0,0,1}, 80000);
    for (int x = 0; x <= 12; ++x) for (int y = 0; y <= 12; ++y) for (int z = 0; z <= 12; ++z) {
        auto sample = strong.bind({-1 + x/6.f, -1 + y/6.f, -2 + z/3.f});
        require(glm::determinant(strong.jacobian(sample)) > 0.f, "repeated impacts preserve local orientation");
        require(glm::length(strong.offset(sample)) < .6f, "repeated deformation is bounded");
    }
    for (int frame = 0; frame < 120; ++frame) strong.relax(1.f/60.f);
    require(!strong.active(), "settled dent sleeps");
    DeformationCage restored({-1,-1,-2}, {1,1,2});
    require(restored.restore(strong.permanentOffsets()), "compact state restores");
    require(glm::distance(restored.offset(binding), strong.offset(binding)) < .0001f, "saved permanent dent matches");
    restored.reset();
    require(glm::length(restored.offset(binding)) == 0.f, "explicit reset repairs");
    auto invalid = strong.permanentOffsets();
    invalid.front().x = NAN;
    require(!restored.restore(invalid), "nonfinite state rejected");
}
void arenaTests() {
    RangeAllocator arena(100);
    const auto a=arena.allocate(20),b=arena.allocate(30),c=arena.allocate(50);
    require(a&&b&&c&&!arena.allocate(1),"arena refuses overflow");
    arena.release(*b,30);arena.release(*a,20);
    require(arena.largestFree()==50,"adjacent retired ranges coalesce");
    arena.release(*c,50);
    for(int frame=0;frame<1000;++frame) {
        const auto range=arena.allocate(100);require(range&&*range==0,"repeated deformation storage reuses the arena");
        arena.release(*range,100);
    }
    require(arena.used()==0&&arena.largestFree()==100,"arena accounting remains exact");
    bool rejected=false;try{arena.release(0,1);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"double retirement rejected");
}
struct Rig {
    Scene scene;
    RigidBodyNode* car;
    SurfaceDriveBehaviour* drive;
    explicit Rig(float slopeDegrees = 0.f) {
        auto* floor = scene.createChild<StaticBodyNode>();
        const auto slope = glm::angleAxis(glm::radians(slopeDegrees), glm::vec3(1,0,0));
        floor->transform().rotation = slope;
        floor->transform().position = slope * glm::vec3(0,-1,0);
        auto* ground = floor->createChild<CollisionShapeNode>();
        ground->shapeType = CollisionShapeType::Box;
        ground->halfExtents = {500,1,500};
        car = scene.createChild<RigidBodyNode>();
        car->mass = 1200;
        car->reportsImpacts = true;
        car->continuousCollision = true;
        car->friction = .05f;
        car->transform().rotation = slope;
        auto* shape = car->createChild<CollisionShapeNode>();
        shape->shapeType = CollisionShapeType::Box;
        shape->halfExtents = {1,.6f,2};
        shape->offset = {0,.6f,0};
        drive = car->addBehaviour<SurfaceDriveBehaviour>();
        drive->parked = false;
        for (int i=0;i<60;++i) scene.update(1.f/60.f);
    }
};
void cruiseGradeTests() {
    for (float throttle : {-1.f, 1.f}) {
        float flatSpeed = 0.f;
        for (float grade : {0.f, 22.f}) {
            Rig rig(grade * throttle);
            rig.car->linearDamping = 0.f;
            rig.car->markDirty();
            rig.drive->slopeAssist = 1.f;
            rig.drive->damage = .35f;
            rig.drive->maxSpeed = rig.drive->reverseSpeed = 25.f;
            rig.drive->command(0,1,0);
            for (int i=0; i<120; ++i) rig.scene.update(1.f/60.f);
            rig.drive->command(throttle,0,0);
            for (int i=0; i<900; ++i) rig.scene.update(1.f/60.f);
            const float speed = rig.drive->signedSpeed() * throttle;
            std::printf("[vehicle-impact] throttle %.0f grade %.0f cruise %.2f\n", throttle, grade, speed);
            if (grade == 0.f) flatSpeed = speed;
            else require(speed > flatSpeed * .97f,
                "powered grade compensation retains cruise speed in both directions with engine wear");
            require(speed < 25.1f, "grade compensation preserves the configured speed limit");
        }
    }
}

void supportTests() {
    Rig rig;
    auto& world = *rig.car->physicsWorld();
    const auto id = rig.car->bodyId();
    const auto identity = glm::quat(1,0,0,0);
    rig.drive->groundStability = 16.f;
    rig.drive->groundDamping = 8.f;
    rig.drive->command(1,0,1);

    world.setBodyTransform(id, {0,.6f,0}, identity, true);
    world.setLinearVelocity(id, {0,0,0});
    world.setAngularVelocity(id, {1,2,3});
    rig.drive->onPhysicsStep(1.f/60.f);
    require(!rig.drive->grounded(), "a nearby road below an airborne body is not tyre support");
    require(glm::length(world.linearVelocity(id)) < 1e-5f, "airborne throttle does not accelerate");
    require(glm::distance(world.angularVelocity(id), glm::vec3(1,2,3)) < 1e-5f,
        "airborne steering and ground stability leave angular momentum alone");

    world.setBodyTransform(id, {0,.02f,0}, identity, true);
    world.setLinearVelocity(id, {0,3,0});
    world.setAngularVelocity(id, {0,0,0});
    rig.drive->onPhysicsStep(1.f/60.f);
    require(!rig.drive->grounded(), "a jumping body loses traction while separating from the road");
    require(glm::distance(world.linearVelocity(id), glm::vec3(0,3,0)) < 1e-5f,
        "a jump retains its launch velocity without powered flight");

    auto* wall = rig.scene.createChild<StaticBodyNode>();
    wall->transform().position = {0,4,-5};
    auto* shape = wall->createChild<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Box;
    shape->halfExtents = {4,8,.05f};
    rig.scene.update(1.f/60.f);
    world.setBodyTransform(id, {0,4,-4.8f}, glm::angleAxis(glm::radians(90.f), glm::vec3(1,0,0)), true);
    world.setLinearVelocity(id, {0,0,0});
    world.setAngularVelocity(id, {0,0,0});
    rig.drive->supportDistance = 2.f;
    rig.drive->onPhysicsStep(1.f/60.f);
    require(!rig.drive->grounded(), "a pitched chassis cannot mistake a trunk wall for road");
    require(glm::length(world.linearVelocity(id)) < 1e-5f, "throttle cannot propel a car up a vertical obstacle");
}

void angularResponseTests() {
    Rig rig;
    auto& world = *rig.car->physicsWorld();
    auto id = rig.car->bodyId();
    for (const auto axis : {glm::vec3(1,0,0), glm::vec3(0,1,0), glm::normalize(glm::vec3(1,2,3))}) {
        world.setAngularVelocity(id, glm::vec3(0));
        const float inertia = world.effectiveAngularMass(id, axis);
        require(inertia > 0.f, "dynamic chassis reports positive angular inertia");
        world.applyAngularImpulse(id, axis * (inertia * .75f));
        require(std::abs(glm::dot(world.angularVelocity(id), axis) - .75f) < 1e-4f,
            "angular effective mass predicts impulse response along arbitrary axes");
    }
    require(world.effectiveAngularMass({}, {0,1,0}) == 0.f, "invalid body has no angular mass");
    require(world.effectiveAngularMass(id, {0,0,0}) == 0.f, "zero axis has no angular mass");

    const float originalInertia=world.effectiveAngularMass(id,{0,1,0});
    rig.car->inertiaMultiplier=2.2f;rig.car->markDirty();
    rig.scene.update(1.f/60.f);
    require(std::abs(world.effectiveAngularMass(rig.car->bodyId(),{0,1,0})/originalInertia-2.2f)<.001f,
        "authored inertia multiplier controls solver rotational response");
    rig.car->inertiaMultiplier=1.f;rig.car->markDirty();rig.scene.update(1.f/60.f);
    id=rig.car->bodyId();

    rig.drive->command(0,0,0);
    rig.drive->groundStability = 0.f;
    rig.drive->groundDamping = 60.f;
    world.setBodyTransform(id, {0,0,0}, glm::quat(1,0,0,0), true);
    world.setLinearVelocity(id, {0,0,0});
    world.setAngularVelocity(id, {0,0,5});
    rig.drive->onPhysicsStep(1.f/60.f);
    const float roll = world.angularVelocity(id).z;
    require(rig.drive->grounded(), "body remains supported for the damping measurement");
    require(roll > 0.f && roll < 2.f, "strong grounded tilt damping removes spin without reversing it");
}

void driveTests() {
    {
        Rig rig;
        rig.car->setLinearVelocity({0,0,-20});
        rig.drive->command(-1,0,0);
        for(int i=0;i<60;++i)rig.scene.update(1.f/60.f);
        require(rig.drive->signedSpeed()<15.f,"reverse torque opposes forward motion above the reverse speed limit");
    }
    {
        Rig assisted(22.f);
        assisted.drive->slopeAssist=1.f;
        assisted.drive->command(0,1,0);
        for(int i=0;i<120;++i)assisted.scene.update(1.f/60.f);
        assisted.drive->command(1,0,0);
        for(int i=0;i<180;++i)assisted.scene.update(1.f/60.f);
        Rig flat;
        flat.drive->command(1,0,0);
        for(int i=0;i<180;++i)flat.scene.update(1.f/60.f);
        require(assisted.drive->signedSpeed()>flat.drive->signedSpeed()*.9f,
            "optional slope assistance retains at least ninety percent of flat-ground acceleration");
        assisted.drive->command(0,0,0);
        const float speed=assisted.drive->signedSpeed();
        for(int i=0;i<60;++i)assisted.scene.update(1.f/60.f);
        require(assisted.drive->signedSpeed()<speed-3.f,"slope assistance does not cancel coasting gravity");
        assisted.car->setLinearVelocity({0,0,0});
        assisted.drive->damage=1.f;assisted.drive->command(1,0,0);
        for(int i=0;i<60;++i)assisted.scene.update(1.f/60.f);
        Rig passive(22.f);
        passive.drive->command(0,1,0);
        for(int i=0;i<120;++i)passive.scene.update(1.f/60.f);
        passive.drive->command(0,0,0);
        for(int i=0;i<60;++i)passive.scene.update(1.f/60.f);
        require(assisted.drive->signedSpeed()<0.f&&
            std::abs(assisted.drive->signedSpeed()-passive.drive->signedSpeed())<.5f,
            "a fully disabled motor behaves like passive coasting without slope assistance");
    }
    for (float slope : {8.f, 15.f, 22.f}) {
        Rig rig(slope);
        rig.drive->groundReach = .66f;
        rig.drive->command(0,1,0);
        for (int i=0;i<300;++i) rig.scene.update(1.f/60.f);
        const auto stopped = rig.car->transform().position;
        rig.drive->command(1,0,0);
        for (int i=0;i<300;++i) rig.scene.update(1.f/60.f);
        std::fprintf(stderr,"slope %.0f: travelled %.2f, speed %.2f, grounded %d\n", slope,
            stopped.z-rig.car->transform().position.z, rig.drive->signedSpeed(), rig.drive->grounded());
        require(stopped.z-rig.car->transform().position.z > 12.f, "vehicle restarts uphill after a complete stop");
    }
    float distance = 0;
    for (int rate : {30,60,120}) {
        Rig rig;
        rig.drive->command(1,0,0);
        for (int i=0;i<rate*4;++i) rig.scene.update(1.f/rate);
        require(rig.car->transform().position.z < -20, "force drive advances without suspension");
        if (rate == 30) distance = rig.car->transform().position.z;
        else require(std::abs(distance-rig.car->transform().position.z) < .5f, "drive independent of render cadence");
        rig.drive->command(0,1,0);
        for (int i=0;i<rate*4;++i) rig.scene.update(1.f/rate);
        require(std::abs(rig.drive->signedSpeed()) < .1f, "brake stops without reversing");
    }
    Rig rig;
    float energy = 0;
    auto* wall = rig.scene.createChild<StaticBodyNode>();
    wall->transform().position = {0,2,-15};
    auto* shape = wall->createChild<CollisionShapeNode>();
    shape->shapeType = CollisionShapeType::Box;
    shape->halfExtents = {5,2,.1f};
    auto connection = rig.car->impacted.connect([&](const auto& event) { energy += event.energy; });
    rig.drive->command(1,0,0);
    for (int i=0;i<240;++i) rig.scene.update(1.f/60.f);
    require(energy > 100, "solid contact reports incident energy");
    require(rig.car->transform().position.z > -15, "continuous collision retains fast vehicle");
    const float initialEnergy = energy;
    rig.drive->command(0,1,0);
    for (int i=0;i<240;++i) rig.scene.update(1.f/60.f);
    require(energy == initialEnergy, "resting contact does not accumulate damage");
    rig.car->setLinearVelocity({2,0,0});
    rig.car->markDirty();
    rig.scene.update(1.f/240.f);
    require(rig.car->physicsWorld()->linearVelocity(rig.car->bodyId()).x > 1.9f, "shape rebuild preserves momentum");
}
}
int main() {
    cageTests();
    arenaTests();
    cruiseGradeTests();
    supportTests();
    angularResponseTests();
    driveTests();
    std::printf("PASS vehicle-impact %d checks\n", checks);
}
