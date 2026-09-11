#include "Simulation.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace game {

Simulation::Simulation(Bridge& bridge, const Config& cfg, Poster post)
    : bridge_(bridge), cfg_(cfg), post_(std::move(post)) {
    snapEvery_ = cfg_.simRate / cfg_.snapRate > 0 ? cfg_.simRate / cfg_.snapRate : 1;

    // Order matters: Move reads grounded state from the previous integrate,
    // Gravity applies only while airborne, Integrate resolves the ground plane.
    world_.system<InputState, Velocity, Yaw, const Grounded>("Move")
        .iter([](flecs::iter& it, InputState* in, Velocity* vel, Yaw* yaw,
                 const Grounded* grounded) {
            const float dt = static_cast<float>(it.delta_time());
            const float approach = 1.f - std::exp(-proto::ACCEL_RATE * dt);
            for (int i : it) {
                yaw[i].v = in[i].yaw;
                const float mf = in[i].moveF / 127.f;
                const float mr = in[i].moveR / 127.f;
                const float sy = std::sin(yaw[i].v);
                const float cy = std::cos(yaw[i].v);
                float tx = -sy * mf + cy * mr;
                float tz = -cy * mf - sy * mr;
                const float len = std::sqrt(tx * tx + tz * tz);
                if (len > 1e-4f) {
                    tx = tx / len * proto::MOVE_SPEED;
                    tz = tz / len * proto::MOVE_SPEED;
                } else {
                    tx = 0.f;
                    tz = 0.f;
                }
                vel[i].x += (tx - vel[i].x) * approach;
                vel[i].z += (tz - vel[i].z) * approach;
                if (grounded[i].v && in[i].jumpHeld && !in[i].prevJumpHeld) {
                    vel[i].y = proto::JUMP_VELOCITY;
                }
                in[i].prevJumpHeld = in[i].jumpHeld;
            }
        });

    world_.system<Velocity, const Grounded>("Gravity")
        .iter([](flecs::iter& it, Velocity* vel, const Grounded* grounded) {
            const float dt = static_cast<float>(it.delta_time());
            for (int i : it) {
                if (!grounded[i].v) vel[i].y -= proto::GRAVITY * dt;
            }
        });

    world_.system<Position, Velocity, Grounded>("Integrate")
        .iter([](flecs::iter& it, Position* pos, Velocity* vel, Grounded* grounded) {
            const float dt = static_cast<float>(it.delta_time());
            for (int i : it) {
                pos[i].x += vel[i].x * dt;
                pos[i].y += vel[i].y * dt;
                pos[i].z += vel[i].z * dt;

                const float r = std::sqrt(pos[i].x * pos[i].x + pos[i].z * pos[i].z);
                if (r > proto::WORLD_RADIUS) {
                    const float s = proto::WORLD_RADIUS / r;
                    pos[i].x *= s;
                    pos[i].z *= s;
                }

                if (pos[i].y <= proto::GROUND_Y) {
                    pos[i].y = proto::GROUND_Y;
                    if (vel[i].y < 0.f) vel[i].y = 0.f;
                    grounded[i].v = true;
                } else {
                    grounded[i].v = false;
                }
            }
        });

    snapQuery_ = world_.query<const NetId, const Position, const Yaw>();
}

Simulation::~Simulation() {
    stop();
}

void Simulation::start() {
    running_ = true;
    thread_ = std::thread(&Simulation::run, this);
}

void Simulation::stop() {
    if (thread_.joinable()) {
        running_ = false;
        thread_.join();
    }
}

void Simulation::run() {
    using clock = std::chrono::steady_clock;
    const auto frameDur = std::chrono::microseconds(1000000 / cfg_.simRate);
    auto next = clock::now();
    while (running_) {
        std::this_thread::sleep_until(next);
        next += frameDur;
        const auto now = clock::now();
        if (now > next + frameDur * 10) next = now; // fell far behind: resync
        tick();
    }
}

