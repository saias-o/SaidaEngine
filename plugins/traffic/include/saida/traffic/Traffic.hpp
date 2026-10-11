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
// Directed lanes, curved junctions, following, priorities, signals and optional
// physical poses share one driver state. The host owns rendering and physics.
//
// Units are metres, seconds and radians throughout. The plane is the host's:
// pass whatever two axes the ground lies in and read the poses back in the
// same ones.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <limits>
#include <functional>
#include <stdexcept>
#include <optional>
#include "Signals.hpp"

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
    float offset = std::numeric_limits<float>::quiet_NaN();
    float width = 3.25f;
    uint64_t road = 0;
    uint16_t number = 0;
    uint16_t priority = 0;
    int32_t signal = -1;
    uint8_t signalGroup = 0;
    bool stop = false;
    bool yield = false;
    uint8_t allowedTurns = 15;
    int16_t layer = 0;
};

struct TurnRestriction { uint64_t fromRoad = 0, toRoad = 0; uint32_t node = 0; bool only = false; };

// A road network in one coordinate frame. Nodes are junctions and shape
// points; lanes connect them. `build()` turns the lane list into the adjacency
// the simulation reads, and must be called before the graph is driven on.
//
// The graph is immutable while agents drive on it. A host that streams road
// data gives each streamed region its own graph and its own `Flow`, which is
// also how a region can be discarded without telling every agent about it.
class Graph {
public:
    int rightTurnCrossSign=-1; // +1 for an X/south plane; -1 for X/north
    std::vector<Vec2> nodes;
    std::vector<Lane> lanes;
    std::vector<Signal> signals;
    std::vector<TurnRestriction> restrictions;
    bool allows(uint32_t from, uint32_t to) const {
        const auto& incoming = lanes[from];
        const auto& outgoing = lanes[to];
        for (const auto& restriction : restrictions)
            if (restriction.node == incoming.to && restriction.fromRoad == incoming.road &&
                (restriction.only ? restriction.toRoad != outgoing.road : restriction.toRoad == outgoing.road)) return false;
        const auto a = direction(from), b = direction(to);
        const float cosine = dot(a,b), cross = a.x*b.y-a.y*b.x;
        if(incoming.road&&incoming.road==outgoing.road&&outgoing.to!=incoming.from&&!isJunction(incoming.to))return true;
        const uint8_t movement = cosine > .7f ? 1 : cosine < -.7f ? 8 : cross*rightTurnCrossSign > 0 ? 4 : 2;
        return (incoming.allowedTurns & movement) != 0;
    }

