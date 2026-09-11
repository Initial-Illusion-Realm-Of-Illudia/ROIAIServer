// Network layer: uWebSockets app on the main thread. Owns the session
// registry (loop-thread-only) and forwards events to/from the simulation.
#pragma once

#include <functional>
#include <memory>

#include "Bridge.hpp"
#include "Protocol.hpp"

class NetServer {
public:
    NetServer(game::Bridge& bridge, const game::Config& cfg);
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Blocks on the uWS event loop. Call after Simulation::start().
    void run();

    // Thread-safe poster the simulation uses to hand results to the net loop.
    std::function<void(game::SimToNet&&)> simPoster();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
