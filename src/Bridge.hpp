// Channel between the network thread (uWS loop) and the simulation thread.
// All state crosses this bridge; neither side ever touches the other's world
// directly (uWS sockets are loop-thread-only, the flecs world is sim-only).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Protocol.hpp"

namespace game {

struct Config {
    int port = 8080;
    int simRate = 60;   // simulation ticks per second
    int snapRate = 20;  // snapshot broadcasts per second
    std::string webDir = "client";
};

// Latest input received from a client.
struct ClientInput {
    uint16_t seq = 0;
    int8_t moveF = 0;
    int8_t moveR = 0;
    bool jumpHeld = false;
    float yaw = 0.f;
};

// Network -> simulation, consumed by Simulation::drainNet().
struct NetToSim {
    struct Connect {
        uint64_t connId;
        std::string name;
    };
    struct Ping {
        uint64_t connId;
        uint32_t nonce;
    };
    std::vector<Connect> connects;
    std::vector<uint64_t> disconnects;
    std::vector<std::pair<uint64_t, ClientInput>> inputs; // latest per client
    std::vector<Ping> pings;
};

// Simulation -> network, delivered to the uWS loop via defer.
struct SimToNet {
    struct Welcome {
        uint64_t connId;
        uint32_t netId;
        float x, y, z;
        std::vector<proto::SpawnInfo> roster; // players already in the world
    };
    struct Pong {
        uint64_t connId;
        uint32_t nonce;
    };
    std::vector<Welcome> welcomes;
    std::vector<proto::SpawnInfo> spawns; // broadcast, including to the new player
    std::vector<uint32_t> despawns;       // broadcast
    std::vector<uint8_t> snapshot;        // broadcast, already encoded
    std::vector<Pong> pongs;
    bool empty() const {
        return welcomes.empty() && spawns.empty() && despawns.empty() &&
               snapshot.empty() && pongs.empty();
    }
};

class Bridge {
public:
    // --- network thread side ---
    void pushConnect(uint64_t connId, std::string name) {
        std::lock_guard<std::mutex> lock(m_);
        n2s_.connects.push_back({connId, std::move(name)});
    }
    void pushDisconnect(uint64_t connId) {
        std::lock_guard<std::mutex> lock(m_);
        n2s_.disconnects.push_back(connId);
    }
    void pushInput(uint64_t connId, const ClientInput& in) {
        std::lock_guard<std::mutex> lock(m_);
        latestInput_[connId] = in;
        hasNewInput_ = true;
    }
    void pushPing(uint64_t connId, uint32_t nonce) {
        std::lock_guard<std::mutex> lock(m_);
        n2s_.pings.push_back({connId, nonce});
    }

    // --- simulation thread side ---
    NetToSim drain() {
        NetToSim out;
        std::lock_guard<std::mutex> lock(m_);
        out = std::move(n2s_);
        n2s_ = {};
        if (hasNewInput_) {
            for (auto& [connId, in] : latestInput_) {
                out.inputs.emplace_back(connId, in);
            }
            latestInput_.clear();
            hasNewInput_ = false;
        }
        return out;
    }

private:
    std::mutex m_;
    NetToSim n2s_;
    std::unordered_map<uint64_t, ClientInput> latestInput_;
    bool hasNewInput_ = false;
};

} // namespace game