    void build() {
        if(rightTurnCrossSign!=1&&rightTurnCrossSign!=-1)throw std::invalid_argument("invalid traffic coordinate handedness");
        lengths_.resize(lanes.size());
        for (const auto& node : nodes)
            if (!std::isfinite(node.x) || !std::isfinite(node.y)) throw std::invalid_argument("nonfinite traffic node");
        for (const auto& lane : lanes)
            if (lane.from >= nodes.size() || lane.to >= nodes.size() || !std::isfinite(lane.speed) || lane.speed <= 0.f)
                throw std::invalid_argument("invalid traffic lane");
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
        junctions_.assign(nodes.size(), false);
        std::vector<std::vector<uint32_t>> neighbours(nodes.size());
        for (const auto& lane : lanes) {
            neighbours[lane.from].push_back(lane.to);
            neighbours[lane.to].push_back(lane.from);
        }
        for (size_t node=0;node<neighbours.size();++node) {
            auto& adjacent=neighbours[node];
            std::sort(adjacent.begin(),adjacent.end());
            adjacent.erase(std::unique(adjacent.begin(),adjacent.end()),adjacent.end());
            junctions_[node]=adjacent.size()>2;
        }
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
    bool isJunction(uint32_t node) const { return junctions_[node]; }
    uint32_t outLane(uint32_t node, uint32_t i) const { return outgoing_[offsets_[node] + i]; }

private:
    std::vector<float> lengths_, cumulative_;
    std::vector<uint32_t> offsets_, outgoing_;
    std::vector<bool> junctions_;
};

// How this world's traffic behaves. Every field is a property of the place or
// of the vehicle, not of the renderer, so a host can hand different rules to
// different regions — which is what drives on the left, and how fast.
struct Rules {
    bool leftHand = false;     // which side of the centre line the lane sits on
    float laneOffset = 1.7f;   // metres from the centre line to the lane centre
    float lateralAxisSign = 1.f; // -1 when the host's second ground axis points south
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
struct DrivingLimits {
    // Zero selects the Flow's default. The host supplies physical capabilities.
    float acceleration=0.f,braking=0.f,lateralAcceleration=0.f,maximumSpeed=0.f;
    float wheelbase=0.f,steeringAngle=0.f; // radians; zero uses driver defaults
};
enum class StopReason { None, Following, Junction, BlockedExit, Signal, StopSign, Incident, EndOfRoad };
struct Agent {
    uint64_t identity = 0; // stable through lane changes; a recycled slot gets a new identity
    uint32_t lane = 0;
    float s = 0.f;      // metres travelled along `lane`
    float speed = 0.f;  // m/s
    bool alive = false;
    uint32_t seed = 1;  // this driver's own stream of choices at junctions
    uint32_t nextLane = UINT32_MAX, previousLane = UINT32_MAX;
    float length = 4.2f, width = 1.8f;
    float targetSpeed = 0.f;
    StopReason stopReason=StopReason::None;
    DrivingLimits limits;
    float waiting = 0.f;
    bool external = false, incident = false;
    bool stopCompleted = false;
    uint32_t laneChangeFrom = UINT32_MAX;
    float laneChangeElapsed = 0.f, laneChangeCooldown = 0.f;
    Vec2 actualPosition;
    float actualYaw = 0.f;
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
    float halfLength=0.f,halfWidth=0.f,yaw=0.f; // nonzero dimensions select an oriented box
};

struct DrivingCommand { float throttle = 0.f, brake = 0.f, steer = 0.f; };

struct DriverSettings {
    float curveDistance = 8.f;
    float lateralAcceleration = 5.f;
    float stopLineDistance = 2.f;
    float stopWait = 1.f;
    float minimumLookahead = 5.f;
    float lookaheadTime = .8f;
    uint32_t maximumLookaheadLanes = 32;
    float speedResponse = 2.f;
    float steeringAngle = .56f;
    float wheelbase = 2.5f;
    float defaultLength = 4.2f;
    float bumperGap = 2.f;
    float lateralClearance = .25f;
    float transferTolerance = 8.f;
    float laneChangeTime = 3.f;
    float laneChangeCooldown = 5.f;
    float minimumChangeDistance = 25.f;
    float congestionRatio = .45f;
    float fairnessWait = 8.f;
};

// The population driving on one graph.
//
// A sorted spatial sweep limits neighbour queries to the driver's corridor.
// Scratch storage is reused; no per-car container is allocated during a step.
class Flow {
public:
    DriverSettings driver;
    using Visibility = std::function<bool(const Pose&)>;
    void setVisibility(Visibility visible) { visible_ = std::move(visible); }
    void setObstacles(std::vector<Obstacle> obstacles) { obstacles_ = std::move(obstacles); }
    void setTime(double time) { if (std::isfinite(time)) time_ = time; }
    void releasePhysicalPose(size_t index) {
        if(index<agents_.size()){agents_[index].external=false;agents_[index].incident=false;}
    }
    void setVehicleDimensions(size_t index, float length, float width) {
        if (index < agents_.size() && length > 0.f && width > 0.f) { agents_[index].length = length; agents_[index].width = width; }
    }
    void setVehicleLimits(size_t index,DrivingLimits limits) {
        const float values[]{limits.acceleration,limits.braking,limits.lateralAcceleration,limits.maximumSpeed,limits.wheelbase,limits.steeringAngle};
        for(float value:values)if(!std::isfinite(value)||value<0.f)throw std::invalid_argument("invalid vehicle driving limits");
        if(index<agents_.size())agents_[index].limits=limits;
    }
    void setPhysicalPose(size_t index, const Pose& pose, bool incident) {
        if (index >= agents_.size()) return;
        auto& agent = agents_[index];
        agent.external = true;
        agent.incident = incident;
        agent.actualPosition = pose.position;
        agent.actualYaw = pose.yaw;
        agent.speed = std::max(0.f, pose.speed);
        const auto from = graph_->pointOn(agent.lane, 0.f);
        agent.s = std::clamp(dot(pose.position - from, graph_->direction(agent.lane)), 0.f, graph_->laneLength(agent.lane));
        if(!incident&&agent.nextLane!=UINT32_MAX) {
            const auto next=positionOf(agent.nextLane,0.f);
            const float beyond=dot(pose.position-next,graph_->direction(agent.nextLane));
            const float blend=std::min(driver.curveDistance,graph_->laneLength(agent.lane)*.25f);
            if(graph_->laneLength(agent.lane)-agent.s<blend&&beyond>=0.f) {
                agent.previousLane=agent.lane;agent.lane=agent.nextLane;agent.nextLane=UINT32_MAX;
                agent.s=std::clamp(beyond,0.f,graph_->laneLength(agent.lane));agent.stopCompleted=false;
            }
        }
    }
    DrivingCommand command(size_t index, const Pose& actual) const {
        if (index >= agents_.size()) return {};
        const auto& agent = agents_[index];
        if (agent.incident) return {0.f, 1.f, 0.f};
        const float lookahead = driver.minimumLookahead + actual.speed * driver.lookaheadTime;
        float s = agent.s + lookahead;
        Agent cursor=agent;
        uint32_t lane=agent.lane,nextLane=agent.nextLane;
        for(uint32_t crossed=0;s>graph_->laneLength(lane)&&nextLane!=UINT32_MAX&&crossed<driver.maximumLookaheadLanes;++crossed) {
            s-=graph_->laneLength(lane);lane=nextLane;cursor.lane=lane;nextLane=chooseNext(cursor);
        }
        const Vec2 aim = positionOf(lane, std::min(s, graph_->laneLength(lane))) - actual.position;
        const Vec2 heading{std::sin(actual.yaw), std::cos(actual.yaw)};
        const float cross = heading.x * aim.y - heading.y * aim.x;
        const float curvature = 2.f * cross / std::max(1.f, dot(aim, aim));
        const float wheelbase=agent.limits.wheelbase>0.f?agent.limits.wheelbase:driver.wheelbase;
        const float steeringAngle=agent.limits.steeringAngle>0.f?agent.limits.steeringAngle:driver.steeringAngle;
        const float steer = std::clamp(std::atan(curvature * wheelbase) / steeringAngle, -1.f, 1.f);
        const float difference = (agent.targetSpeed - actual.speed) / driver.speedResponse;
        if (agent.targetSpeed <= .05f) return {0.f, 1.f, steer};
        return {std::clamp(difference, 0.f, 1.f), std::clamp(-difference, 0.f, 1.f), steer};
    }
    void reset(const Graph* graph, const Rules& rules, uint32_t seed = 1) {
        graph_ = graph;
        rules_ = rules;
        random_ = seed ? seed : 1;
        identityPrefix_ = uint64_t(random_) << 32;
        identitySequence_ = 0;
        agents_.clear();
        population_ = 0;
    }
    // How many cars this network should show. Changing it is how a host makes
    // a dense district busier than a village without touching anything else.
    void setPopulation(uint32_t cars) { population_ = cars; if (!cars && !visible_) agents_.clear(); }
    uint32_t population() const { return population_; }
    uint32_t capacityNear(Vec2 observer) const {
        if(!graph_||!graph_->ready())return 0;
        float capacity=0.f;
        for(uint32_t lane=0;lane<graph_->lanes.size();++lane) {
            const auto start=positionOf(lane,0.f),direction=graph_->direction(lane);
            const auto delta=observer-start;
            const float projection=dot(delta,direction);
            const float distanceSquared=std::max(0.f,dot(delta,delta)-projection*projection);
            const float radiusSquared=rules_.spawnFar*rules_.spawnFar;
            if(distanceSquared>=radiusSquared)continue;
            const float half=std::sqrt(radiusSquared-distanceSquared);
            const float span=std::max(0.f,std::min(graph_->laneLength(lane),projection+half)-std::max(0.f,projection-half));
            const float spacing=rules_.minGap+graph_->lanes[lane].speed*rules_.headway;
            capacity+=span/std::max(1.f,spacing);
        }
        return uint32_t(capacity);
    }
    std::optional<size_t> adopt(const Agent& source,const Pose& actual,uint64_t road=0) {
        if(!source.alive||!graph_||!graph_->ready()||!std::isfinite(actual.position.x)||
           !std::isfinite(actual.position.y)||!std::isfinite(actual.yaw)||!std::isfinite(actual.speed))return std::nullopt;
        uint32_t best=UINT32_MAX;float bestDistance=driver.transferTolerance,bestS=0;
        const Vec2 heading{std::sin(actual.yaw),std::cos(actual.yaw)};
        for(uint32_t lane=0;lane<graph_->lanes.size();++lane) {
            if(road&&graph_->lanes[lane].road!=road)continue;
            if(!source.incident&&dot(heading,graph_->direction(lane))<.7f)continue;
            const float s=std::clamp(dot(actual.position-positionOf(lane,0),graph_->direction(lane)),-driver.transferTolerance,graph_->laneLength(lane));
            const float distance=length(actual.position-positionOf(lane,s));
            if(distance<bestDistance){best=lane;bestDistance=distance;bestS=s;}
        }
        if(best==UINT32_MAX)return std::nullopt;
        size_t slot=0;while(slot<agents_.size()&&agents_[slot].alive)++slot;
        if(slot==agents_.size())agents_.emplace_back();
        auto& agent=agents_[slot];agent=source;agent.lane=best;agent.s=bestS;
        agent.nextLane=agent.previousLane=UINT32_MAX;agent.actualPosition=actual.position;agent.actualYaw=actual.yaw;
        agent.laneChangeFrom=UINT32_MAX;
        return slot;
    }
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
        if (agent.external) return {agent.actualPosition, agent.actualYaw, agent.speed};
        const auto curve = curvedPose(agent);
        if(agent.laneChangeFrom!=UINT32_MAX) {
            const auto old=positionOf(agent.laneChangeFrom,agent.s);
            const float t=std::clamp(agent.laneChangeElapsed/driver.laneChangeTime,0.f,1.f);
            const float blend=t*t*(3.f-2.f*t);
            const auto offset=curve.position-old;
            const auto tangent=graph_->direction(agent.lane)*agent.speed+offset*(6.f*t*(1.f-t)/driver.laneChangeTime);
            return {old+offset*blend,std::atan2(tangent.x,tangent.y),agent.speed};
        }
        return {curve.position, curve.yaw, agent.speed};
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
        const float offset = std::isfinite(graph_->lanes[lane].offset) ? graph_->lanes[lane].offset : rules_.laneOffset;
        return graph_->pointOn(lane, s) + right * ((rules_.leftHand ? -offset : offset) * rules_.lateralAxisSign);
    }

