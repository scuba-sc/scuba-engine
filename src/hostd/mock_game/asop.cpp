// game.asop 1.0 (sc_asop.h): two synthetic ship-terminal values, read only.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_asop.h"

namespace scuba::hostd::mock_game {

namespace {

ErrorBook g_err;
AsopValues g_values;

sco_result ClaimTimeout(sco_plugin* self, int32_t* outSeconds) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready("game.asop.claim_timeout")) return g_err.Fail(self, SCO_UNAVAILABLE, "game.asop.claim_timeout isn't ready");
    if (!outSeconds) return g_err.Fail(self, SCO_BAD_ARG, "out_seconds is NULL");
    if (!g_values.configLoaded) return g_err.Fail(self, SCO_NOT_FOUND, "the synthetic insurance configuration isn't loaded");
    *outSeconds = g_values.claimTimeoutSeconds;
    return SCO_OK;
}

sco_result ShardPersisted(sco_plugin* self, uint32_t* outPersisted) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready("game.asop.shard_persisted")) return g_err.Fail(self, SCO_UNAVAILABLE, "game.asop.shard_persisted isn't ready");
    if (!outPersisted) return g_err.Fail(self, SCO_BAD_ARG, "out_persisted is NULL");
    *outPersisted = g_values.shardPersisted ? 1u : 0u;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

void Reset() {
    g_err.Reset();
    g_values = AsopValues{};
}
void Release(const void* owner) { g_err.Forget(owner); }

const sc_asop_v1 kTable = { sizeof(sc_asop_v1), 0, ClaimTimeout, ShardPersisted, LastError };

}  // namespace

AsopValues& Asop() { return g_values; }

const ServiceDef& AsopService() {
    static const ServiceDef def{ SC_ASOP_SERVICE_NAME, SC_ASOP_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
