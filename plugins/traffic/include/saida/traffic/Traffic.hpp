// Saida Traffic — ambient road traffic over a lane graph.
//
// A standalone, header-only add-on in the sense of `plugins/python-tools`: it
// is not linked into `saida_engine`, it is absent from CMake, and a project
// that does not include this header is unaffected by it. A project that does
// include it compiles it into its own binary, which is what lets a game link
// against a prebuilt engine and still gain traffic.
//
// **It knows nothing about the engine, and that is the point.** No scene, no
// node, no material, no JSON, no geodesy, not even glm. It owns a lane graph
// and a population of agents driving on it, and it hands back poses. The host
// decides what a pose looks like, where the graph came from, and when to draw.
// That seam is what makes it testable without a window (`tests/`) and reusable
// by a hand-built city as readily as by a streamed planet.
//
// The model is deliberately the one the sixth-generation open-world games
// shipped: agents on a directed lane graph, a car-following rule, a give-way
// rule at junctions, and spawning in an annulus around the observer so traffic
// exists where it is seen and nowhere else. There is no path planning, no
// destination, no signals and no lane changing. Each of those is a feature
// somebody can add on top of this; none of them is needed for a street to stop
// looking abandoned, and every one of them costs frame time on every car.
//
// Units are metres, seconds and radians throughout. The plane is the host's:
// pass whatever two axes the ground lies in and read the poses back in the
// same ones.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace saida::traffic {

struct Vec2 {
    float x = 0.f, y = 0.f;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float k) { return {a.x * k, a.y * k}; }
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 a) { return std::sqrt(dot(a, a)); }

// One directed lane, from one graph node to another. A two-way street is two
// lanes; a one-way street is one. The distinction is the producer's to make,
// because only the producer knows what the road actually is.
struct Lane {
    uint32_t from = 0, to = 0;
    float speed = 13.9f;   // m/s the lane is driven at: a limit, or its estimate
    // How much of this network's traffic this lane carries, relative to the
    // others. Spawning uniformly over lanes is what an empty boulevard looks
    // like: a city has far more back streets than avenues, so uniform choice
    // puts the whole population in the back streets and leaves the avenue —
    // the one street the player is actually looking down — empty. Only the
    // producer knows which is which, so only the producer can say.
    float weight = 1.f;
};

// A road network in one coordinate frame. Nodes are junctions and shape
// points; lanes connect them. `build()` turns the lane list into the adjacency
// the simulation reads, and must be called before the graph is driven on.
//
// The graph is immutable while agents drive on it. A host that streams road
// data gives each streamed region its own graph and its own `Flow`, which is
// also how a region can be discarded without telling every agent about it.
class Graph {
public:
    std::vector<Vec2> nodes;
    std::vector<Lane> lanes;

    void build() {
        lengths_.resize(lanes.size());
        for (size_t i = 0; i < lanes.size(); ++i)
            lengths_[i] = length(nodes[lanes[i].to] - nodes[lanes[i].from]);
        // CSR adjacency: outgoing lanes grouped by their origin node. Built
        // once rather than searched per junction, because a car reaching a
        // junction is the one moment in this simulation that happens on a
        // frame boundary for every car at once.
        offsets_.assign(nodes.size() + 1, 0);
        for (const Lane& lane : lanes) ++offsets_[lane.from + 1];
        for (size_t i = 1; i < offsets_.size(); ++i) offsets_[i] += offsets_[i - 1];
        outgoing_.resize(lanes.size());
        std::vector<uint32_t> cursor(offsets_.begin(), offsets_.end() - 1);
        for (uint32_t i = 0; i < lanes.size(); ++i) outgoing_[cursor[lanes[i].from]++] = i;
        // Cumulative weight x length, so a lane is chosen in proportion to how
        // much traffic it carries *and* how much room it has to carry it. A
        // 400 m avenue holds more cars than a 20 m block of the same class,
        // and choosing per lane rather than per metre would forget that.
        cumulative_.resize(lanes.size());
        float total = 0.f;
        for (size_t i = 0; i < lanes.size(); ++i) {
            const float w = lanes[i].weight > 0.f ? lanes[i].weight : 0.f;
            total += w * lengths_[i];
            cumulative_[i] = total;
        }
    }

