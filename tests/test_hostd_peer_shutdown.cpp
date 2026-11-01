// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include "hostd/net_sim/broker.h"
#include "hostd/shutdown.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace scuba::hostd;

int main(int argc, char** argv) {
    if (argc != 2) return 2;

    Registry registry;
    Broadcaster broadcaster;
    auto broker = net_sim::RegisterNetSim(registry, broadcaster, argv[1]);
    Kernel kernel;
    KernelOptions options;
    options.lua = false;
    options.mockGame = false;

    testing::ForceStopFailure();
    CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::Started);
    broker->Start(2, "", false);

    // Production hostd calls the same combined shutdown helper. Its broker
    // branch waits for and reaps children before the injected retained core stop.
    const auto stopped = StopHostRuntime(kernel, *broker);
    CHECK(stopped.peersClean);
    CHECK(!stopped.kernelClean);
    CHECK(kernel.retained());
    std::cout << "hostd peer retirement precedes retained core shutdown\n";
    const int result = CheckExit("hostd_peer_shutdown_order");
    std::fflush(nullptr);
    std::_Exit(result);
}
