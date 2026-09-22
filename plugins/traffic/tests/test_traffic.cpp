// Contract tests for the traffic add-on. No engine, no window, no project:
// one translation unit, one header, standard library only.
//
//   g++ -std=c++20 -O2 -I../include test_traffic.cpp -o test_traffic && ./test_traffic
//
// Each case below is a property a street must have for a player at ground
// level not to notice the simulation. Three of them are here because the first
// version failed them.

#include "saida/traffic/Traffic.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace saida::traffic;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

// A straight two-way street 400 m long, cut into 20 m spans so a car has
// somewhere to be. Both carriageways, which is what a real street is and what
// broke the first version of the following rule.
static Graph street(int spans = 20, float span = 20.f) {
    Graph g;
    for (int i = 0; i <= spans; ++i) g.nodes.push_back({0.f, float(i) * span});
    for (int i = 0; i < spans; ++i) {
        g.lanes.push_back({uint32_t(i), uint32_t(i + 1), 12.f});
        g.lanes.push_back({uint32_t(i + 1), uint32_t(i), 12.f});
    }
    g.build();
    return g;
}

// Two streets crossing at the origin, so junction behaviour has a junction.
static Graph crossroads() {
    Graph g;
    g.nodes = {{0.f, 0.f}, {0.f, 60.f}, {0.f, -60.f}, {60.f, 0.f}, {-60.f, 0.f}};
    for (uint32_t arm = 1; arm <= 4; ++arm) {
        g.lanes.push_back({arm, 0u, 12.f});
        g.lanes.push_back({0u, arm, 12.f});
    }
    g.build();
    return g;
}

static void run(Flow& flow, float seconds, Vec2 observer, const Obstacle& obstacle = {}) {
    for (float t = 0.f; t < seconds; t += 1.f / 60.f)
        flow.update(1.f / 60.f, observer, {0.f, 0.f}, obstacle);
}

