// game.contracts 1.0 (sc_contracts.h): the synthetic mission stream radius, read only.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_contracts.h"

namespace scuba::hostd::mock_game {

namespace {

ErrorBook g_err;
ContractsValues g_values;

sco_result StreamRadius(sco_plugin* self, float* outRadius) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready("game.contracts.stream_radius"))
        return g_err.Fail(self, SCO_UNAVAILABLE, "game.contracts.stream_radius isn't ready");
    if (!outRadius) return g_err.Fail(self, SCO_BAD_ARG, "out_radius is NULL");
    if (!g_values.settingsLoaded) return g_err.Fail(self, SCO_NOT_FOUND, "the synthetic mission settings aren't loaded");
    *outRadius = g_values.streamRadius;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

void Reset() {
    g_err.Reset();
    g_values = ContractsValues{};
}
void Release(const void* owner) { g_err.Forget(owner); }

const sc_contracts_v1 kTable = { sizeof(sc_contracts_v1), 0, StreamRadius, LastError };

}  // namespace

ContractsValues& Contracts() { return g_values; }

const ServiceDef& ContractsService() {
    static const ServiceDef def{ SC_CONTRACTS_SERVICE_NAME, SC_CONTRACTS_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
