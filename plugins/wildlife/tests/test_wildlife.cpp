// Contract tests for the wildlife add-on. No engine, no window, no project:
// one translation unit, one header, standard library only.
//
//   g++ -std=c++17 -O2 -Wall -I../include test_wildlife.cpp -o test_wildlife && ./test_wildlife
//
// Each case is something a walker would notice: an animal popping up in
// front of him, a deer walking into a lake, a herd that ignores him, a lizard
// that crawls without legs, a group scattered over a valley.

#include "saida/wildlife/Wildlife.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace saida::wildlife;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static Kind deer() {
    Kind k;
    k.walkSpeed = 1.2f;
    k.runSpeed = 2.5f;
    k.fleeSpeed = 6.f;
    k.fleeDistance = 40.f;
    k.spread = 4.f;
    k.roam = 50.f;
    k.idleMin = 2.f;
    k.idleMax = 6.f;
    k.canEat = true;
    k.viewRange = 120.f;
    return k;
}

static Kind lizard() {
    Kind k;
    k.flee = FleeStyle::Dart;
    k.fleeDistance = 3.f;
    k.fleeSpeed = 2.f;
    k.spread = 1.f;
    k.roam = 2.f;
    k.viewRange = 12.f;
    k.radius = 0.05f;
    return k;
}

static void run(Herds& herds, Scene& scene, float seconds) {
    for (float t = 0.f; t < seconds; t += 1.f / 30.f) herds.update(1.f / 30.f, scene);
}

