// game.mining 1.0 (sc_mining.h): owner-scoped request/active state and synthetic counters.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_mining.h"

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "game.mining";
std::map<const void*, bool> g_requested;
uint64_t g_cells = 0, g_promoted = 0;

bool AnyActive() {
    if (!Ready(kCap)) return false;
    for (const auto& [owner, on] : g_requested)
        if (on) return true;
    return false;
}

sco_result Request(sco_plugin* self, uint32_t enabled) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || enabled > 1u) return SCO_BAD_ARG;
    if (!Ready(kCap)) return SCO_UNAVAILABLE;
    g_requested[self] = enabled == 1u;
    return SCO_OK;
}

sco_result State(sco_plugin* self, sc_mining_state* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || out->size < sizeof(sc_mining_state)) return SCO_BAD_ARG;
    const auto it = g_requested.find(self);
    const bool requested = it != g_requested.end() && it->second;
    out->requested = requested ? 1u : 0u;
    out->active = requested && Ready(kCap) ? 1u : 0u;
    out->block = !Ready(kCap) ? SC_MINING_BLOCK_UNAVAILABLE : SC_MINING_BLOCK_OFF;
    out->cells = g_cells;
    out->promoted = g_promoted;
    out->faults = 0;
    return SCO_OK;
}

void Tick() {
    if (!AnyActive()) return;
    ++g_cells;
    if (g_cells % 8 == 0) ++g_promoted;
}

void Reset() {
    g_requested.clear();
    g_cells = g_promoted = 0;
}
void Release(const void* owner) { g_requested.erase(owner); }

const sc_mining_v1 kTable = { sizeof(sc_mining_v1), 0, Request, State };

}  // namespace

const ServiceDef& MiningService() {
    static const ServiceDef def{ SC_MINING_SERVICE_NAME, SC_MINING_SERVICE_VERSION, &kTable, Reset, Release, Tick };
    return def;
}

}  // namespace scuba::hostd::mock_game