    bool ready() const { return lengths_.size() == lanes.size() && !offsets_.empty(); }
    float laneLength(uint32_t lane) const { return lengths_[lane]; }
    Vec2 pointOn(uint32_t lane, float s) const {
        const Lane& l = lanes[lane];
        const float len = lengths_[lane];
        const float t = len > 0.f ? s / len : 0.f;
        return nodes[l.from] + (nodes[l.to] - nodes[l.from]) * t;
    }
    Vec2 direction(uint32_t lane) const {
        const Lane& l = lanes[lane];
        Vec2 d = nodes[l.to] - nodes[l.from];
        const float len = lengths_[lane];
        return len > 0.f ? d * (1.f / len) : Vec2{1.f, 0.f};
    }
    // The lane a uniform draw in [0,1) lands on, weighted. Falls back to a
    // flat choice when every weight is zero, so a producer that sets none
    // still gets traffic.
    uint32_t laneAt(float u) const {
        const float total = cumulative_.empty() ? 0.f : cumulative_.back();
        if (total <= 0.f) return uint32_t(u * float(lanes.size())) % uint32_t(lanes.size());
        const float target = u * total;
        uint32_t low = 0, high = uint32_t(cumulative_.size()) - 1;
        while (low < high) {
            const uint32_t mid = (low + high) / 2;
            if (cumulative_[mid] < target) low = mid + 1; else high = mid;
        }
        return low;
    }
    uint32_t outCount(uint32_t node) const { return offsets_[node + 1] - offsets_[node]; }
    uint32_t outLane(uint32_t node, uint32_t i) const { return outgoing_[offsets_[node] + i]; }

private:
    std::vector<float> lengths_, cumulative_;
    std::vector<uint32_t> offsets_, outgoing_;
};

// How this world's traffic behaves. Every field is a property of the place or
// of the vehicle, not of the renderer, so a host can hand different rules to
// different regions — which is what drives on the left, and how fast.
struct Rules {
    bool leftHand = false;     // which side of the centre line the lane sits on
    float laneOffset = 1.7f;   // metres from the centre line to the lane centre
    float accel = 2.6f;        // m/s^2
    float brake = 5.5f;        // m/s^2
    float headway = 1.6f;      // seconds of gap a driver keeps to the car ahead
    float minGap = 6.5f;       // metres of gap at a standstill, nose to nose
    float junctionGuard = 9.f; // metres around a junction another car reserves
    float spawnNear = 45.f;    // no car appears in front closer than this
    // Behind the observer, a car may appear much closer: nobody sees it, and
    // the alternative is a hole around the player that reads as an empty city
    // however many cars the network holds. The host says which way the
    // observer is looking; when it does not, this is ignored and `spawnNear`
    // applies all round.
    float spawnNearBehind = 16.f;
    // What counts as "behind". It must be behind the *frustum*, not behind the
    // shoulder: a 16:9 camera at a 62-degree vertical field sees about 100
    // degrees across, so anything within 50 degrees of the view axis is on
    // screen. A cosine of 0.2 (78 degrees) called a car behind the observer
    // while it was plainly in the corner of the picture, which is what
    // "sometimes cars spawn in my field of view" was.
    float behindCosine = -.25f;   // ~104 degrees off the view axis
    float spawnFar = 190.f;       // nor further away than this
    // Two despawn radii for the same reason there are two spawn radii. A car
    // vanishing 240 m down a straight avenue is visible; one vanishing 150 m
    // behind the observer is not.
    float despawn = 150.f;        // behind the observer
    float despawnAhead = 420.f;   // in front of it, past where fog hides a car
    uint32_t maxSpawnsPerUpdate = 2;  // a street fills in over a second, not in one frame
};

// One driver. `lane` and `s` are the whole of its position; everything else a
// host wants (a model, a colour, a node) belongs to the host and is keyed by
// the agent's index, which is stable for as long as the agent is alive.
struct Agent {
    uint32_t lane = 0;
    float s = 0.f;      // metres travelled along `lane`
    float speed = 0.f;  // m/s
    bool alive = false;
    uint32_t seed = 1;  // this driver's own stream of choices at junctions
};

