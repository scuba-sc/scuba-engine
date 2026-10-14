#pragma once
// What every hostd module receives, and the module list. To add a module: write
// src/hostd/<module>_handlers.cpp with `void Register<Module>Handlers(Registry&, HostContext&)`,
// declare it below, and add one call to RegisterAllModules in modules.cpp.
#include "hostd/handlers.h"
#include "hostd/kernel.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scuba::hostd {

class Server;

struct HostContext {
    Kernel* kernel = nullptr;
    const Server* server = nullptr;
    Broadcaster* broadcaster = nullptr;
    std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    std::string pluginRoot;
    // Set: the method system.shutdown exists and calls this (tests only: --test-shutdown).
    std::function<void()> requestShutdown;
    // Run by the main loop on the game thread after every kernel Tick, with the same nowMs.
    std::vector<std::function<void(uint32_t nowMs)>> tickHooks;
};

void RegisterSystemHandlers(Registry& registry, HostContext& host);
void RegisterWidgetsHandlers(Registry& registry, HostContext& host);
void RegisterMockGameHandlers(Registry& registry, HostContext& host);
void RegisterAssetsHandlers(Registry& registry, HostContext& host);

void RegisterAllModules(Registry& registry, HostContext& host);

}  // namespace scuba::hostd