    // `facing` is the unit vector the observer is looking along, or {0,0} when
    // the host does not track one. It is not defaulted and there is no shorter
    // overload: `update(dt, where, obstacle)` and `update(dt, where, facing)`
    // are ambiguous to the compiler and, worse, to the reader.
    void update(float dt, Vec2 observer, Vec2 facing, const Obstacle& obstacle = {}) {
        if (!graph_ || !graph_->ready() || graph_->lanes.empty()) return;
        if (!std::isfinite(dt) || dt <= 0.f) return;
        constexpr float kMaximumAcceptedDelta = .25f, kMaximumStep = .05f;
        const float elapsed = std::min(dt, kMaximumAcceptedDelta);
        const uint32_t steps = uint32_t(std::ceil(elapsed / kMaximumStep));
        const float step = elapsed / float(steps);
        for (uint32_t i = 0; i < steps; ++i) updateStep(step, observer, facing, obstacle);
    }

private:
    void updateStep(float dt, Vec2 observer, Vec2 facing, const Obstacle& obstacle) {
        if (agents_.size() < population_) agents_.resize(population_);
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
            if (!agents_[i].external && distance > despawnLimit(delta, distance, facing, looking) &&
                (!visible_ || !visible_(pose(agents_[i])))) agents_[i].alive = false;
        }
        spawn(observer, facing, looking, obstacle);
    }