struct Pose {
    Vec2 position;
    float yaw = 0.f;    // radians, atan2(dir.x, dir.y): 0 points along +y
    float speed = 0.f;
};

// Something the traffic must not drive into that the traffic does not own —
// the player's vehicle, in practice. It is a circle with a heading, and it is
// tested exactly as another agent would be, so a player stopped across a lane
// makes a queue rather than a pile-up.
struct Obstacle {
    Vec2 position;
    float radius = 2.2f;
    bool active = false;
};

// The population driving on one graph.
//
// Cost per update is O(n^2) in the number of *live* agents, deliberately: with
// the tens of cars a street can show, a uniform grid costs more to maintain
// than the comparisons it saves, and the constant here is two subtractions and
// a dot product. A host that wants thousands wants a different class.
class Flow {
public:
    void reset(const Graph* graph, const Rules& rules, uint32_t seed = 1) {
        graph_ = graph;
        rules_ = rules;
        random_ = seed ? seed : 1;
        agents_.clear();
        population_ = 0;
    }
    // How many cars this network should show. Changing it is how a host makes
    // a dense district busier than a village without touching anything else.
    void setPopulation(uint32_t cars) { population_ = cars; }
    uint32_t population() const { return population_; }
    // Take one agent out of the flow. The host owns what happens next -- the
    // case this exists for is a player getting into that very car and driving
    // it away, at which point the simulation must stop having an opinion about
    // where it is. The slot is reused by the next spawn.
    void retire(size_t index) {
        if (index < agents_.size()) agents_[index].alive = false;
    }
    const std::vector<Agent>& agents() const { return agents_; }
    uint32_t live() const {
        uint32_t n = 0;
        for (const Agent& a : agents_) n += a.alive ? 1 : 0;
        return n;
    }

    Pose pose(const Agent& agent) const {
        return {positionOf(agent.lane, agent.s), std::atan2(graph_->direction(agent.lane).x,
                                                            graph_->direction(agent.lane).y),
                agent.speed};
    }

    // Where a car on this lane actually is: beside the centre line, on the
    // side this country drives on.
    //
    // **Every distance in this file is measured between these, never between
    // centre-line points.** A two-way street is two lanes over one line, so a
    // car measured on the line has every oncoming car sitting directly in
    // front of it — and a flow that brakes for oncoming traffic stops dead at
    // the first car it meets. Right of travel is (dir.y, -dir.x) in a plane
    // whose yaw turns that way; the host's plane decides the sign, and
    // `leftHand` is the same switch either way.
    Vec2 positionOf(uint32_t lane, float s) const {
        const Vec2 dir = graph_->direction(lane);
        const Vec2 right{dir.y, -dir.x};
        return graph_->pointOn(lane, s) + right * (rules_.leftHand ? -rules_.laneOffset
                                                                   : rules_.laneOffset);
    }

    // `facing` is the unit vector the observer is looking along, or {0,0} when
    // the host does not track one. It is not defaulted and there is no shorter
    // overload: `update(dt, where, obstacle)` and `update(dt, where, facing)`
    // are ambiguous to the compiler and, worse, to the reader.
    void update(float dt, Vec2 observer, Vec2 facing, const Obstacle& obstacle = {}) {
        if (!graph_ || !graph_->ready() || graph_->lanes.empty()) return;
        agents_.resize(population_);
        const bool looking = facing.x != 0.f || facing.y != 0.f;
        // Every car's position, once per step instead of once per comparison.
        // `positionOf` normalises the lane to find its side of the road, so
        // reading it inside the O(n^2) neighbour loops cost a square root per
        // pair; this makes it a square root per car. It also makes the step
        // simultaneous -- every driver sees the same instant -- where before,
        // a car late in the array reacted to cars that had already moved.
        refreshPositions();
        for (size_t i = 0; i < agents_.size(); ++i) {
            if (!agents_[i].alive) continue;
            drive(i, dt, obstacle);
        }
        refreshPositions();
        for (size_t i = 0; i < agents_.size(); ++i) {
            if (!agents_[i].alive) continue;
            const Vec2 delta = positions_[i] - observer;
            const float distance = length(delta);
            if (distance > despawnLimit(delta, distance, facing, looking)) agents_[i].alive = false;
        }
        spawn(observer, facing, looking);
    }

private:
    // xorshift32. A named, reproducible stream rather than the global RNG:
    // traffic that cannot be replayed cannot be tested, and a host that seeds
    // two regions the same must get the same two streets.
    static uint32_t next(uint32_t& state) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    static float unit(uint32_t& state) { return float(next(state) & 0xFFFFFF) / float(0x1000000); }

