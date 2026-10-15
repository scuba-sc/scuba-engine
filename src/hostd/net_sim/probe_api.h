#pragma once
#include "sco_api.h"

// Synthetic sample service, not an SDK extension. Game-thread calls only.
struct NetSimProbeStats {
    uint32_t reliableSent, unreliableSent, reliableReceived, unreliableReceived;
};
struct NetSimProbe {
    uint32_t size;
    sco_result (*send)(uint32_t count);
    NetSimProbeStats (*stats)();
};
const sco_plugin_info* NetSimProbeQuery();
sco_result NetSimProbeLoad(const sco_api* api, sco_plugin* self);
void NetSimProbeUnload();
