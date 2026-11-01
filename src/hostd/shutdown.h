#pragma once

#include "hostd/kernel.h"
#include "hostd/net_sim/broker.h"

namespace scuba::hostd {

struct HostShutdownResult {
    bool peersClean = false;
    bool kernelClean = false;
    bool kernelChecked = false;
};

// Reap network-sandbox children before attempting checked core shutdown. A
// false peer result still means every child was confirmed gone; it can report
// a nonzero exit or a staging-directory cleanup failure.
HostShutdownResult StopHostRuntime(Kernel& kernel, net_sim::Broker& broker);

}  // namespace scuba::hostd
