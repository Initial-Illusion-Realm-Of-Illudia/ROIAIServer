// Wire protocol: little-endian binary WebSocket frames.
// Every frame starts with a one-byte message id. See README.md for the tables.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace proto {

constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr size_t   MAX_NAME_BYTES   = 24;
constexpr size_t   MAX_PAYLOAD      = 1024;

// C -> S
enum MsgC2S : uint8_t {
    C2S_Hello = 1,  // version u16, name str8
    C2S_Input = 2,  // seq u16, moveF i8, moveR i8, flags u8 (bit0 jumpHeld), yaw f32
    C2S_Ping  = 3,  // nonce u32
};

// S -> C
enum MsgS2C : uint8_t {
    S2C_Welcome  = 1,  // netId u32, x f32, y f32, z f32, rosterCount u16, roster[N] = Spawn without id byte
    S2C_Spawn    = 2,  // netId u32, name str8, x f32, y f32, z f32, yaw f32
    S2C_Despawn  = 3,  // netId u32
    S2C_Snapshot = 4,  // tick u32, count u16, count * { netId u32, x f32, y f32, z f32, yaw f32 }
    S2C_Pong     = 5,  // nonce u32
};

// Input flag bits
enum InputFlags : uint8_t {
    INPUT_JUMP_HELD = 1 << 0,
};

// Movement tuning. Server-authoritative; the browser client mirrors these
// constants for its local prediction. Keep both sides in sync.
constexpr float MOVE_SPEED      = 6.0f;   // m/s horizontal target speed
constexpr float ACCEL_RATE      = 20.0f;  // exponential approach rate (1/s)
constexpr float GRAVITY         = 25.0f;  // m/s^2
constexpr float JUMP_VELOCITY   = 8.0f;   // m/s
constexpr float GROUND_Y        = 0.0f;
constexpr float WORLD_RADIUS    = 200.0f; // players are clamped inside this circle

// Yaw convention (matches three.js object rotation around +Y):
//   yaw = 0 faces -Z; forward = (-sin(yaw), 0, -cos(yaw)); right = (cos(yaw), 0, -sin(yaw))

// ---------------------------------------------------------------------------
// Writer / Reader: explicit little-endian primitives.
// ---------------------------------------------------------------------------
class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void i8(int8_t v) { u8(static_cast<uint8_t>(v)); }
    void u16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void u32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    void f32(float v) {
        static_assert(sizeof(float) == sizeof(uint32_t));
        uint32_t b;
        std::memcpy(&b, &v, sizeof(b));
        u32(b);
    }
    // Length-prefixed string, clamped to 255 bytes.
    void str8(std::string_view s) {
        size_t n = std::min(s.size(), static_cast<size_t>(255));
        u8(static_cast<uint8_t>(n));
        bytes(s.data(), n);
    }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t>& buf() { return buf_; }
    const std::vector<uint8_t>& buf() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const void* data, size_t size)
        : data_(static_cast<const uint8_t*>(data)), size_(size) {}
    explicit Reader(std::string_view sv)
        : Reader(sv.data(), sv.size()) {}

    uint8_t u8() {
        if (!need(1)) return 0;
        return data_[pos_++];
    }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    uint16_t u16() {
        uint16_t lo = u8(), hi = u8();
        return static_cast<uint16_t>(lo | (hi << 8));
    }
    uint32_t u32() {
        uint32_t b0 = u8(), b1 = u8(), b2 = u8(), b3 = u8();
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }
    float f32() {
        uint32_t b = u32();
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }
    std::string str8() {
        uint8_t n = u8();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    void skip(size_t n) {
        if (need(n)) pos_ += n;
    }
    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

private:
    bool need(size_t n) {
        if (!ok_ || n > size_ - pos_) { ok_ = false; return false; }
        return true;
    }
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// C2S payload decoding
// ---------------------------------------------------------------------------
struct HelloData {
    std::string name;
};
struct InputData {
    uint16_t seq = 0;
    int8_t moveF = 0;   // -127..127, +1 = forward
    int8_t moveR = 0;   // -127..127, +1 = right
    bool jumpHeld = false;
    float yaw = 0.f;
};

inline bool decodeHello(std::string_view payload, HelloData& out) {
    Reader r(payload);
    uint16_t version = r.u16();
    if (!r.ok() || version != PROTOCOL_VERSION) return false;
    out.name = r.str8();
    return r.ok();
}

inline bool decodeInput(std::string_view payload, InputData& out) {
    Reader r(payload);
    out.seq = r.u16();
    out.moveF = r.i8();
    out.moveR = r.i8();
    uint8_t flags = r.u8();
    out.jumpHeld = (flags & INPUT_JUMP_HELD) != 0;
    out.yaw = r.f32();
    return r.ok() && std::abs(out.yaw) <= 6.29f * 2.f;
}

// ---------------------------------------------------------------------------
// S2C message encoders (each returns a full frame including the id byte)
// ---------------------------------------------------------------------------
struct SpawnInfo {
    uint32_t netId = 0;
    std::string name;
    float x = 0, y = 0, z = 0, yaw = 0;
};

inline void writeSpawnBody(Writer& w, const SpawnInfo& s) {
    w.u32(s.netId);
    w.str8(s.name);
    w.f32(s.x);
    w.f32(s.y);
    w.f32(s.z);
    w.f32(s.yaw);
}

inline std::vector<uint8_t> encodeSpawn(const SpawnInfo& s) {
    Writer w;
    w.u8(S2C_Spawn);
    writeSpawnBody(w, s);
    return std::move(w.buf());
}

inline std::vector<uint8_t> encodeDespawn(uint32_t netId) {
    Writer w;
    w.u8(S2C_Despawn);
    w.u32(netId);
    return std::move(w.buf());
}

inline std::vector<uint8_t> encodeWelcome(uint32_t netId, float x, float y, float z,
                                          const std::vector<SpawnInfo>& roster) {
    Writer w;
    w.u8(S2C_Welcome);
    w.u32(netId);
    w.f32(x);
    w.f32(y);
    w.f32(z);
    w.u16(static_cast<uint16_t>(roster.size()));
    for (const SpawnInfo& s : roster) writeSpawnBody(w, s);
    return std::move(w.buf());
}

struct SnapshotEntry {
    uint32_t netId;
    float x, y, z, yaw;
};

inline std::vector<uint8_t> encodeSnapshot(uint32_t tick, const std::vector<SnapshotEntry>& ents) {
    Writer w;
    w.u8(S2C_Snapshot);
    w.u32(tick);
    w.u16(static_cast<uint16_t>(ents.size()));
    for (const SnapshotEntry& e : ents) {
        w.u32(e.netId);
        w.f32(e.x);
        w.f32(e.y);
        w.f32(e.z);
        w.f32(e.yaw);
    }
    return std::move(w.buf());
}

inline std::vector<uint8_t> encodePong(uint32_t nonce) {
    Writer w;
    w.u8(S2C_Pong);
    w.u32(nonce);
    return std::move(w.buf());
}

} // namespace proto
