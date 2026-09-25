#include "scene/Scene.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace saida;

int main() {
    Scene scene;

    // Floor: static box, top at y = 0.5.
    auto* floor = scene.createChild<StaticBodyNode>();
    floor->transform().position = {0.0f, 0.0f, 0.0f};
    {
        auto cs = std::make_unique<CollisionShapeNode>();
        cs->shapeType = CollisionShapeType::Box;
        cs->halfExtents = {20.0f, 0.5f, 20.0f};
        floor->addChild(std::move(cs));
    }

    // Character: capsule, dropped from y = 3.
    auto* player = scene.createChild<CharacterBodyNode>();
    player->transform().position = {0.0f, 3.0f, 0.0f};
    {
        auto cs = std::make_unique<CollisionShapeNode>();
        cs->shapeType = CollisionShapeType::Capsule;
        cs->radius = 0.4f;
        cs->height = 1.8f;
        cs->axis = 1;
        player->addChild(std::move(cs));
    }

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 600; ++i) {
        if (player->isOnFloor()) player->velocity.y = 0.0f;
        else player->velocity.y -= 16.0f * dt;
        scene.update(dt);
        if (i % 60 == 0)
            std::printf("t=%.2f  y=%.3f  onFloor=%d\n", i * dt,
                        player->transform().position.y, player->isOnFloor() ? 1 : 0);
    }
    std::printf("FINAL y=%.3f onFloor=%d\n", player->transform().position.y,
                player->isOnFloor() ? 1 : 0);

    // A capsule (radius 0.4, height 1.8 → half-height 0.9) resting on a box whose
    // top is at y=0.5 settles with its centre at ~1.4. The character must stop
    // falling and report being on the floor.
    assert(player->isOnFloor());
    assert(std::fabs(player->transform().position.y - 1.4f) < 0.15f);
    std::printf("PASS: character rests on the floor\n");

    // A post in the way: a kinematic capsule (a person moved by code) 2 m
    // ahead. Walking into it, the character stops at it and names it among
    // its contacts, with the normal pointing back at the character.
    auto* post = scene.createChild<RigidBodyNode>();
    post->kinematic = true;
    post->transform().position = {2.0f, 1.4f, 0.0f};
    {
        auto cs = std::make_unique<CollisionShapeNode>();
        cs->shapeType = CollisionShapeType::Capsule;
        cs->radius = 0.3f;
        cs->height = 1.8f;
        cs->axis = 1;
        post->addChild(std::move(cs));
    }
    bool touched = false;
    for (int i = 0; i < 120; ++i) {
        player->velocity = {2.0f, player->isOnFloor() ? 0.0f : -1.0f, 0.0f};
        scene.update(dt);
        for (const auto& contact : player->contacts())
            if (contact.node == post) {
                touched = true;
                assert(contact.normal.x < -0.5f);
            }
    }
    const float stopped = player->transform().position.x;
    std::printf("stopped at x=%.3f, touched=%d\n", stopped, touched ? 1 : 0);
    assert(touched);
    assert(stopped < 2.0f - 0.3f - 0.4f + 0.05f);

    // Disabled, the post leaves the world: nothing stops the character there
    // any more, and nothing is touched.
    post->setEnabled(false);
    bool touchedDisabled = false;
    for (int i = 0; i < 90; ++i) {
        player->velocity = {2.0f, player->isOnFloor() ? 0.0f : -1.0f, 0.0f};
        scene.update(dt);
        for (const auto& contact : player->contacts()) touchedDisabled = touchedDisabled || contact.node == post;
    }
    std::printf("walked on to x=%.3f\n", player->transform().position.x);
    assert(!touchedDisabled);
    assert(player->transform().position.x > 3.0f);
    std::printf("PASS: contacts name what the character ran into; a disabled body is gone\n");

    // Move-and-slide now, for a game that keeps its own positions: a second
    // character whose capsule is offset up by half its height, so that the
    // node stands at its feet, walked into the post again from x = 0.
    post->setEnabled(true);
    auto* walker = scene.createChild<CharacterBodyNode>();
    walker->transform().position = {0.0f, 0.5f, 0.0f};
    {
        auto cs = std::make_unique<CollisionShapeNode>();
        cs->shapeType = CollisionShapeType::Capsule;
        cs->radius = 0.4f;
        cs->height = 1.8f;
        cs->axis = 1;
        cs->offset = {0.0f, 0.9f, 0.0f};
        walker->addChild(std::move(cs));
    }
    scene.update(dt);  // its first sync builds the character
    player->setEnabled(false);
    glm::vec3 feet{0.0f, 0.5f, 0.0f};
    bool met = false;
    for (int i = 0; i < 90; ++i) {
        walker->transform().position = feet;
        feet = walker->moveAndSlide({2.0f, 0.0f, 0.0f}, dt);
        for (const auto& contact : walker->contacts()) met = met || contact.node == post;
        scene.update(dt);
        assert(glm::length(walker->transform().position - feet) < 1e-3f);  // the step left it there
    }
    std::printf("moved to x=%.3f y=%.3f, met=%d\n", feet.x, feet.y, met ? 1 : 0);
    assert(met);
    assert(feet.x > 1.0f && feet.x < 2.0f - 0.3f - 0.4f + 0.05f);
    assert(std::fabs(feet.y - 0.5f) < 0.05f);
    // Glancing: aimed past the post's side, it slides round and goes on.
    walker->transform().position = {0.0f, 0.5f, 0.3f};
    for (int i = 0; i < 150; ++i) feet = walker->moveAndSlide({2.0f, 0.0f, 0.0f}, dt);
    std::printf("slid round to x=%.3f z=%.3f\n", feet.x, feet.z);
    assert(feet.x > 3.0f && feet.z > 0.6f);
    std::printf("PASS: moveAndSlide stops at and slides round bodies\n");
    return 0;
}