    Pose curvedPose(const Agent& agent) const {
        const float span = graph_->laneLength(agent.lane);
        uint32_t before = agent.lane, after = agent.nextLane;
        float distance = agent.s - span;
        if (agent.previousLane != UINT32_MAX && agent.s < driver.curveDistance) {
            before = agent.previousLane; after = agent.lane; distance = agent.s;
        }
        const auto straight = [&] {
            const auto dir = graph_->direction(agent.lane);
            return Pose{positionOf(agent.lane, agent.s), std::atan2(dir.x, dir.y), agent.speed};
        };
        if (after == UINT32_MAX) return straight();
        if (dot(graph_->direction(before), graph_->direction(after)) > .999f) return straight();
        const float blend = std::min({driver.curveDistance, graph_->laneLength(before) * .25f, graph_->laneLength(after) * .25f});
        if (blend <= 0.f || distance < -blend || distance > blend) return straight();
        const float t = (distance + blend) / (2.f * blend);
        const Vec2 p0 = positionOf(before, graph_->laneLength(before) - blend);
        const Vec2 p3 = positionOf(after, blend);
        const Vec2 p1 = p0 + graph_->direction(before) * blend;
        const Vec2 p2 = p3 - graph_->direction(after) * blend;
        const float u = 1.f - t;
        const Vec2 position = p0 * (u*u*u) + p1 * (3.f*u*u*t) + p2 * (3.f*u*t*t) + p3 * (t*t*t);
        const Vec2 tangent = (p1-p0) * (u*u) + (p2-p1) * (2.f*u*t) + (p3-p2) * (t*t);
        return {position, std::atan2(tangent.x, tangent.y), agent.speed};
    }
    uint32_t chooseNext(Agent& agent) const {
        const auto node = graph_->lanes[agent.lane].to;
        const auto count = graph_->outCount(node);
        if (!count) return UINT32_MAX;
        const auto start = next(agent.seed) % count;
        // A polyline's shape point is not a lane-change request. Stay on the
        // same strip when the road continues, before selecting a junction turn.
        const auto& current=graph_->lanes[agent.lane];
        for(uint32_t i=0;i<count;++i) {
            const auto candidate=graph_->outLane(node,i);
            const auto& lane=graph_->lanes[candidate];
            if(current.road&&lane.road==current.road&&lane.number==current.number&&!isReverseOf(candidate,agent.lane)&&
               (!graph_->isJunction(node)||dot(graph_->direction(agent.lane),graph_->direction(candidate))>.95f)&&graph_->allows(agent.lane,candidate))return candidate;
        }
        for (uint32_t attempt=0;attempt<count;++attempt) {
            const auto lane = graph_->outLane(node, (start+attempt)%count);
            if (!isReverseOf(lane, agent.lane) && graph_->allows(agent.lane, lane)) return lane;
        }
        return UINT32_MAX;
    }
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
        waiting_.resize(agents_.size());
        spatial_.clear();
        neighbourDistance_=rules_.minGap*1.5f+driver.defaultLength;
        maximumHalfExtent_=driver.defaultLength*.5f;
        for (size_t i = 0; i < agents_.size(); ++i)
            if (agents_[i].alive) {
                positions_[i] = pose(agents_[i]).position; waiting_[i] = agents_[i].waiting;
                spatial_.push_back(i);
                const auto& agent=agents_[i];
                const float speed=std::max(agent.speed,graph_->lanes[agent.lane].speed);
                const float braking=std::max(.1f,agent.limits.braking>0.f?agent.limits.braking:rules_.brake);
                neighbourDistance_=std::max(neighbourDistance_,rules_.minGap+agent.length*2.f+
                    std::max(speed*rules_.headway,speed*speed/(2.f*braking)));
                maximumHalfExtent_=std::max(maximumHalfExtent_,.5f*std::hypot(agent.length,agent.width));
            }
        std::sort(spatial_.begin(),spatial_.end(),[&](size_t a,size_t b){return positions_[a].x<positions_[b].x;});
    }
    template<class Visitor> void neighbours(Vec2 minimum,Vec2 maximum,Visitor visit) const {
        auto first=std::lower_bound(spatial_.begin(),spatial_.end(),minimum.x,
            [&](size_t index,float x){return positions_[index].x<x;});
        for(;first!=spatial_.end()&&positions_[*first].x<=maximum.x;++first)
            if(agents_[*first].alive&&positions_[*first].y>=minimum.y&&positions_[*first].y<=maximum.y)visit(*first);
    }
    // Distance to whatever is in front of this agent on its own lane, and on
    // the lane it is about to enter. Returns a large number when the road is
    // clear. `self` is an index into the flow, or `kNobody` for a car that is
    // not in it yet -- a spawn candidate.
    static constexpr size_t kNobody = size_t(-1);
    float clearAhead(size_t self, uint32_t lane, float s, const Obstacle& obstacle) const {
        const bool physical=self!=kNobody&&agents_[self].external&&agents_[self].lane==lane;
        const Vec2 head = physical ? positions_[self] : positionOf(lane, s);
        const Vec2 dir = graph_->direction(lane);
        float nearest = 1e9f;
        const auto end=head+dir*neighbourDistance_;
        const float ownWidth=self==kNobody?Agent{}.width:agents_[self].width;
        const float corridor=maximumHalfExtent_+ownWidth*.5f+driver.lateralClearance;
        neighbours({std::min(head.x,end.x)-corridor,std::min(head.y,end.y)-corridor},
                   {std::max(head.x,end.x)+corridor,std::max(head.y,end.y)+corridor},[&](size_t j) {
            const Agent& other = agents_[j];
            if (j == self) return;
            if(graph_->lanes[other.lane].layer!=graph_->lanes[lane].layer)return;
            // Crossing approaches are ordered by the junction reservation.
            // Counting a waiting cross-street queue as a leader traps its winner.
            if(!other.incident&&dot(graph_->direction(other.lane),dir)<.5f)return;
            const Vec2 delta = positions_[j] - head;
            const float along = dot(delta, dir);
            // Only what is in front and roughly in this lane's corridor: a car
            // on the opposite carriageway is not an obstacle, and a car on a
            // crossing street is handled by the junction rule below.
            const float lateral = std::abs(delta.x * dir.y - delta.y * dir.x);
            const float yaw=pose(other).yaw;
            const Vec2 heading{std::sin(yaw),std::cos(yaw)};
            const float parallel=std::abs(dot(dir,heading));
            const float crossing=std::abs(dir.x*heading.y-dir.y*heading.x);
            const float halfLength=(other.length*parallel+other.width*crossing)*.5f;
            const float halfWidth=(other.width*parallel+other.length*crossing)*.5f;
            if (along+halfLength <= 0.f) return;
            const float occupiedWidth=ownWidth*.5f+halfWidth+driver.lateralClearance;
            if (lateral > occupiedWidth) return;
            const float clearance = std::max(0.f,along-halfLength+driver.defaultLength*.5f);
            if (clearance < nearest) nearest = clearance;
        });
        const auto considerObstacle=[&](const Obstacle& extra) {
            if(!extra.active)return;
            const auto delta=extra.position-head;
            const float along = dot(delta, dir);
            const float lateral = std::abs(delta.x * dir.y - delta.y * dir.x);
            float lengthExtent=extra.radius,widthExtent=extra.radius;
            if(extra.halfLength>0.f&&extra.halfWidth>0.f) {
                const Vec2 forward{std::sin(extra.yaw),std::cos(extra.yaw)};
                const float parallel=std::abs(dot(dir,forward));
                const float crossing=std::abs(dir.x*forward.y-dir.y*forward.x);
                lengthExtent=extra.halfLength*parallel+extra.halfWidth*crossing;
                widthExtent=extra.halfWidth*parallel+extra.halfLength*crossing;
            }
            if(along+lengthExtent>0.f&&lateral<widthExtent+ownWidth*.5f+driver.lateralClearance)
                nearest=std::min(nearest,std::max(0.f,along-lengthExtent+driver.defaultLength*.5f));
        };
        considerObstacle(obstacle);
        for(const auto& extra:obstacles_)considerObstacle(extra);
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
    // Keep the current owner, then order arrivals by waiting time, priority,
    // distance and stable identity. A strict order prevents mutual deadlocks.
    bool mustYield(size_t self) const {
        const auto& agent=agents_[self];const auto& lane=graph_->lanes[agent.lane];
        // Polyline shape points do not reserve the entire road. Opposing
        // traffic on short OSM segments otherwise stops for each other.
        if(!graph_->isJunction(lane.to))return false;
        if(graph_->laneLength(agent.lane)-agent.s>rules_.junctionGuard)return false;
        const auto junction=graph_->nodes[lane.to];
        const float mine=length(positions_[self]-junction);
        bool blocked=false;
        const float guard=rules_.junctionGuard;
        neighbours({junction.x-guard,junction.y-guard},{junction.x+guard,junction.y+guard},[&](size_t j) {
            const auto& other=agents_[j];if(blocked||j==self||other.lane==agent.lane)return;
            const auto& incoming=graph_->lanes[other.lane];
            const auto straight=[&](const Agent& car) {
                const auto neighbour=car.nextLane!=UINT32_MAX?car.nextLane:car.previousLane;
                return neighbour!=UINT32_MAX&&dot(graph_->direction(car.lane),graph_->direction(neighbour))>.95f;
            };
            if(std::abs(dot(graph_->direction(agent.lane),graph_->direction(other.lane)))>.95f&&straight(agent)&&straight(other))return;
            const float theirs=length(positions_[j]-junction);
            if(theirs>=rules_.junctionGuard)return;
            // A driver already inside owns the junction until its rear clears.
            if(incoming.from==lane.to&&other.s<other.length*.5f+driver.bumperGap){blocked=true;return;}
            if(incoming.to!=lane.to)return;
            if(incoming.signal>=0&&size_t(incoming.signal)<graph_->signals.size()&&
               graph_->signals[size_t(incoming.signal)].light(incoming.signalGroup,time_)==Light::Red)return;
            const bool myWait=waiting_[self]>=driver.fairnessWait,theirWait=waiting_[j]>=driver.fairnessWait;
            if(myWait!=theirWait){blocked=theirWait;return;}
            if(myWait&&waiting_[self]!=waiting_[j]){blocked=waiting_[j]>waiting_[self];return;}
            const auto myPriority=lane.yield?0:lane.priority;
            const auto theirPriority=incoming.yield?0:incoming.priority;
            if(theirPriority!=myPriority){blocked=theirPriority>myPriority;return;}
            if(theirs<mine||(theirs==mine&&other.identity<agent.identity))blocked=true;
        });
        return blocked;
    }
    void drive(size_t index, float dt, const Obstacle& obstacle) {
        Agent& agent = agents_[index];
        agent.laneChangeCooldown=std::max(0.f,agent.laneChangeCooldown-dt);
        if(agent.laneChangeFrom!=UINT32_MAX) {
            agent.laneChangeElapsed+=dt;
            if(agent.laneChangeElapsed>=driver.laneChangeTime)agent.laneChangeFrom=UINT32_MAX;
        }
        if (agent.nextLane == UINT32_MAX) agent.nextLane = chooseNext(agent);
        const float gap = clearAhead(index, agent.lane, agent.s, obstacle);
        agent.stopReason=StopReason::None;
        float target = graph_->lanes[agent.lane].speed;
        const float braking=agent.limits.braking>0.f?agent.limits.braking:rules_.brake;
        const float acceleration=agent.limits.acceleration>0.f?agent.limits.acceleration:rules_.accel;
        const float lateralAcceleration=agent.limits.lateralAcceleration>0.f?agent.limits.lateralAcceleration:driver.lateralAcceleration;
        if(agent.limits.maximumSpeed>0.f)target=std::min(target,agent.limits.maximumSpeed);
        const auto& lane = graph_->lanes[agent.lane];
        const float remaining = graph_->laneLength(agent.lane) - agent.s;
        if (agent.nextLane != UINT32_MAX) {
            const float angle = std::acos(std::clamp(dot(graph_->direction(agent.lane), graph_->direction(agent.nextLane)), -1.f, 1.f));
            if (angle > .05f) {
                const float radius = driver.curveDistance / angle;
                const float cornerSpeed = std::sqrt(lateralAcceleration * std::max(1.f, radius));
                target = std::min(target, std::sqrt(cornerSpeed * cornerSpeed + 2.f * braking * std::max(0.f, remaining-driver.curveDistance)));
            }
        }
        // Car following: the gap a driver keeps is a time, not a distance, so
        // the same rule queues a jam at 10 km/h and spaces a dual carriageway
        // at 90 without a second constant.
        const float baseGap = rules_.minGap + .5f * (agent.length - driver.defaultLength);
        const float available = std::max(0.f,gap-baseGap);
        // Reserve a response interval before braking, including the physical
        // driver's proportional brake response and a discrete simulation step.
        const float responseSpeed=braking*(agent.external?std::max(dt,driver.speedResponse):dt);
        const auto stoppingSpeed=[&](float distance) {
            return std::sqrt(responseSpeed*responseSpeed+2.f*braking*std::max(0.f,distance))-responseSpeed;
        };
        const float followingSpeed = std::min(available/std::max(.1f,rules_.headway),stoppingSpeed(available));
        if (followingSpeed < target) {target=followingSpeed;agent.stopReason=StopReason::Following;}
        if(graph_->isJunction(lane.to)&&agent.nextLane!=UINT32_MAX&&remaining<rules_.junctionGuard&&
           clearAhead(index,agent.nextLane,0.f,obstacle)<agent.length+driver.bumperGap){target=0.f;agent.stopReason=StopReason::BlockedExit;}
        if (mustYield(index)) {target = 0.f;agent.stopReason=StopReason::Junction;}
        bool stop = false;
        if (lane.signal >= 0 && size_t(lane.signal) < graph_->signals.size()) {
            const auto light = graph_->signals[size_t(lane.signal)].light(lane.signalGroup, time_);
            const float brakingDistance = agent.speed * agent.speed / (2.f * braking);
            stop = light == Light::Red || (light == Light::Amber && remaining > brakingDistance + driver.stopLineDistance);
        }
        if (lane.stop && !agent.stopCompleted) stop = true;
        if (stop) {
            const float approach=stoppingSpeed(remaining-driver.stopLineDistance-agent.length*.5f);
            if(approach<=target)agent.stopReason=lane.stop?StopReason::StopSign:StopReason::Signal;
            target=std::min(target,approach);
        }
        if (agent.speed < .1f) agent.waiting += dt; else agent.waiting = 0.f;
        if (agent.waiting >= driver.stopWait && remaining <= driver.stopLineDistance + agent.length*.5f + .5f) agent.stopCompleted = true;
        if (agent.incident) {target = 0.f;agent.stopReason=StopReason::Incident;}
        agent.targetSpeed = target;
        if(!stop&&!agent.incident&&agent.laneChangeFrom==UINT32_MAX&&agent.laneChangeCooldown==0.f&&
           agent.s>driver.minimumChangeDistance&&remaining>driver.minimumChangeDistance&&target<lane.speed*driver.congestionRatio)
            tryLaneChange(index,obstacle);
        if (agent.external) {
            if (agent.s >= graph_->laneLength(agent.lane)-.1f && agent.nextLane != UINT32_MAX && !agent.incident) {
                agent.previousLane = agent.lane; agent.lane = agent.nextLane; agent.nextLane = UINT32_MAX; agent.s = 0.f; agent.stopCompleted = false;
            }
            return;
        }
        const float previousSpeed=agent.speed;
        agent.speed = std::max(0.f,agent.speed+std::clamp(target-agent.speed,-braking*dt,acceleration*dt));
        agent.s += (previousSpeed+agent.speed)*.5f*dt;

        float length = graph_->laneLength(agent.lane);
        while (agent.s >= length) {
            agent.s -= length;
            if (agent.nextLane == UINT32_MAX) {
                if (visible_ && visible_(pose(agent))) { agent.s = length-driver.stopLineDistance; agent.speed = 0.f; return; }
                agent.alive = false;
                return;
            }
            // Anything but the way we came, unless that is the only way out —
            // a dead end is a real place and turning round in one is correct.
            agent.previousLane = agent.lane;
            agent.lane = agent.nextLane;
            agent.stopCompleted = false;
            agent.nextLane = chooseNext(agent);
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
    void tryLaneChange(size_t index,const Obstacle& obstacle) {
        auto& agent=agents_[index];const auto& current=graph_->lanes[agent.lane];
        if(!current.road)return;
        const auto count=graph_->outCount(current.from);
        for(uint32_t candidate=0;candidate<count;++candidate) {
            const auto lane=graph_->outLane(current.from,candidate);const auto& adjacent=graph_->lanes[lane];
            if(adjacent.to!=current.to||adjacent.road!=current.road||std::abs(int(adjacent.number)-int(current.number))!=1)continue;
            const float needed=agent.length+driver.bumperGap+agent.speed*rules_.headway;
            if(clearAhead(index,lane,agent.s,obstacle)<needed)continue;
            bool safe=true;const auto position=positionOf(lane,agent.s);
            const float radius=neighbourDistance_;
            neighbours({position.x-radius,position.y-radius},{position.x+radius,position.y+radius},[&](size_t other) {
                if(other==index)return;
                const auto delta=positions_[other]-position;
                const auto direction=graph_->direction(lane);
                const auto heading=pose(agents_[other]).yaw;
                const Vec2 otherDirection{std::sin(heading),std::cos(heading)};
                const float cross=direction.x*otherDirection.y-direction.y*otherDirection.x;
                const float halfWidth=(agents_[other].length*std::abs(cross)+agents_[other].width*std::abs(dot(direction,otherDirection))+agent.width)*.5f;
                if(std::abs(delta.x*direction.y-delta.y*direction.x)>halfWidth+driver.bumperGap*.25f)return;
                const float gap=length(positions_[other]-position);
                if(gap<agent.length+agents_[other].length*.5f+agents_[other].speed*rules_.headway)safe=false;
            });
            if(!safe)continue;
            agent.laneChangeFrom=agent.lane;agent.lane=lane;agent.laneChangeElapsed=0.f;
            agent.laneChangeCooldown=driver.laneChangeCooldown;agent.nextLane=UINT32_MAX;return;
        }
    }

    bool spawnClear(const Agent& candidate,const Obstacle& obstacle) const {
        if(clearAhead(kNobody,candidate.lane,candidate.s,obstacle)<rules_.minGap*1.5f)return false;
        const auto where=positionOf(candidate.lane,candidate.s),direction=graph_->direction(candidate.lane);
        const float radius=neighbourDistance_+maximumHalfExtent_;
        bool clear=true;
        neighbours({where.x-radius,where.y-radius},{where.x+radius,where.y+radius},[&](size_t index) {
            const auto& other=agents_[index];
            if(!clear||graph_->lanes[other.lane].layer!=graph_->lanes[candidate.lane].layer)return;
            const auto delta=positions_[index]-where;
            const float yaw=pose(other).yaw;
            const Vec2 heading{std::sin(yaw),std::cos(yaw)};
            const float parallel=std::abs(dot(direction,heading));
            const float crossing=std::abs(direction.x*heading.y-direction.y*heading.x);
            const float halfLength=(other.length*parallel+other.width*crossing)*.5f;
            const float halfWidth=(other.width*parallel+other.length*crossing)*.5f;
            if(std::abs(delta.x*direction.y-delta.y*direction.x)>candidate.width*.5f+halfWidth+driver.lateralClearance)return;
            const float along=dot(delta,direction);
            float clearance=candidate.length*.5f+halfLength+driver.bumperGap;
            if(along<0.f&&dot(direction,heading)>.5f)clearance=std::max(clearance,rules_.minGap+other.speed*rules_.headway);
            if(std::abs(along)<clearance)clear=false;
        });
        return clear;
    }
    void spawn(Vec2 observer, Vec2 facing, bool looking, const Obstacle& obstacle) {
        uint32_t placed = 0;
        for (Agent& agent : agents_) {
            if (agent.alive) continue;
            if (live() >= population_) return;
            if (placed >= rules_.maxSpawnsPerUpdate) return;
            // A few blind draws rather than a search: a graph with no lane in
            // the annulus must cost nothing, and one with many must not prefer
            // whichever lane happens to be first in the array.
            const uint32_t lane = graph_->laneAt(unit(random_));
            for (int attempt = 0; attempt < 8; ++attempt) {
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
                candidate.alive = true;
                if (visible_ && visible_(pose(candidate))) continue;
                // Never on top of another car: the one artefact of annulus
                // spawning a player can actually catch, because it happens
                // behind him and drives into view.
                if (!spawnClear(candidate,obstacle)) continue;
                candidate.seed = next(random_) | 1u;
                candidate.identity = identityPrefix_ | ++identitySequence_;
                agent = candidate;
                // Later spawn candidates must see this trip immediately.
                refreshPositions();
                ++placed;
                break;
            }
        }
    }

    const Graph* graph_ = nullptr;
    Rules rules_;
    std::vector<Agent> agents_;
    std::vector<Vec2> positions_;  // start-of-step positions, scratch
    std::vector<float> waiting_;
    std::vector<size_t> spatial_;
    float neighbourDistance_=0.f;
    float maximumHalfExtent_=0.f;
    uint32_t population_ = 0;
    uint32_t random_ = 1;
    uint64_t identityPrefix_ = 0;
    uint32_t identitySequence_ = 0;
    double time_ = 0.0;
    Visibility visible_;
    std::vector<Obstacle> obstacles_;
};

}  // namespace saida::traffic
