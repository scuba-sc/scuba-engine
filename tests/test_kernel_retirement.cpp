// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"
#include <thread>

int main() {
    scuba::hostd::Kernel host;
    scuba::hostd::KernelOptions options;
    CHECK(host.Start(options, nullptr));
    CHECK(host.started() && scuba::hostd::mock_game::Published());
    bool stopped = true;
    std::thread wrongThread([&] { stopped = host.Stop(); });
    wrongThread.join();
    CHECK(!stopped && !host.started());
    CHECK(scuba::hostd::mock_game::Published()); // no teardown without the core's consent
    CHECK(host.profileError().find("retained until process exit") != std::string::npos);
    CHECK(!host.Stop() && !host.Start(options, nullptr));
    return CheckExit("kernel_retirement");
}