int main() {
    {
        Herds h;
        h.reset({deer()}, {Group{{0,0},{0,0,0},7}}, 4, 11);
        Scene s;s.observer={60,0};s.facing={1,0};run(h,s,1);
        check(h.live()==3,"resident retirement fixture spawns all members");
        const auto victim=h.animals().front();
        check(h.setMemberEnabled(victim.group,victim.member,false),"a resident can be disabled");
        check(h.live()==2,"disabling removes exactly one living resident");
        check(!h.setMemberEnabled(99,0,false)&&!h.setMemberEnabled(0,99,false),"invalid resident indices are refused");
        s.observer={1000,0};run(h,s,10);s.observer={60,0};run(h,s,10);
        check(h.live()==2,"a disabled resident stays gone after visibility retirement and respawn");
        for(const auto& a:h.animals())check(!a.alive||a.member!=victim.member,"surviving member indices do not shift");
        h.reset({deer()}, {Group{{0,0},{0,0,0},7}}, 4, 11);
        h.setMemberEnabled(0,victim.member,false);run(h,s,1);
        check(h.live()==2,"host persistence can restore disabled residents before the first spawn");
        h.setMemberEnabled(0,0,false);h.setMemberEnabled(0,1,false);h.setMemberEnabled(0,2,false);
        run(h,s,60);check(h.live()==0,"a wholly disabled group stays empty indefinitely");
        Kind near=deer(),far=deer();near.viewRange=10;near.roam=1;far.viewRange=120;far.roam=1;
        h.reset({near,far},{Group{{0,0},{0,1},9}},4,11);h.setMemberEnabled(0,0,false);run(h,s,1);
        check(h.live()==1,"spawning uses the surviving leader's range rather than a disabled member's kind");
    }
    // Nothing is spawned beyond the view range, a group spawns whole once
    // the observer comes within it, and the budget caps it.
    {
        Herds h;
        Group g{{0.f, 0.f}, {0, 0, 0, 0, 0}, 7};
        h.reset({deer()}, {g}, 3, 11);
        Scene s;
        s.observer = {500.f, 0.f};
        run(h, s, 1.f);
        check(h.live() == 0, "no animal beyond the view range");
        s.observer = {60.f, 0.f};
        s.facing = {1.f, 0.f};  // looking away from the group
        run(h, s, 0.5f);
        check(h.live() == 3, "a group spawns within view range, up to the budget");
        check(h.spawned(0), "the group is marked spawned");
    }
    // A short budget goes to the nearest group, not the first one listed: a
    // far herd must not keep the deer at the walker's feet from coming.
    {
        Kind k = deer();
        k.roam = 1.f;
        Herds h;
        h.reset({k}, {Group{{110.f, 0.f}, {0, 0}, 3}, Group{{60.f, 0.f}, {0, 0}, 4}}, 2, 7);
        Scene s;
        s.observer = {0.f, 0.f};
        s.facing = {-1.f, 0.f};
        run(h, s, 0.5f);
        check(h.live() == 2 && h.spawned(1) && !h.spawned(0), "the nearest group takes a short budget");
    }
    // Never out of nowhere in plain sight close by: facing the home at 20 m
    // spawns nothing; turning away lets it come.
    {
        Kind still = deer();
        still.roam = 2.f;          // where it is: right ahead of the observer
        still.fleeDistance = 10.f;  // and outside its flight distance
        Herds h;
        h.reset({still}, {Group{{0.f, 0.f}, {0, 0}, 1}}, 8, 3);
        Scene s;
        s.observer = {0.f, -20.f};
        s.facing = {0.f, 1.f};
        run(h, s, 2.f);
        check(h.live() == 0, "no spawn in plain sight near the observer");
        s.facing = {0.f, -1.f};
        run(h, s, 0.2f);
        check(h.live() == 2, "spawned once the observer looks away");
    }
    // Habitat: over ten minutes nobody stands on forbidden ground (a lake
    // east of x = 20), and calm animals stay near home.
    {
        Herds h;
        h.reset({deer()}, {Group{{0.f, 0.f}, {0, 0, 0, 0}, 5}}, 8, 5);
        h.setAllowed([](uint16_t, Vec2 p) { return p.x < 20.f; });
        Scene s;
        s.observer = {-60.f, 0.f};
        s.facing = {-1.f, 0.f};
        bool dry = true, near = true, walked = false;
        std::vector<Vec2> start;
        for (float t = 0.f; t < 600.f; t += 1.f / 15.f) {
            h.update(1.f / 15.f, s);
            for (const auto& a : h.animals())
                if (a.alive) {
                    dry = dry && a.position.x < 20.f;
                    near = near && distance(a.position, {0.f, 0.f}) < 50.f * 1.5f;
                    walked = walked || a.state == State::Walk;
                }
        }
        check(dry, "animals never step onto forbidden ground");
        check(near, "calm animals stay within their roam of home");
        check(walked, "calm animals do walk about");
    }
    // A group keeps together: followers stay near their leader.
    {
        Herds h;
        h.reset({deer()}, {Group{{0.f, 0.f}, {0, 0, 0, 0, 0, 0}, 9}}, 8, 9);
        Scene s;
        s.observer = {-60.f, 0.f};
        s.facing = {-1.f, 0.f};
        float widest = 0.f;
        for (float t = 0.f; t < 300.f; t += 1.f / 15.f) {
            h.update(1.f / 15.f, s);
            const Animal* lead = nullptr;
            for (const auto& a : h.animals())
                if (a.alive && a.member == 0) lead = &a;
            if (!lead) continue;
            for (const auto& a : h.animals())
                if (a.alive) widest = std::max(widest, distance(a.position, lead->position));
        }
        check(widest > 1.f && widest < 4.f * 12.f, "a herd stays together");
    }
    // Flight: a walker inside the flee distance sends the whole group away,
    // faster than it walks, and it calms down after a while.
    {
        Herds h;
        h.reset({deer()}, {Group{{0.f, 0.f}, {0, 0, 0}, 13}}, 8, 13);
        Scene s;
        s.observer = {-60.f, 0.f};
        s.facing = {-1.f, 0.f};
        run(h, s, 1.f);
        check(h.live() == 3, "deer spawned for the flight test");
        float before = 1e9f;
        for (const auto& a : h.animals())
            if (a.alive) before = std::min(before, distance(a.position, {-10.f, 0.f}));
        s.threats = {Threat{{-10.f, 0.f}, 1.f}};
        bool fled = false, fast = false;
        for (float t = 0.f; t < 3.f; t += 1.f / 30.f) {
            h.update(1.f / 30.f, s);
            for (const auto& a : h.animals())
                if (a.alive && a.state == State::Flee) {
                    fled = true;
                    fast = fast || a.speed > 3.f;
                }
        }
        float after = 1e9f;
        for (const auto& a : h.animals())
            if (a.alive) after = std::min(after, distance(a.position, {-10.f, 0.f}));
        check(fled, "a threat inside the flee distance scares the group");
        check(fast, "fleeing is faster than walking");
        check(after > before + 5.f, "fleeing animals get away from the threat");
        s.threats.clear();
        run(h, s, 20.f);
        bool calm = true;
        for (const auto& a : h.animals())
            if (a.alive && a.state == State::Flee) calm = false;
        check(calm, "the group calms down once the threat is gone");
    }
    // An animal without a gait never moves; a small one darts and is gone.
    {
        Herds h;
        h.reset({lizard()}, {Group{{0.f, 0.f}, {0}, 21}}, 4, 21);
        Scene s;
        s.observer = {0.f, -8.f};
        s.facing = {0.f, -1.f};
        run(h, s, 0.5f);
        check(h.live() == 1, "the lizard spawns within its short view range");
        Vec2 at{};
        uint64_t identity = 0;
        for (const auto& a : h.animals())
            if (a.alive) at = a.position, identity = a.identity;
        run(h, s, 30.f);
        bool still = true;
        for (const auto& a : h.animals())
            if (a.alive) still = still && distance(a.position, at) < 1e-4f;
        check(still, "an animal without a gait stays put");
        s.threats = {Threat{at + Vec2{0.f, -1.f}, 1.f}};
        bool darted = false;
        for (float t = 0.f; t < 2.f; t += 1.f / 30.f) {
            h.update(1.f / 30.f, s);
            for (const auto& a : h.animals())
                if (a.alive && a.state == State::Dart) darted = true;
        }
        check(darted, "a scared small animal darts");
        check(h.live() == 0, "and is gone into cover");
        s.threats.clear();
        run(h, s, 30.f);
        check(h.live() == 0, "it does not come straight back");
        (void)identity;
    }
    // Leaving: the group is dropped once the observer is far, and comes back
    // as the same group, a new identity in its slot.
    {
        Herds h;
        h.reset({deer()}, {Group{{0.f, 0.f}, {0, 0}, 31}}, 4, 31);
        Scene s;
        s.observer = {60.f, 0.f};
        s.facing = {1.f, 0.f};
        run(h, s, 0.5f);
        check(h.live() == 2, "spawned before leaving");
        uint64_t first = 0;
        for (const auto& a : h.animals())
            if (a.alive && a.member == 0) first = a.identity;
        s.observer = {400.f, 0.f};
        run(h, s, 1.f);
        check(h.live() == 0, "dropped once the observer is far");
        s.observer = {60.f, 0.f};
        run(h, s, 10.f);
        uint64_t second = 0;
        for (const auto& a : h.animals())
            if (a.alive && a.member == 0) second = a.identity;
        check(h.live() == 2 && second != first, "back as the same group, a new identity");
    }
    // A group lives across its roam: met far from its home when one passes
    // where it is, and not met at all from most of its range.
    {
        Kind wide = deer();
        wide.roam = 300.f;
        wide.viewRange = 80.f;
        int met = 0, far = 0;
        for (uint64_t id = 1; id <= 200; ++id) {
            Herds h;
            h.reset({wide}, {Group{{0.f, 0.f}, {0}, id}}, 2, 77);
            Scene s;
            s.observer = {200.f, 0.f};
            s.facing = {1.f, 0.f};
            run(h, s, 0.2f);
            if (h.live()) {
                ++met;
                for (const auto& a : h.animals())
                    if (a.alive && distance(a.position, {0.f, 0.f}) > 110.f) ++far;
            }
        }
        check(met > 5 && met < 120, "a group in a wide roam is met now and then, not always");
        check(far == met, "and met where it is, far from its home");
    }
    // An inactive group (its hour) is not spawned.
    {
        Herds h;
        h.reset({deer()}, {Group{{0.f, 0.f}, {0}, 41}}, 4, 41);
        h.setActive(0, false);
        Scene s;
        s.observer = {50.f, 0.f};
        s.facing = {1.f, 0.f};
        run(h, s, 2.f);
        check(h.live() == 0, "an inactive group stays away");
    }
    // A kind that cannot stand keeps walking; an air kind keeps its height.
    {
        Kind walker = deer();
        walker.canStand = false;
        walker.fleeDistance = 0.f;
        Kind flyer;
        flyer.walkSpeed = 0.8f;
        flyer.canStand = false;
        flyer.surface = Surface::Air;
        flyer.hoverLow = 0.5f;
        flyer.hoverHigh = 2.f;
        flyer.roam = 6.f;
        flyer.legMin = 1.f;
        flyer.legMax = 4.f;
        flyer.viewRange = 20.f;
        Herds h;
        h.reset({walker, flyer}, {Group{{0.f, 0.f}, {0}, 51}, Group{{0.f, 30.f}, {1}, 52}}, 4, 51);
        Scene s;
        s.observer = {0.f, 15.f};
        s.facing = {1.f, 0.f};
        bool moving = true, aloft = true;
        int frames = 0;
        for (float t = 0.f; t < 60.f; t += 1.f / 30.f) {
            h.update(1.f / 30.f, s);
            for (const auto& a : h.animals()) {
                if (!a.alive) continue;
                if (a.kind == 0 && ++frames > 30) moving = moving && a.state == State::Walk;
                if (a.kind == 1) aloft = aloft && a.height >= 0.49f && a.height <= 2.01f;
            }
        }
        check(moving, "a kind without a standing clip keeps walking");
        check(aloft, "an air kind stays within its hover band");
    }
    // Determinism: the same seed and groups place the same animals.
    {
        auto place = [] {
            Herds h;
            h.reset({deer()}, {Group{{0.f, 0.f}, {0, 0, 0}, 61}}, 4, 99);
            Scene s;
            s.observer = {60.f, 0.f};
            s.facing = {1.f, 0.f};
            run(h, s, 5.f);
            std::vector<float> out;
            for (const auto& a : h.animals()) out.push_back(a.position.x), out.push_back(a.position.y);
            return out;
        };
        check(place() == place(), "the same groups and seed draw the same animals");
    }
    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("PASS saida::wildlife\n");
    return 0;
}
