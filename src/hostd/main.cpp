// scuba-hostd: the sco-core host kernel, headless, behind a local HTTP + WebSocket bridge.
//
//   scuba-hostd [--port N] [--bind ADDR] [--studio DIR] [--plugins DIR] [--data DIR]
//               [--allow-origin ORIGIN]... [--no-lua] [--test-shutdown]
//
// Prints "scuba-hostd: listening port=<N>" once the server accepts connections. Stops on
// SIGINT/SIGTERM (Ctrl+C) and, with --test-shutdown, on the bridge method system.shutdown.
#include "hostd/handlers.h"
#include "hostd/scenario_runner.h"
#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"
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
#include <fstream>
#include <algorithm>
#include <set>
#include <iterator>

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

bool LoadProfile(const std::string& path, scuba::hostd::KernelOptions& options, std::string& error) {
    constexpr size_t kMaxBytes = 65536;
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open profile"; return false; }
    std::string text(kMaxBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto count = static_cast<size_t>(file.gcount());
    if (count > kMaxBytes) { error = "profile exceeds 65536 bytes"; return false; }
    if (file.bad()) { error = "cannot read profile"; return false; }
    text.resize(count);
    auto profile = scuba::hostd::Json::parse(text, &error, true);
    if (!profile || !profile->is_object()) { if (profile) error = "profile must be an object"; return false; }
    for (const auto& key : profile->keys()) {
        if (key != "plugins" && key != "mocks" && key != "services") {
            error = "unknown profile field: " + key; return false;
        }
    }
    for (const char* key : {"plugins", "mocks", "services"}) {
        const auto* list = profile->find(key);
        if (!list || !list->is_array()) { error = std::string(key) + " must be an array"; return false; }
        if (list->size() > 128) { error = std::string(key) + " exceeds 128 entries"; return false; }
        std::set<std::string> seen;
        for (size_t i = 0; i < list->size(); ++i) {
            const auto& item = list->at(i);
            if (!item.is_string() || item.as_string().empty() || item.as_string().size() > 256 || item.as_string().find('\0') != std::string::npos) {
                error = std::string(key) + " entries must be nonempty strings of at most 256 bytes"; return false;
            }
            const auto& name = item.as_string();
            if (!seen.insert(name).second) { error = std::string("duplicate ") + key + " entry: " + name; return false; }
            if (std::string(key) == "plugins") {
                options.profilePlugins.push_back(name);
            } else if (std::string(key) == "mocks") {
                if (name != "game.events") { error = "unknown mock: " + name; return false; }
                options.mockEvents = true;
            } else {
                const auto services = scuba::hostd::mock_game::Services();
                if (std::none_of(services.begin(), services.end(), [&](const auto& service) { return service.name == name; })) {
                    error = "unknown service: " + name; return false;
                }
                options.profileServices.push_back(name);
            }
        }
    }
    options.selectiveProfile = true;
    options.mockGame = true;
    return true;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: scuba-hostd [--port N] [--bind ADDR] [--studio DIR] [--plugins DIR] [--data DIR] [--product DIR]\n"
                 "                   [--allow-origin ORIGIN]... [--no-lua] [--no-mock-game] [--test-shutdown]\n"
                 "  --port N             listen port (default 7878; 0 picks a free port)\n"
                 "  --bind ADDR          IPv4 address to bind (default 127.0.0.1)\n"
                 "  --studio DIR         built studio to serve (default: the source tree's studio/dist)\n"
                 "  --scenario FILE      run bounded synthetic scene controls before listening\n"
                 "  --profile FILE       select plugins, services and mocks (see docs/engine/profiles.md)\n"
                 "  --product DIR        mount a product distribution root: DIR/data/plugins is the roster and\n"
                 "                       DIR/data is the sco.storage root; fail-closed on a refused plugin\n"
                 "  --plugins DIR        plugin root (default: build-tree APP plugins)\n"
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
    kernelOptions.pluginRoot = SCUBA_DEFAULT_PLUGIN_DIR;
    bool testShutdown = false;
    std::string profilePath;
    std::string scenarioPath;
    Json scenario;
    bool noMockGame = false;
    bool pluginRootSet = false, dataRootSet = false;

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
        } else if (!std::strcmp(a, "--scenario") && hasValue) {
            if (!scenarioPath.empty()) return Usage();
            scenarioPath = argv[++i];
        } else if (!std::strcmp(a, "--profile") && hasValue) {
            if (!profilePath.empty()) return Usage();
            profilePath = argv[++i];
        } else if (!std::strcmp(a, "--product") && hasValue) {
            if (!kernelOptions.productRoot.empty()) return Usage();
            kernelOptions.productRoot = argv[++i];
        } else if (!std::strcmp(a, "--plugins") && hasValue) {
            kernelOptions.pluginRoot = argv[++i];
            pluginRootSet = true;
        } else if (!std::strcmp(a, "--data") && hasValue) {
            kernelOptions.dataRoot = argv[++i];
            dataRootSet = true;
        } else if (!std::strcmp(a, "--allow-origin") && hasValue) {
            serverOptions.allowedOrigins.emplace_back(argv[++i]);
        } else if (!std::strcmp(a, "--no-lua")) {
            kernelOptions.lua = false;
        } else if (!std::strcmp(a, "--no-mock-game")) {
            kernelOptions.mockGame = false;
            noMockGame = true;
        } else if (!std::strcmp(a, "--test-shutdown")) {
            testShutdown = true;
        } else {
            return Usage();
        }
    }

    if (!kernelOptions.productRoot.empty()) {
        if (!profilePath.empty()) { std::fprintf(stderr, "scuba-hostd: --product conflicts with --profile\n"); return 1; }
        if (pluginRootSet) { std::fprintf(stderr, "scuba-hostd: --product conflicts with --plugins\n"); return 1; }
        if (dataRootSet) { std::fprintf(stderr, "scuba-hostd: --product conflicts with --data\n"); return 1; }
        if (noMockGame) { std::fprintf(stderr, "scuba-hostd: --product conflicts with --no-mock-game\n"); return 1; }
        kernelOptions.pluginRoot = kernelOptions.productRoot / "data" / "plugins";
        kernelOptions.dataRoot = kernelOptions.productRoot / "data";
    }

    if (!profilePath.empty()) {
        if (noMockGame) { std::fprintf(stderr, "scuba-hostd: --profile conflicts with --no-mock-game\n"); return 1; }
        std::string error;
        if (!LoadProfile(profilePath, kernelOptions, error)) { std::fprintf(stderr, "scuba-hostd: profile: %s\n", error.c_str()); return 1; }
    }

    if (!scenarioPath.empty()) {
        std::string error;
        if (!LoadScenario(scenarioPath, scenario, error)) { std::fprintf(stderr, "scuba-hostd: scenario: %s\n", error.c_str()); return 1; }
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
        std::fprintf(stderr, "scuba-hostd: the sco-core host kernel did not start%s%s\n",
                     kernel.profileError().empty() ? "" : ": ", kernel.profileError().c_str());
        return 1;
    }

    if (!scenarioPath.empty()) {
        const bool passed = RunScenario(scenario);
        return kernel.Stop() && passed ? 0 : 1;
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
    const bool stopped = kernel.Stop();
    g_logBroadcaster = nullptr;
    return stopped ? 0 : 1;
}
