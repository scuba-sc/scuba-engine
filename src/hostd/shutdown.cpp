#include "hostd/shutdown.h"

namespace scuba::hostd {

HostShutdownResult StopHostRuntime(Kernel& kernel, net_sim::Broker& broker) {
    HostShutdownResult result;
    result.peersClean = broker.Stop();
    if (!result.peersClean)
        (void)broker.Stop(); // retry residual data-root removal before a retained core can _Exit
#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
    result.kernelChecked = true;
    const auto stopped = kernel.StopDetailed();
    result.kernelClean = stopped == KernelStopResult::Stopped || stopped == KernelStopResult::NotStarted;
#else
    kernel.Stop();
#endif
    return result;
}

}  // namespace scuba::hostd
