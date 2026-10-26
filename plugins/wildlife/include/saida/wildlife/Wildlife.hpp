// Ambient wildlife: groups of animals that live around a home, stand, graze,
// wander and run from whatever comes too close.
//
// Header-only, standard library only. It knows nothing about the engine -- no
// scene, node, clip or physics -- and nothing about where the groups came
// from: the host says which kinds exist, where each group lives, which ground
// each kind may stand on and what threatens them, and reads back where every
// animal is and what it is doing, which is all it needs to draw and animate
// them. Two axes the ground lies in; the host owns height and coordinates.
//
// A group lives anywhere in its roam around its home: at any moment it is at
// one point of it, drawn on ground it may stand on and drawn again every
// kRelocateSeconds. It is spawned whole when that point comes within its
// kind's view range of the observer and the host says it is out (the hour,
// the season and the weather are the host's business) -- so it is met as
// often as one passes where it is -- never in plain sight close by; it is
// dropped once the observer has left it behind. An animal without a gait never moves: it
// stands, and a small one darts off and is gone when scared.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace saida::wildlife {

struct Vec2 {
    float x = 0.f, y = 0.f;
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float k) { return {a.x * k, a.y * k}; }
inline float length(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline float distance(Vec2 a, Vec2 b) { return length(a - b); }

// Heading: radians from the +y axis toward +x, so (sin h, cos h) is forward.
inline Vec2 forward(float heading) { return {std::sin(heading), std::cos(heading)}; }
inline float headingOf(Vec2 d) { return std::atan2(d.x, d.y); }

enum class Surface : uint8_t { Ground, Air };
// What a scared animal does: nothing, run off at its flee speed, or dart a
// short way and vanish into cover (small animals: a lizard, a mouse).
enum class FleeStyle : uint8_t { None, Run, Dart };

// One kind of animal. Speeds in metres per second as drawn: a gait clip
// covers ground at `walkSpeed`, and the host scales playback to any other.
struct Kind {
    float walkSpeed = 0.f;     // 0: no gait, it never walks
    float runSpeed = 0.f;      // 0: no run clip
    float fleeSpeed = 0.f;     // how fast it flees; 0: its run (or walk) speed
    float fleeDistance = 0.f;  // a threat closer than this scares it; 0: never
    FleeStyle flee = FleeStyle::Run;
    float fleeSeconds = 8.f;   // how long it keeps fleeing after the last fright
    float dartSeconds = 0.8f;  // how long a dart lasts before it is gone
    float spread = 4.f;        // metres between members of a group
    float roam = 60.f;         // how far from home it wanders
    float idleMin = 4.f, idleMax = 20.f;  // seconds standing between walks
    float legMin = 6.f, legMax = 30.f;    // metres of one walk
    bool canStand = true;      // false: it keeps walking (no standing clip)
    bool canEat = false;       // part of its standing is spent eating
    Surface surface = Surface::Ground;
    float hoverLow = 0.f, hoverHigh = 0.f;  // air: metres above the ground
    float viewRange = 150.f;   // spawned within this of the observer, kept to 1.3 times it
    float radius = 0.5f;       // half its width: how close two animals stand
};

// A group that lives somewhere: the kind of each member, its home and how
// far it ranges. `identity` is the host's stable name for it (a resident's
// id); a group's spawns are seeded from it, so the same visit draws the same
// animals in the same places.
struct Group {
    Vec2 home;
    std::vector<uint16_t> members;
    uint64_t identity = 0;
};

enum class State : uint8_t { Idle, Eat, Walk, Run, Flee, Dart };

struct Animal {
    bool alive = false;
    uint32_t group = 0, member = 0;
    uint16_t kind = 0;
    State state = State::Idle;
    Vec2 position, target;
    float heading = 0.f;
    float speed = 0.f;         // m/s it moves now
    float height = 0.f;        // air: metres above the ground
    float timer = 0.f;         // seconds left of what it is doing
    uint64_t identity = 0;     // grows with every spawn into this slot
};

// What can scare an animal: a walker, a car. `scale` multiplies every flee
// distance (a car at speed is feared from further than a walker).
struct Threat {
    Vec2 position;
    float scale = 1.f;
};

struct Scene {
    Vec2 observer;             // the eye, on the ground plane
    Vec2 facing{0.f, 1.f};     // where it looks, unit length
    float fieldOfView = 2.2f;  // radians, full width
    std::vector<Threat> threats;
};

class Herds {
public:
    using Allowed = std::function<bool(uint16_t kind, Vec2 at)>;

    // `budget` is how many animals may live at once: slots never move, so a
    // host keys its nodes by slot and tells reuse apart by `identity`.
    void reset(std::vector<Kind> kinds, std::vector<Group> groups, size_t budget, uint64_t seed) {
        kinds_ = std::move(kinds);
        groups_ = std::move(groups);
        state_.assign(groups_.size(), GroupState{});
        animals_.assign(budget, Animal{});
        seed_ = seed ? seed : 1;
        for (auto& g : groups_)
            for (auto& k : g.members)
                if (k >= kinds_.size()) k = 0;
    }
    // Where a kind may stand; without one, anywhere.
    void setAllowed(Allowed allowed) { allowed_ = std::move(allowed); }
    // Whether a group is out now (its hour, its season). An inactive group is
    // not spawned, and its animals leave when nobody sees them go.
    void setActive(size_t group, bool active) {
        if (group < state_.size()) state_[group].active = active;
    }
    // Fewer slots than reset gave: the rest stay empty (the host's share).
    void setBudget(size_t budget) { budget_ = std::min(budget, animals_.size()); }

    const std::vector<Kind>& kinds() const { return kinds_; }
    const std::vector<Group>& groups() const { return groups_; }
    const std::vector<Animal>& animals() const { return animals_; }
    size_t live() const {
        size_t n = 0;
        for (const auto& a : animals_) n += a.alive;
        return n;
    }
    bool spawned(size_t group) const { return group < state_.size() && state_[group].spawned; }
    bool active(size_t group) const { return group < state_.size() && state_[group].active; }
    // Where in its roam a group is now, once the observer came within reach of it.
    bool whereabouts(size_t group, Vec2& at) const {
        if (group >= state_.size() || state_[group].whereAge >= kRelocateSeconds) return false;
        at = state_[group].where;
        return true;
    }

    void update(float dt, const Scene& scene) {
        if (dt <= 0.f) return;
        dt = std::min(dt, 0.1f);
        scene_ = &scene;
        for (size_t i = 0; i < animals_.size(); ++i)
            if (animals_[i].alive) step(i, dt);
        retire(scene);
        spawnNear(scene);
        for (auto& s : state_) {
            s.cooldown = std::max(0.f, s.cooldown - dt);
            s.whereAge += dt;
        }
        scene_ = nullptr;
    }

    // In plain sight of the observer: within the field of view and close
    // enough to be made out.
    bool seen(const Scene& scene, Vec2 at, float range) const {
        const Vec2 d = at - scene.observer;
        const float r = length(d);
        if (r > range) return false;
        if (r < 1e-3f) return true;
        const float cosine = (d.x * scene.facing.x + d.y * scene.facing.y) / r;
        return cosine >= std::cos(scene.fieldOfView * 0.5f);
    }

private:
    // How long a group stays at one point of its roam while unseen.
    static constexpr float kRelocateSeconds = 45.f;
    // An animal in view is kept out to this many view ranges: one does not
    // vanish while watched, however far the camera went.
    static constexpr float kKeptInView = 3.f;

    struct GroupState {
        bool active = true, spawned = false;
        Vec2 where;              // where in its roam it is now
        float whereAge = 1e9f;   // seconds since it was there
        uint32_t moves = 0;
        float cooldown = 0.f;   // seconds before a group that left may come back
        float fear = 0.f;       // seconds left of its fright
        Vec2 fleeFrom;
        uint32_t spawns = 0;
    };

    // splitmix64: one stream per purpose, seeded from the group.
    static uint64_t mix(uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    float random() {
        rng_ = mix(rng_);
        return float((rng_ >> 40) * (1.0 / 16777216.0));
    }
    float between(float lo, float hi) { return lo + (hi - lo) * random(); }

    bool allowed(uint16_t kind, Vec2 at) const { return !allowed_ || allowed_(kind, at); }
    const Kind& kindOf(const Animal& a) const { return kinds_[a.kind]; }

    // A point within `radius` of `around` the kind may stand on, else `around`.
    Vec2 pointNear(uint16_t kind, Vec2 around, float radius, int tries = 10) {
        for (int i = 0; i < tries; ++i) {
            const float a = between(0.f, 6.2831853f), r = radius * std::sqrt(random());
            const Vec2 p = around + Vec2{std::sin(a) * r, std::cos(a) * r};
            if (allowed(kind, p)) return p;
        }
        return around;
    }

    int leaderOf(uint32_t group) const {
        int best = -1;
        for (size_t i = 0; i < animals_.size(); ++i) {
            const auto& a = animals_[i];
            if (a.alive && a.group == group && (best < 0 || a.member < animals_[size_t(best)].member)) best = int(i);
        }
        return best;
    }

    void stand(Animal& a) {
        const Kind& k = kindOf(a);
        if (!k.canStand && k.walkSpeed > 0.f) {
            wander(a);
            return;
        }
        a.state = k.canEat && random() < 0.5f ? State::Eat : State::Idle;
        a.timer = between(k.idleMin, k.idleMax);
        a.speed = 0.f;
    }
    void wander(Animal& a) {
        const Kind& k = kindOf(a);
        const Vec2 home = groups_[a.group].home;
        Vec2 target = a.position;
        for (int i = 0; i < 10; ++i) {
            const float h = a.heading + between(-1.6f, 1.6f);
            Vec2 t = a.position + forward(h) * between(k.legMin, k.legMax);
            if (distance(t, home) > k.roam) t = home + (t - home) * (k.roam / std::max(distance(t, home), 1e-3f)) * 0.8f;
            if (allowed(a.kind, t) && clearPath(a.kind, a.position, t)) {
                target = t;
                break;
            }
        }
        a.target = target;
        a.state = State::Walk;
        a.timer = 60.f;
    }
    bool clearPath(uint16_t kind, Vec2 from, Vec2 to) const {
        const float d = distance(from, to);
        const int n = std::max(1, int(d / 2.f));
        for (int i = 1; i <= n; ++i)
            if (!allowed(kind, from + (to - from) * (float(i) / float(n)))) return false;
        return true;
    }

    // Away from `from`, turned toward ground it may stand on.
    Vec2 fleeTarget(const Animal& a, Vec2 from) const {
        Vec2 away = a.position - from;
        float r = length(away);
        if (r < 1e-3f) away = forward(a.heading), r = 1.f;
        const float base = headingOf(away);
        const float reach = std::max(8.f, kindOf(a).spread * 3.f);
        for (float turn : {0.f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f}) {
            const Vec2 t = a.position + forward(base + turn) * reach;
            if (allowed(a.kind, t) && clearPath(a.kind, a.position, t)) return t;
        }
        return a.position;
    }

    void scare(uint32_t group, Vec2 from) {
        auto& s = state_[group];
        s.fleeFrom = from;
        for (size_t i = 0; i < animals_.size(); ++i) {
            Animal& a = animals_[i];
            if (!a.alive || a.group != group) continue;
            const Kind& k = kindOf(a);
            if (k.flee == FleeStyle::None) continue;
            if (k.flee == FleeStyle::Dart) {
                if (a.state != State::Dart) {
                    a.state = State::Dart;
                    a.timer = k.dartSeconds;
                    a.target = fleeTarget(a, from);
                }
                continue;
            }
            if (k.walkSpeed <= 0.f && k.runSpeed <= 0.f) continue;
            if (s.fear <= 0.f || a.state != State::Flee) a.target = fleeTarget(a, from);
            a.state = State::Flee;
        }
        s.fear = 0.f;
        for (const auto& a : animals_)
            if (a.alive && a.group == group) s.fear = std::max(s.fear, kindOf(a).fleeSeconds);
    }

    void step(size_t index, float dt) {
        Animal& a = animals_[index];
        const Kind& k = kindOf(a);
        auto& g = state_[a.group];
        // Frights: the nearest threat inside the flee distance scares the group.
        if (k.fleeDistance > 0.f && k.flee != FleeStyle::None)
            for (const auto& t : scene_->threats)
                if (distance(t.position, a.position) < k.fleeDistance * t.scale) {
                    scare(a.group, t.position);
                    break;
                }
        if (a.state == State::Flee) {
            g.fear -= dt / std::max<size_t>(1, members(a.group));
            if (g.fear <= 0.f) stand(a);
        }
        if (k.surface == Surface::Air) {
            const float mid = 0.5f * (k.hoverLow + k.hoverHigh), amp = 0.5f * (k.hoverHigh - k.hoverLow);
            const float phase = float(a.identity % 97) + float(index);
            hover_ += dt;
            a.height = a.state == State::Idle ? 0.f : mid + amp * std::sin(hover_ * 1.3f + phase);
        }
        switch (a.state) {
        case State::Idle:
        case State::Eat: {
            a.speed = 0.f;
            a.timer -= dt;
            if (k.walkSpeed <= 0.f) {
                if (a.timer <= 0.f) stand(a);
                break;
            }
            const int leader = leaderOf(a.group);
            if (leader >= 0 && size_t(leader) != index) {
                const Vec2 slot = formation(animals_[size_t(leader)], a.member);
                if (distance(slot, a.position) > k.spread * 2.5f) {
                    a.target = pointNear(a.kind, slot, k.spread * 0.5f);
                    a.state = State::Walk;
                    a.timer = 60.f;
                    break;
                }
            }
            if (a.timer <= 0.f) {
                if (leader >= 0 && size_t(leader) != index) stand(a);
                else wander(a);
            }
            break;
        }
        case State::Walk:
        case State::Run: {
            const Animal* lead = nullptr;
            const int leader = leaderOf(a.group);
            if (leader >= 0 && size_t(leader) != index) lead = &animals_[size_t(leader)];
            const float behind = lead ? distance(formation(*lead, a.member), a.position) : 0.f;
            const bool hurry = lead && k.runSpeed > 0.f && behind > k.spread * 8.f;
            a.state = hurry ? State::Run : State::Walk;
            const float speed = hurry ? k.runSpeed : k.walkSpeed;
            if (move(a, speed, 2.5f, dt) || (a.timer -= dt) <= 0.f) stand(a);
            break;
        }
        case State::Flee: {
            if (distance(a.position, a.target) < 2.f) a.target = fleeTarget(a, g.fleeFrom);
            const float speed = k.fleeSpeed > 0.f ? k.fleeSpeed : (k.runSpeed > 0.f ? k.runSpeed : k.walkSpeed);
            move(a, speed, 4.f, dt);
            break;
        }
        case State::Dart: {
            const float speed = k.fleeSpeed > 0.f ? k.fleeSpeed : 2.f;
            move(a, speed, 12.f, dt);
            a.timer -= dt;
            if (a.timer <= 0.f) {
                a.alive = false;
                g.cooldown = 120.f;
            }
            break;
        }
        }
    }

    size_t members(uint32_t group) const {
        size_t n = 0;
        for (const auto& a : animals_) n += a.alive && a.group == group;
        return n;
    }

    // Where member `m` keeps relative to its leader: a loose phyllotaxis.
    Vec2 formation(const Animal& leader, uint32_t m) const {
        const Kind& k = kindOf(leader);
        const float angle = float(m) * 2.39996f, r = k.spread * std::sqrt(float(m));
        return leader.position + Vec2{std::sin(angle) * r, std::cos(angle) * r};
    }

    // Toward the target, turning at `turnRate`; true once there or blocked.
    bool move(Animal& a, float speed, float turnRate, float dt) {
        const Vec2 to = a.target - a.position;
        const float d = length(to);
        if (d < 0.3f || speed <= 0.f) {
            a.speed = 0.f;
            return true;
        }
        const float want = headingOf(to);
        const float turn = std::remainder(want - a.heading, 6.2831853f);
        const float most = turnRate * dt;
        a.heading += std::clamp(turn, -most, most);
        // Slow down while turning hard: an animal does not slide sideways.
        const float along = std::max(0.f, std::cos(turn));
        a.speed = speed * (0.25f + 0.75f * along);
        Vec2 next = a.position + forward(a.heading) * std::min(a.speed * dt, d);
        // Keep apart from the others of the group.
        const Kind& k = kindOf(a);
        for (const auto& o : animals_) {
            if (!o.alive || &o == &a || o.group != a.group) continue;
            const Vec2 off = next - o.position;
            const float r = length(off), keep = k.radius + kindOf(o).radius;
            if (r > 1e-4f && r < keep) next = next + off * ((keep - r) / r * 0.5f);
        }
        if (!allowed(a.kind, next)) {
            a.speed = 0.f;
            return true;
        }
        a.position = next;
        return false;
    }

    void retire(const Scene& scene) {
        for (size_t gi = 0; gi < groups_.size(); ++gi) {
            auto& s = state_[gi];
            if (!s.spawned) continue;
            size_t alive = 0;
            for (auto& a : animals_) {
                if (!a.alive || a.group != gi) continue;
                const Kind& k = kindOf(a);
                const float r = distance(a.position, scene.observer);
                const bool visible = seen(scene, a.position, k.viewRange * kKeptInView);
                if (visible ? r > k.viewRange * kKeptInView : (r > k.viewRange * 1.3f || !s.active))
                    a.alive = false;
                else
                    ++alive;
            }
            if (!alive) {
                s.spawned = false;
                s.fear = 0.f;
                s.cooldown = std::max(s.cooldown, 5.f);
            }
        }
    }

    void spawnNear(const Scene& scene) {
        size_t used = live();
        for (size_t gi = 0; gi < groups_.size() && used < budget_; ++gi) {
            auto& s = state_[gi];
            const Group& g = groups_[gi];
            if (s.spawned || !s.active || s.cooldown > 0.f || g.members.empty()) continue;
            const Kind& lead = kinds_[g.members.front()];
            // Out of reach of its whole roam: where it is will be drawn anew.
            if (distance(g.home, scene.observer) > lead.viewRange + lead.roam) {
                s.whereAge = 1e9f;
                continue;
            }
            if (s.whereAge >= kRelocateSeconds) {
                rng_ = mix(seed_ ^ mix(g.identity * 0x9E37ull + ++s.moves));
                s.where = pointNear(g.members.front(), g.home, lead.roam, 16);
                s.whereAge = 0.f;
            }
            const float away = distance(s.where, scene.observer);
            if (away > lead.viewRange) continue;
            // Not inside its own flight distance, where it would bolt the
            // moment it came: it slips off unseen instead. And never out of
            // nowhere in plain sight close by: there, only behind the
            // observer's back; further out, a group may come into view.
            if (away < std::min(lead.viewRange * 0.9f, lead.fleeDistance * 1.15f)) continue;
            if (seen(scene, s.where, lead.viewRange * 0.45f)) continue;
            rng_ = mix(seed_ ^ mix(g.identity + 0x51ED27ull * ++s.spawns));
            Vec2 first{};
            bool any = false;
            for (uint32_t m = 0; m < g.members.size() && used < budget_; ++m) {
                const uint16_t kind = g.members[m];
                const Kind& k = kinds_[kind];
                const Vec2 around = any ? first : s.where;
                const float radius = any ? k.spread * std::sqrt(float(m) + 1.f) : std::min(k.roam, k.spread);
                const Vec2 at = pointNear(kind, around, radius);
                if (!allowed(kind, at)) continue;
                size_t slot = 0;
                while (slot < animals_.size() && animals_[slot].alive) ++slot;
                if (slot >= animals_.size()) break;
                Animal& a = animals_[slot];
                const uint64_t identity = a.identity + 1;
                a = Animal{};
                a.alive = true;
                a.identity = identity;
                a.group = uint32_t(gi);
                a.member = m;
                a.kind = kind;
                a.position = a.target = at;
                a.heading = between(-3.1415926f, 3.1415926f);
                if (k.surface == Surface::Air) a.height = 0.5f * (k.hoverLow + k.hoverHigh);
                stand(a);
                a.timer *= random();  // not all in step
                if (!any) first = at, any = true;
                ++used;
            }
            if (any) s.spawned = true;
            else s.cooldown = 10.f;
        }
    }

    std::vector<Kind> kinds_;
    std::vector<Group> groups_;
    std::vector<GroupState> state_;
    std::vector<Animal> animals_;
    Allowed allowed_;
    const Scene* scene_ = nullptr;
    size_t budget_ = static_cast<size_t>(-1);
    uint64_t seed_ = 1, rng_ = 1;
    float hover_ = 0.f;
};

}  // namespace saida::wildlife
