// game.reputation 1.0 (sc_reputation.h): synthetic check-site counts, read only.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_reputation.h"

namespace scuba::hostd::mock_game {

namespace {

ErrorBook g_err;
sc_reputation_checks g_checks{ sizeof(sc_reputation_checks), 4, 4, 1 };

sco_result Checks(sco_plugin* self, sc_reputation_checks* out) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready("game.reputation.checks")) return g_err.Fail(self, SCO_UNAVAILABLE, "game.reputation.checks isn't ready");
    if (!out || out->size < sizeof(sc_reputation_checks))
        return g_err.Fail(self, SCO_BAD_ARG, "out is NULL or smaller than sc_reputation_checks");
    out->sites = g_checks.sites;
    out->intact = g_checks.intact;
    out->services = g_checks.services;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

void Reset() {
    g_err.Reset();
    g_checks = sc_reputation_checks{ sizeof(sc_reputation_checks), 4, 4, 1 };
}
void Release(const void* owner) { g_err.Forget(owner); }

const sc_reputation_v1 kTable = { sizeof(sc_reputation_v1), 0, Checks, LastError };

}  // namespace

sc_reputation_checks& ReputationChecks() { return g_checks; }

const ServiceDef& ReputationService() {
    static const ServiceDef def{ SC_REPUTATION_SERVICE_NAME, SC_REPUTATION_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
