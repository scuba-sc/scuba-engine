// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/kernel.h"
#include "scuba/sco/sco_host.h"

#include <cstdlib>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    scuba::hostd::testing::ForceStopFailure();

    scuba::ScoHost host;
    scuba::ScoHostOptions options;
    options.lua = false;
    if (!host.Start(options)) return 3;

    std::ofstream marker(argv[1], std::ios::binary);
    if (!marker) return 4;
    marker << "started";
    marker.close();
    if (!marker) return 5;

    host.Stop();
    // Reaching here means the void wrapper returned without honoring failure.
    std::_Exit(EXIT_SUCCESS);
}