    void refreshPositions() {
        positions_.resize(agents_.size());
        for (size_t i = 0; i < agents_.size(); ++i)
            if (agents_[i].alive) positions_[i] = positionOf(agents_[i].lane, agents_[i].s);
    }
    // Distance to whatever is in front of this agent on its own lane, and on
    // the lane it is about to enter. Returns a large number when the road is
    // clear. `self` is an index into the flow, or `kNobody` for a car that is
    // not in it yet -- a spawn candidate.
    static constexpr size_t kNobody = size_t(-1);
    float clearAhead(size_t self, uint32_t lane, float s, const Obstacle& obstacle) const {
        const Vec2 head = positionOf(lane, s);
        const Vec2 dir = graph_->direction(lane);
        float nearest = 1e9f;
        for (size_t j = 0; j < agents_.size(); ++j) {
            const Agent& other = agents_[j];
            if (j == self || !other.alive) continue;
            const Vec2 delta = positions_[j] - head;
            const float along = dot(delta, dir);
            if (along <= 0.f) continue;
            // Only what is in front and roughly in this lane's corridor: a car
            // on the opposite carriageway is not an obstacle, and a car on a
            // crossing street is handled by the junction rule below.
            const float lateral = std::abs(delta.x * dir.y - delta.y * dir.x);
            if (lateral > rules_.laneOffset * 1.6f) continue;
            if (along < nearest) nearest = along;
        }
        if (obstacle.active) {
            const Vec2 delta = obstacle.position - head;
            const float along = dot(delta, dir);
            const float lateral = std::abs(delta.x * dir.y - delta.y * dir.x);
            if (along > 0.f && lateral < rules_.laneOffset + obstacle.radius && along < nearest)
                nearest = along;
        }
        return nearest;
    }

    // Is this point behind the observer? False whenever the host does not say
    // which way it is looking, so an unaware host gets the safe answer
    // everywhere rather than a guess.
    bool isBehind(Vec2 delta, float distance, Vec2 facing, bool looking) const {
        return looking && distance > .01f &&
               dot(delta, facing) / distance < rules_.behindCosine;
    }
    // How far a car may get before it is recycled. Short behind the observer,
    // long in front of it -- and long everywhere when there is no facing, so
    // the cost of not knowing is a few more cars and never a car vanishing on
    // screen.
    float despawnLimit(Vec2 delta, float distance, Vec2 facing, bool looking) const {
        return isBehind(delta, distance, facing, looking) ? rules_.despawn : rules_.despawnAhead;
    }
    // Give way: whoever is nearest the junction goes first. It is not a
    // priority rule and does not pretend to be one — it is what keeps two cars
    // from occupying the same square of tarmac, which is the only thing a
    // player at street level can actually see.
    bool mustYield(size_t self) const {
        const Agent& agent = agents_[self];
        const float toJunction = graph_->laneLength(agent.lane) - agent.s;
        if (toJunction > rules_.junctionGuard) return false;
        const Vec2 junction = graph_->nodes[graph_->lanes[agent.lane].to];
        for (size_t j = 0; j < agents_.size(); ++j) {
            const Agent& other = agents_[j];
            if (j == self || !other.alive || other.lane == agent.lane) continue;
            const float theirs = length(positions_[j] - junction);
            if (theirs >= rules_.junctionGuard) continue;
            // Distance orders the cars strictly, so there is no cycle of cars
            // each waiting for the next. Two at exactly the same distance
            // would be one, and the index breaks that tie rather than leaving
            // both stopped for ever.
            if (theirs < toJunction || (theirs == toJunction && j < self)) return true;
        }
        return false;
    }

