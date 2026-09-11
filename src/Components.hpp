// Flecs components for player entities.
#pragma once

#include <cstdint>

#include "Protocol.hpp"

namespace game {

// Globally unique per connected player, monotonically increasing, never reused.
struct NetId {
    uint32_t id;
};

// World-space position (metres). Ground plane is y = 0.
struct Position {
    float x, y, z;
};

struct Velocity {
    float x, y, z;
};

// Facing angle in radians, authored by the client's look input.
struct Yaw {
    float v;
};

// True while resting on the ground plane.
struct Grounded {
    bool v;
};

// Latest movement input received from the client.
struct InputState {
    int8_t moveF = 0;
    int8_t moveR = 0;
    bool jumpHeld = false;
    bool prevJumpHeld = false;
    float yaw = 0.f;
    uint16_t lastSeq = 0;
};

struct PlayerName {
    char v[proto::MAX_NAME_BYTES + 1];
};

// Links a flecs entity back to its network session.
struct ClientLink {
    uint64_t connId;
};

} // namespace game
