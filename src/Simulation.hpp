// Simulation thread: a fixed-rate flecs pipeline that owns the authoritative
// world state. Player movement is server-authoritative: clients send input
// state (axes + yaw + jump), the server integrates and broadcasts snapshots.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include <flecs.h>

#include "Bridge.hpp"
#include "Components.hpp"
#include "Protocol.hpp"

namespace game {

class Simulation {
public:
    using Poster = std::function<void(SimToNet&&)>;

    Simulation(Bridge& bridge, const Config& cfg, Poster post);
    ~Simulation();

    void start();
    void stop();

private:
    void run();
    void tick();
    void drainNet();
    void applyInput(uint64_t connId, const ClientInput& in);
    flecs::entity createPlayer(uint64_t connId, const std::string& name);
    void spawnPosition(float& x, float& z);
    void publish();

    Bridge& bridge_;
    Config cfg_;
    Poster post_;

    // Declared before snapQuery_ so the query is destroyed before the world.
    flecs::world world_;
    flecs::query<const NetId, const Position, const Yaw> snapQuery_;

    std::unordered_map<uint64_t, flecs::entity> byConn_;
    uint32_t nextNetId_ = 1;
    uint64_t tick_ = 0;
    uint32_t snapEvery_;
    uint32_t spawnCounter_ = 0;
    SimToNet out_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace game