    void drive(size_t index, float dt, const Obstacle& obstacle) {
        Agent& agent = agents_[index];
        const float gap = clearAhead(index, agent.lane, agent.s, obstacle);
        float target = graph_->lanes[agent.lane].speed;
        // Car following: the gap a driver keeps is a time, not a distance, so
        // the same rule queues a jam at 10 km/h and spaces a dual carriageway
        // at 90 without a second constant.
        const float wanted = rules_.minGap + agent.speed * rules_.headway;
        if (gap < wanted) target = gap <= rules_.minGap ? 0.f : target * (gap - rules_.minGap) / wanted;
        if (mustYield(index)) target = 0.f;
        const float rate = target < agent.speed ? rules_.brake : rules_.accel;
        agent.speed += (target - agent.speed > 0.f ? 1.f : -1.f) * rate * dt;
        if (std::abs(target - agent.speed) < rate * dt) agent.speed = target;
        if (agent.speed < 0.f) agent.speed = 0.f;
        agent.s += agent.speed * dt;

        float length = graph_->laneLength(agent.lane);
        while (agent.s >= length) {
            agent.s -= length;
            const uint32_t node = graph_->lanes[agent.lane].to;
            const uint32_t choices = graph_->outCount(node);
            if (choices == 0) {  // the road ran out: this car's trip is over
                agent.alive = false;
                return;
            }
            // Anything but the way we came, unless that is the only way out —
            // a dead end is a real place and turning round in one is correct.
            const uint32_t previous = agent.lane;
            uint32_t picked = graph_->outLane(node, next(agent.seed) % choices);
            if (choices > 1 && isReverseOf(picked, previous))
                picked = graph_->outLane(node, (picked + 1) % choices);
            agent.lane = picked;
            length = graph_->laneLength(agent.lane);
            if (length <= 0.f) {  // a degenerate lane would spin this loop
                agent.alive = false;
                return;
            }
        }
    }

    bool isReverseOf(uint32_t lane, uint32_t other) const {
        return graph_->lanes[lane].to == graph_->lanes[other].from &&
               graph_->lanes[lane].from == graph_->lanes[other].to;
    }

    void spawn(Vec2 observer, Vec2 facing, bool looking) {
        uint32_t placed = 0;
        for (Agent& agent : agents_) {
            if (agent.alive) continue;
            if (placed >= rules_.maxSpawnsPerUpdate) return;
            // A few blind draws rather than a search: a graph with no lane in
            // the annulus must cost nothing, and one with many must not prefer
            // whichever lane happens to be first in the array.
            for (int attempt = 0; attempt < 8; ++attempt) {
                const uint32_t lane = graph_->laneAt(unit(random_));
                const float length = graph_->laneLength(lane);
                if (length < 1.f) continue;
                const float s = unit(random_) * length;
                const Vec2 where = positionOf(lane, s);
                const float distance = saida::traffic::length(where - observer);
                if (distance > rules_.spawnFar) continue;
                const Vec2 delta = where - observer;
                const bool behind = isBehind(delta, distance, facing, looking);
                // Never further out than it would be recycled from, or a car
                // appears and vanishes on the same frame -- which is a spawn
                // every frame, and a hierarchy the host has to rebuild.
                if (distance > despawnLimit(delta, distance, facing, looking)) continue;
                // Not named `near`: <windows.h> defines that as an empty macro,
                // and a host that includes it before this header gets a syntax
                // error in a file it did not write.
                const float closest = behind ? rules_.spawnNearBehind : rules_.spawnNear;
                if (distance < closest) continue;
                Agent candidate;
                candidate.lane = lane;
                candidate.s = s;
                candidate.speed = graph_->lanes[lane].speed * .6f;
                candidate.seed = next(random_) | 1u;
                candidate.alive = true;
                // Never on top of another car: the one artefact of annulus
                // spawning a player can actually catch, because it happens
                // behind him and drives into view.
                if (clearAhead(kNobody, candidate.lane, candidate.s, {}) < rules_.minGap * 1.5f)
                    continue;
                agent = candidate;
                ++placed;
                break;
            }
        }
    }

    const Graph* graph_ = nullptr;
    Rules rules_;
    std::vector<Agent> agents_;
    std::vector<Vec2> positions_;  // start-of-step positions, scratch
    uint32_t population_ = 0;
    uint32_t random_ = 1;
};

}  // namespace saida::traffic