void Simulation::tick() {
    drainNet();
    world_.progress(1.0f / cfg_.simRate);
    ++tick_;

    if (tick_ % snapEvery_ == 0) {
        std::vector<proto::SnapshotEntry> ents;
        snapQuery_.each([&](const NetId& id, const Position& p, const Yaw& y) {
            ents.push_back({id.id, p.x, p.y, p.z, y.v});
        });
        out_.snapshot = proto::encodeSnapshot(static_cast<uint32_t>(tick_), ents);
    }

    publish();
}

flecs::entity Simulation::createPlayer(uint64_t connId, const std::string& name) {
    float x, z;
    spawnPosition(x, z);

    flecs::entity e = world_.entity()
                          .set<NetId>({nextNetId_++})
                          .set<Position>({x, proto::GROUND_Y + 1.0f, z})
                          .set<Velocity>({0.f, 0.f, 0.f})
                          .set<Yaw>({0.f})
                          .set<Grounded>({true})
                          .set<InputState>({})
                          .set<ClientLink>({connId});

    PlayerName pn{};
    std::string n = name.empty() ? "Player" : name;
    n.resize(std::min(n.size(), proto::MAX_NAME_BYTES));
    std::memcpy(pn.v, n.c_str(), n.size() + 1);
    e.set<PlayerName>(pn);

    byConn_[connId] = e;
    return e;
}

void Simulation::spawnPosition(float& x, float& z) {
    const float goldenAngle = 2.39996323f;
    const float a = spawnCounter_ * goldenAngle;
    const float r = 3.f + static_cast<float>(spawnCounter_ % 7) * 1.5f;
    ++spawnCounter_;
    x = std::cos(a) * r;
    z = std::sin(a) * r;
}

void Simulation::drainNet() {
    NetToSim events = bridge_.drain();

    // Despawn first so re-connecting clients are handled cleanly.
    for (uint64_t connId : events.disconnects) {
        auto it = byConn_.find(connId);
        if (it == byConn_.end()) continue;
        const NetId* id = it->second.get<NetId>();
        if (id) out_.despawns.push_back(id->id);
        it->second.destruct();
        byConn_.erase(it);
    }

    // Current roster for newcomer Welcomes (excludes players joining this tick).
    std::vector<proto::SpawnInfo> roster;
    snapQuery_.each([&](flecs::entity e, const NetId& id, const Position& p, const Yaw& y) {
        const PlayerName* n = e.get<PlayerName>();
        roster.push_back({id.id, n ? n->v : "", p.x, p.y, p.z, y.v});
    });

    for (const NetToSim::Connect& c : events.connects) {
        if (byConn_.count(c.connId)) continue; // double hello
        flecs::entity e = createPlayer(c.connId, c.name);
        const NetId* id = e.get<NetId>();
        const Position* p = e.get<Position>();
        const PlayerName* n = e.get<PlayerName>();
        if (!id || !p) continue;

        proto::SpawnInfo self{id->id, n ? n->v : "", p->x, p->y, p->z, 0.f};
        out_.welcomes.push_back({c.connId, id->id, p->x, p->y, p->z, roster});
        roster.push_back(self);
        out_.spawns.push_back(self);
    }

    for (const auto& [connId, in] : events.inputs) {
        applyInput(connId, in);
    }

    for (const NetToSim::Ping& p : events.pings) {
        out_.pongs.push_back({p.connId, p.nonce});
    }
}

void Simulation::applyInput(uint64_t connId, const ClientInput& in) {
    auto it = byConn_.find(connId);
    if (it == byConn_.end()) return;
    InputState* st = it->second.get_mut<InputState>();
    if (!st) return;
    st->moveF = in.moveF;
    st->moveR = in.moveR;
    st->jumpHeld = in.jumpHeld;
    st->yaw = in.yaw;
    st->lastSeq = in.seq;
}

void Simulation::publish() {
    if (out_.empty()) return;
    SimToNet out = std::move(out_);
    out_ = {};
    post_(std::move(out));
}

} // namespace game