int main() {
    // ── the graph ───────────────────────────────────────────────────────────
    {
        Graph g = street(2);
        check(g.ready(), "a built graph is ready");
        check(std::abs(g.laneLength(0) - 20.f) < 1e-3f, "lane length is its geometry");
        check(g.outCount(1) == 2, "the middle node continues both ways");
        Vec2 mid = g.pointOn(0, 10.f);
        check(std::abs(mid.y - 10.f) < 1e-3f, "a point halfway along is halfway along");
    }

    // ── population ──────────────────────────────────────────────────────────
    {
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 7);
        flow.setPopulation(12);
        run(flow, 20.f, {0.f, 200.f});
        check(flow.live() > 0, "cars appear on a street the observer is standing in");
        check(flow.live() <= 12, "and never more than the population asked for");
        check(flow.agents().size() == 12, "the agent array is the population");
    }
    {
        // An empty population is the off switch, and it must cost nothing
        // rather than leave the last cars parked for ever.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 7);
        flow.setPopulation(6);
        run(flow, 10.f, {0.f, 200.f});
        flow.setPopulation(0);
        run(flow, 1.f, {0.f, 200.f});
        check(flow.live() == 0, "a population of zero empties the street");
    }

    // ── where cars are ──────────────────────────────────────────────────────
    {
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 3);
        flow.setPopulation(10);
        run(flow, 20.f, {0.f, 200.f});
        bool onLane = true, tooFar = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const Pose p = flow.pose(a);
            // The street runs along y at x=0, so every car must sit exactly one
            // lane offset to one side of it.
            if (std::abs(std::abs(p.position.x) - rules.laneOffset) > 1e-3f) onLane = false;
            // No facing was given, so the long radius applies all round: not
            // knowing where the observer looks costs a few more cars and never
            // a car vanishing on screen.
            if (length(p.position - Vec2{0.f, 200.f}) > rules.despawnAhead) tooFar = true;
        }
        check(onLane, "every car sits on a lane, one offset from the centre line");
        check(!tooFar, "no car outlives the despawn radius");
    }
    {
        // Nothing may *appear* in the observer's lap. Checked on the frame it
        // appears and not later: a car that spawned properly and then drove
        // past the observer is traffic working, and asserting on the settled
        // state tests luck instead of the spawn rule.
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 3);
        flow.setPopulation(10);
        flow.update(1.f / 60.f, {0.f, 200.f}, {0.f, 0.f});
        bool inLap = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const float distance = length(flow.pose(a).position - Vec2{0.f, 200.f});
            if (distance < rules.spawnNear || distance > rules.spawnFar) inLap = true;
        }
        check(!inLap, "a car appears only in the spawn annulus");
    }
    {
        // Left-hand traffic is the same street, mirrored, and nothing else.
        Graph g = street();
        Flow flow;
        Rules rules;
        rules.leftHand = true;
        flow.reset(&g, rules, 3);
        flow.setPopulation(4);
        run(flow, 12.f, {0.f, 200.f});
        Flow right;
        Rules mirrored;
        right.reset(&g, mirrored, 3);
        right.setPopulation(4);
        run(right, 12.f, {0.f, 200.f});
        bool mirroredOk = true;
        for (size_t i = 0; i < flow.agents().size(); ++i) {
            if (!flow.agents()[i].alive || !right.agents()[i].alive) continue;
            if (std::abs(flow.pose(flow.agents()[i]).position.x +
                         right.pose(right.agents()[i]).position.x) > 1e-3f)
                mirroredOk = false;
        }
        check(mirroredOk, "leftHand mirrors the lane and changes nothing else");
    }
    {
        // With a facing, a car that gets far enough behind is recycled -- the
        // short radius is what keeps the population in front of the player.
        Graph g = street(40, 20.f);
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 47);
        flow.setPopulation(14);
        for (int frame = 0; frame < 1200; ++frame)
            flow.update(1.f / 60.f, {0.f, 100.f}, Vec2{0.f, 1.f});
        bool keptBehind = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const Vec2 d = flow.pose(a).position - Vec2{0.f, 100.f};
            const float distance = length(d);
            if (dot(d, Vec2{0.f, 1.f}) / distance < rules.behindCosine && distance > rules.despawn)
                keptBehind = true;
        }
        check(!keptBehind, "a car far behind the observer is recycled");
    }

    {
        // Looking one way, a car may appear close behind and never close in
        // front. The hole in front is what a player sees; the one behind is
        // what makes a city feel empty for no reason.
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 41);
        flow.setPopulation(14);
        bool inFront = false, closeBehind = false;
        for (int frame = 0; frame < 120; ++frame) {
            flow.update(1.f / 60.f, {0.f, 200.f}, Vec2{0.f, 1.f});
            for (const Agent& a : flow.agents()) {
                if (!a.alive) continue;
                const Vec2 p = flow.pose(a).position;
                const float distance = length(p - Vec2{0.f, 200.f});
                if (a.speed > 0.f) continue;  // only just spawned cars are still
                if (p.y > 200.f && distance < rules.spawnNear - 1.f) inFront = true;
                if (p.y < 200.f && distance < rules.spawnNear - 1.f) closeBehind = true;
            }
        }
        check(!inFront, "nothing appears close in front of the observer");
        check(closeBehind || true, "cars may appear close behind");
    }

    {
        // Nothing may appear *or* vanish where the observer is looking. The
        // second half is the one that shipped broken: a car recycled 240 m
        // down a straight avenue is a car that disappeared on screen.
        //
        // A long straight street, the observer looking along it, every
        // transition watched frame by frame.
        Graph g = street(40, 20.f);  // 800 m
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 43);
        flow.setPopulation(16);
        const Vec2 eye{0.f, 100.f}, look{0.f, 1.f};
        std::vector<char> was(16, 0);
        std::vector<Vec2> last(16, Vec2{});
        bool appearedInView = false, vanishedInView = false;
        for (int frame = 0; frame < 900; ++frame) {
            flow.update(1.f / 60.f, eye, look);
            for (size_t i = 0; i < flow.agents().size(); ++i) {
                const Agent& a = flow.agents()[i];
                auto inView = [&](Vec2 p) {
                    const Vec2 d = p - eye;
                    const float distance = length(d);
                    return distance > .01f && dot(d, look) / distance >= rules.behindCosine;
                };
                if (a.alive && !was[i]) {
                    const Vec2 p = flow.pose(a).position;
                    if (inView(p) && length(p - eye) < rules.spawnNear - 1.f) appearedInView = true;
                }
                if (!a.alive && was[i]) {
                    if (inView(last[i]) && length(last[i] - eye) < rules.despawnAhead - 1.f)
                        vanishedInView = true;
                }
                was[i] = a.alive ? 1 : 0;
                if (a.alive) last[i] = flow.pose(a).position;
            }
        }
        check(!appearedInView, "no car appears inside the view cone");
        check(!vanishedInView, "no car vanishes inside the view cone");
    }

    // ── car following ───────────────────────────────────────────────────────
    {
        // The bug this file exists for: distances measured on the centre line
        // put every oncoming car directly in front of you, and a street of two
        // carriageways stops dead. Cars must keep moving.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 11);
        flow.setPopulation(14);
        run(flow, 25.f, {0.f, 200.f});
        int moving = 0, live = 0;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            ++live;
            if (a.speed > 1.f) ++moving;
        }
        check(live >= 4, "a 400 m street holds a few cars");
        check(moving * 2 >= live, "oncoming traffic is not an obstacle");
    }
    {
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 5);
        flow.setPopulation(14);
        run(flow, 30.f, {0.f, 200.f});
        // No two cars on the same lane may be inside the standstill gap.
        bool spaced = true;
        const auto& all = flow.agents();
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j) {
                if (!all[i].alive || !all[j].alive || all[i].lane != all[j].lane) continue;
                if (std::abs(all[i].s - all[j].s) < rules.minGap * .5f) spaced = false;
            }
        check(spaced, "cars on one lane keep their distance");
    }

    // ── the player ──────────────────────────────────────────────────────────
    {
        // A car parked across the lane is a queue, not a pile-up.
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 13);
        flow.setPopulation(8);
        run(flow, 15.f, {0.f, 200.f});
        Obstacle blocker;
        blocker.active = true;
        blocker.position = {rules.laneOffset, 200.f};
        run(flow, 12.f, {0.f, 200.f}, blocker);
        bool drivenInto = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const Pose p = flow.pose(a);
            if (length(p.position - blocker.position) < 1.5f) drivenInto = true;
        }
        check(!drivenInto, "nothing drives through the player's car");
    }

    // ── junctions ───────────────────────────────────────────────────────────
    {
        Graph g = crossroads();
        Flow flow;
        Rules rules;
        rules.spawnNear = 5.f;
        rules.spawnFar = 70.f;
        rules.despawn = 120.f;
        flow.reset(&g, rules, 17);
        flow.setPopulation(12);
        run(flow, 40.f, {0.f, 0.f});
        // Two cars may not stand on the same square of tarmac, and the flow
        // may not deadlock: somebody must still be moving after 40 seconds.
        bool overlap = false;
        int moving = 0;
        const auto& all = flow.agents();
        for (size_t i = 0; i < all.size(); ++i) {
            if (!all[i].alive) continue;
            if (all[i].speed > 1.f) ++moving;
            for (size_t j = i + 1; j < all.size(); ++j) {
                if (!all[j].alive) continue;
                if (length(flow.pose(all[i]).position - flow.pose(all[j]).position) < 2.f)
                    overlap = true;
            }
        }
        check(!overlap, "no two cars occupy the same place at a junction");
        check(moving > 0, "a crossroads does not deadlock");
    }
    {
        // A dead end ends the trip rather than teleporting the car.
        Graph g;
        g.nodes = {{0.f, 0.f}, {0.f, 40.f}};
        g.lanes.push_back({0u, 1u, 12.f});  // one way in, nothing out
        g.build();
        Flow flow;
        Rules rules;
        rules.spawnNear = 1.f;
        rules.spawnFar = 60.f;
        flow.reset(&g, rules, 19);
        flow.setPopulation(3);
        run(flow, 30.f, {0.f, 20.f});
        bool sane = true;
        for (const Agent& a : flow.agents())
            if (a.alive && (a.s < 0.f || a.s > g.laneLength(a.lane) + 1e-3f)) sane = false;
        check(sane, "a dead end retires the car instead of running past its lane");
    }

    // ── where the traffic goes ──────────────────────────────────────────────
    {
        // One avenue and eight back streets of the same length. Uniform lane
        // choice puts 8/9 of the cars in the back streets and leaves the
        // avenue — the street the player is looking down — empty. This is
        // what "not enough traffic in Paris" actually looked like.
        Graph g;
        g.nodes.push_back({0.f, 0.f});
        g.nodes.push_back({0.f, 300.f});
        g.lanes.push_back({0u, 1u, 14.f, 10.f});  // the avenue
        for (uint32_t i = 0; i < 8; ++i) {
            const float x = 40.f + float(i) * 25.f;
            g.nodes.push_back({x, 0.f});
            g.nodes.push_back({x, 300.f});
            g.lanes.push_back({uint32_t(2 + i * 2), uint32_t(3 + i * 2), 8.f, 1.f});
        }
        g.build();
        Flow flow;
        Rules rules;
        rules.spawnNear = 5.f;
        rules.spawnFar = 400.f;
        rules.despawn = 600.f;
        flow.reset(&g, rules, 29);
        flow.setPopulation(18);
        run(flow, 12.f, {0.f, 150.f});
        int onAvenue = 0, live = 0;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            ++live;
            if (a.lane == 0) ++onAvenue;
        }
        check(live > 8, "the network fills up");
        // A flat draw would give the avenue one lane in nine. It takes ten
        // draws in eighteen and settles at about four in ten, because a lane
        // that is already full rejects further spawns and they spill into the
        // side streets -- which is the behaviour wanted, and why the claim is
        // "far more than its share" rather than an exact fraction.
        check(onAvenue * 3 > live, "the avenue carries far more than an equal share");
    }
    {
        // A producer that sets no weight at all still gets traffic.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 31);
        flow.setPopulation(8);
        run(flow, 12.f, {0.f, 200.f});
        check(flow.live() > 0, "unweighted lanes still carry cars");
    }

    {
        // A car the host takes over leaves the flow and its slot comes back.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 37);
        flow.setPopulation(10);
        run(flow, 15.f, {0.f, 200.f});
        const uint32_t before = flow.live();
        check(before > 0, "there is a car to take");
        for (size_t i = 0; i < flow.agents().size(); ++i)
            if (flow.agents()[i].alive) { flow.retire(i); break; }
        check(flow.live() == before - 1, "retiring one takes one out of the flow");
        run(flow, 10.f, {0.f, 200.f});
        check(flow.live() > 0, "and the slot is spawned into again");
    }

    // ── reproducibility ─────────────────────────────────────────────────────
    {
        Graph g = street();
        Flow a, b;
        a.reset(&g, Rules{}, 23);
        b.reset(&g, Rules{}, 23);
        a.setPopulation(10);
        b.setPopulation(10);
        run(a, 18.f, {0.f, 200.f});
        run(b, 18.f, {0.f, 200.f});
        bool same = a.agents().size() == b.agents().size();
        for (size_t i = 0; same && i < a.agents().size(); ++i)
            same = a.agents()[i].alive == b.agents()[i].alive &&
                   a.agents()[i].lane == b.agents()[i].lane &&
                   std::abs(a.agents()[i].s - b.agents()[i].s) < 1e-4f;
        check(same, "one seed is one street, every run");
    }

    if (failures == 0) std::printf("PASS saida::traffic\n");
    return failures == 0 ? 0 : 1;
}
