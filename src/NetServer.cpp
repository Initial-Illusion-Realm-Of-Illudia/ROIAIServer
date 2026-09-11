#include "NetServer.hpp"

#include <App.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

struct NetServer::Impl {
    struct WsUserData {
        uint64_t connId = 0;
    };

    using WS = uWS::WebSocket<false, true, WsUserData>;

    game::Bridge& bridge;
    game::Config cfg;
    uWS::App app;
    std::unordered_map<uint64_t, WS*> live; // loop thread only
    std::unordered_set<uint64_t> saidHello;
    uint64_t nextConnId = 1;
    std::vector<char> html;

    Impl(game::Bridge& bridge_, const game::Config& cfg_)
        : bridge(bridge_), cfg(cfg_), app() {
        loadClientHtml();
        setupRoutes();
    }

    void loadClientHtml() {
        const char* root = AIROI_SOURCE_ROOT;
        std::ifstream f(cfg.webDir + "/index.html", std::ios::binary);
        if (!f) f.open(std::string(root) + "/client/index.html", std::ios::binary);
        if (f) {
            html.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            std::printf("[net] serving browser client: %zu bytes\n", html.size());
        } else {
            static const char fallback[] =
                "<!doctype html><meta charset=utf-8><title>airoi server</title>"
                "<p>Browser client not found (expected client/index.html). "
                "The WebSocket endpoint is live at this port.</p>";
            html.assign(fallback, fallback + sizeof(fallback) - 1);
        }
    }

    void setupRoutes() {
        uWS::WebSocketBehavior<WsUserData> behavior;
        behavior.compression = uWS::CompressOptions(0); // disabled (no-zlib build)
        behavior.maxPayloadLength = proto::MAX_PAYLOAD;
        behavior.idleTimeout = 120;
        behavior.maxBackpressure = 64 * 1024;
        behavior.closeOnBackpressureLimit = false;
        behavior.resetIdleTimeoutOnSend = true;
        behavior.sendPingsAutomatically = true;
        behavior.open = [this](WS* ws) {
            ws->getUserData()->connId = nextConnId++;
            live[ws->getUserData()->connId] = ws;
        };
        behavior.message = [this](WS* ws, std::string_view msg, uWS::OpCode op) {
            onMessage(ws, msg, op);
        };
        behavior.close = [this](WS* ws, int, std::string_view) {
            const uint64_t connId = ws->getUserData()->connId;
            live.erase(connId);
            saidHello.erase(connId);
            bridge.pushDisconnect(connId);
        };
        app.ws<WsUserData>("/*", std::move(behavior));

        app.get("/", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest*) {
            res->writeStatus("200 OK")
                ->writeHeader("Content-Type", "text/html; charset=utf-8")
                ->end(std::string_view(html.data(), html.size()));
        });
        app.get("/health", [](uWS::HttpResponse<false>* res, uWS::HttpRequest*) {
            res->writeStatus("200 OK")->end("ok\n");
        });
        app.any("/*", [](uWS::HttpResponse<false>* res, uWS::HttpRequest*) {
            res->writeStatus("404 Not Found")->end();
        });

        app.listen("", cfg.port, [this](us_listen_socket_t* ls) {
            if (ls) {
                std::printf("[net] listening on http://0.0.0.0:%d (ws at /)\n", cfg.port);
            } else {
                std::fprintf(stderr, "[net] failed to listen on port %d\n", cfg.port);
                std::exit(1);
            }
        });
    }

    void onMessage(WS* ws, std::string_view msg, uWS::OpCode op) {
        if (op != uWS::OpCode::BINARY || msg.empty()) return;
        const uint8_t id = static_cast<uint8_t>(msg[0]);
        const std::string_view payload = msg.substr(1);
        const uint64_t connId = ws->getUserData()->connId;

        switch (id) {
        case proto::C2S_Hello: {
            if (!saidHello.insert(connId).second) return;
            proto::HelloData h;
            if (!proto::decodeHello(payload, h)) {
                ws->close();
                return;
            }
            bridge.pushConnect(connId, h.name);
            break;
        }
        case proto::C2S_Input: {
            proto::InputData in;
            if (!proto::decodeInput(payload, in)) return; // malformed: ignore
            game::ClientInput ci;
            ci.seq = in.seq;
            ci.moveF = in.moveF;
            ci.moveR = in.moveR;
            ci.jumpHeld = in.jumpHeld;
            ci.yaw = in.yaw;
            bridge.pushInput(connId, ci);
            break;
        }
        case proto::C2S_Ping: {
            proto::Reader r(payload);
            const uint32_t nonce = r.u32();
            if (!r.ok()) return;
            bridge.pushPing(connId, nonce);
            break;
        }
        default:
            break; // unknown message: ignore
        }
    }

    void sendTo(uint64_t connId, const std::vector<uint8_t>& buf) {
        auto it = live.find(connId);
        if (it == live.end()) return;
        it->second->send(std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()),
                         uWS::OpCode::BINARY);
    }

    void sendAll(const std::vector<uint8_t>& buf, bool skipCongested) {
        const std::string_view sv(reinterpret_cast<const char*>(buf.data()), buf.size());
        for (auto& [connId, ws] : live) {
            if (skipCongested && ws->getBufferedAmount() > 64 * 1024) continue;
            ws->send(sv, uWS::OpCode::BINARY);
        }
    }

    void flush(game::SimToNet&& out) {
        for (const game::SimToNet::Welcome& w : out.welcomes) {
            sendTo(w.connId, proto::encodeWelcome(w.netId, w.x, w.y, w.z, w.roster));
        }
        for (const proto::SpawnInfo& s : out.spawns) {
            sendAll(proto::encodeSpawn(s), false);
        }
        for (uint32_t netId : out.despawns) {
            sendAll(proto::encodeDespawn(netId), false);
        }
        if (!out.snapshot.empty()) sendAll(out.snapshot, true);
        for (const game::SimToNet::Pong& p : out.pongs) {
            sendTo(p.connId, proto::encodePong(p.nonce));
        }
    }
};

NetServer::NetServer(game::Bridge& bridge, const game::Config& cfg)
    : impl_(std::make_unique<Impl>(bridge, cfg)) {}

NetServer::~NetServer() = default;

void NetServer::run() {
    impl_->app.run();
}

std::function<void(game::SimToNet&&)> NetServer::simPoster() {
    Impl* impl = impl_.get();
    return [impl](game::SimToNet&& out) {
        impl->app.loop()->defer([impl, out = std::move(out)]() mutable {
            impl->flush(std::move(out));
        });
    };
}
