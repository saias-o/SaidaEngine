#include "scene/Scene.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"

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
    return 0;
}
