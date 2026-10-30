// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "scuba/sco/sco_host.h"
#include <thread>

int main() {
    scuba::ScoHost host;
    CHECK(host.Start({}));
    bool stopped = true;
    std::thread wrongThread([&] { stopped = host.Stop(); });
    wrongThread.join();
    CHECK(!stopped);
    CHECK(!host.Stop()); // a second call must not hide the retained state
    CHECK(!host.Start({}));
    return CheckExit("sco_host_retirement");
}
