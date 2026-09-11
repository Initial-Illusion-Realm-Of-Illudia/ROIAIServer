#include "NetServer.hpp"
#include "Simulation.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    game::Config cfg;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string val = argv[i + 1];
        if (key == "--port") cfg.port = std::atoi(val.c_str());
        else if (key == "--sim-rate") cfg.simRate = std::atoi(val.c_str());
        else if (key == "--snap-rate") cfg.snapRate = std::atoi(val.c_str());
        else if (key == "--web-dir") cfg.webDir = val;
    }

    std::printf("airoi mmo server\n"
                "  port:      %d\n"
                "  sim rate:  %d Hz\n"
                "  snap rate: %d Hz\n"
                "  movement:  server-authoritative input state (see README)\n",
                cfg.port, cfg.simRate, cfg.snapRate);

    game::Bridge bridge;
    NetServer net(bridge, cfg);

    game::Simulation sim(bridge, cfg, net.simPoster());
    sim.start();

    net.run(); // blocks until the process is terminated

    sim.stop();
    return 0;
}
