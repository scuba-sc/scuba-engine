// scuba-hostd: the sco-core host kernel, headless, behind a local HTTP + WebSocket bridge.
//
//   scuba-hostd [--port N] [--bind ADDR] [--studio DIR] [--plugins DIR] [--data DIR]
//               [--allow-origin ORIGIN]... [--no-lua] [--test-shutdown]
//
// Prints "scuba-hostd: listening port=<N>" once the server accepts connections. Stops on
// SIGINT/SIGTERM (Ctrl+C) and, with --test-shutdown, on the bridge method system.shutdown.
#include "hostd/handlers.h"
#include "hostd/kernel.h"
#include "hostd/modules.h"
#include "hostd/server.h"
#include "hostd/net_sim/broker.h"
#include "hostd/net_sim/peer.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

volatile std::sig_atomic_t g_stopSignal = 0;

void OnSignal(int) { g_stopSignal = 1; }

scuba::hostd::Broadcaster* g_logBroadcaster = nullptr;

void KernelLog(const char* line) {
    std::fprintf(stdout, "%s\n", line);
    std::fflush(stdout);
    if (g_logBroadcaster) {
        scuba::hostd::Json data = scuba::hostd::Json::MakeObject();
        data.set("line", line);
        g_logBroadcaster->broadcast("kernel.log", std::move(data));
    }
}

int Usage() {
    std::fprintf(stderr,
                 "usage: scuba-hostd [--port N] [--bind ADDR] [--studio DIR] [--plugins DIR] [--data DIR]\n"
                 "                   [--allow-origin ORIGIN]... [--no-lua] [--no-mock-game] [--test-shutdown]\n"
                 "  --port N             listen port (default 7878; 0 picks a free port)\n"
                 "  --bind ADDR          IPv4 address to bind (default 127.0.0.1)\n"
                 "  --studio DIR         built studio to serve (default: the source tree's studio/dist)\n"
                 "  --plugins DIR        sco-core plugin root to load plugins from\n"
                 "  --data DIR           data root (sco.storage databases)\n"
                 "  --allow-origin O     also accept WebSocket connections from origin O (e.g. http://localhost:5173)\n"
                 "  --no-lua             no Lua runtime: lua plugins are refused\n"
                 "  --no-mock-game       do not publish synthetic game service tables\n"
                 "  --test-shutdown      enable the system.shutdown method (tests only)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace scuba::hostd;
    if (argc == 2 && std::strcmp(argv[1], "--net-peer") == 0) return net_sim::PeerMain();

    ServerOptions serverOptions;
    serverOptions.staticDir = SCUBA_DEFAULT_STUDIO_DIR;
    KernelOptions kernelOptions;
    bool testShutdown = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(a, "--port") && hasValue) {
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || v < 0 || v > 65535) return Usage();
            serverOptions.port = static_cast<uint16_t>(v);
        } else if (!std::strcmp(a, "--bind") && hasValue) {
            serverOptions.bindAddress = argv[++i];
        } else if (!std::strcmp(a, "--studio") && hasValue) {
            serverOptions.staticDir = argv[++i];
        } else if (!std::strcmp(a, "--plugins") && hasValue) {
            kernelOptions.pluginRoot = argv[++i];
        } else if (!std::strcmp(a, "--data") && hasValue) {
            kernelOptions.dataRoot = argv[++i];
        } else if (!std::strcmp(a, "--allow-origin") && hasValue) {
            serverOptions.allowedOrigins.emplace_back(argv[++i]);
        } else if (!std::strcmp(a, "--no-lua")) {
            kernelOptions.lua = false;
        } else if (!std::strcmp(a, "--no-mock-game")) {
            kernelOptions.mockGame = false;
        } else if (!std::strcmp(a, "--test-shutdown")) {
            testShutdown = true;
        } else {
            return Usage();
        }
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    Registry registry;
    Broadcaster broadcaster;
    g_logBroadcaster = &broadcaster;
    Kernel kernel;
    Server server(serverOptions, registry, broadcaster);

    bool stopRequested = false;
    HostContext host;
    host.kernel = &kernel;
    host.server = &server;
    host.broadcaster = &broadcaster;
    host.pluginRoot = kernelOptions.pluginRoot.string();
    if (testShutdown) host.requestShutdown = [&stopRequested] { stopRequested = true; };
    RegisterAllModules(registry, host);
    net_sim::RegisterNetSim(registry, broadcaster, argv[0]);

    if (!kernel.Start(kernelOptions, KernelLog)) {
        std::fprintf(stderr, "scuba-hostd: the sco-core host kernel did not start\n");
        return 1;
    }

    std::string error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "scuba-hostd: %s\n", error.c_str());
        kernel.Stop();
        return 1;
    }
    if (serverOptions.bindAddress != "127.0.0.1")
        std::fprintf(stderr, "scuba-hostd: warning: listening on %s, not loopback; the bridge has no authentication\n",
                     serverOptions.bindAddress.c_str());
    std::printf("scuba-hostd: listening port=%u url=http://127.0.0.1:%u/ ws=/ws\n",
                static_cast<unsigned>(server.port()), static_cast<unsigned>(server.port()));
    std::fflush(stdout);

    const auto start = std::chrono::steady_clock::now();
    while (!g_stopSignal && !stopRequested) {
        server.Poll(16);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        const auto nowMs = static_cast<uint32_t>(static_cast<uint64_t>(elapsed.count()));
        kernel.Tick(nowMs);
        for (const auto& hook : host.tickHooks) hook(nowMs);
    }
    server.Poll(0);

    std::printf("scuba-hostd: stopping\n");
    std::fflush(stdout);
    server.Stop();
    kernel.Stop();
    g_logBroadcaster = nullptr;
    return 0;
}
