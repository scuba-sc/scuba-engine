// game.cvars_write 1.0 (sc_cvars_write.h): reversible bool writes, one owner per name, the first
// prior value kept until restore_all or the owner's release.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/cvars_store.h"
#include "hostd/mock_game/services.h"
#include "sc_cvars_write.h"

namespace scuba::hostd::mock_game {

namespace {

struct Claim {
    const void* owner = nullptr;
    uint32_t prior = 0;
};

std::map<std::string, Claim> g_claims;

void RestoreOwner(const void* owner) {
    for (auto it = g_claims.begin(); it != g_claims.end();) {
        if (it->second.owner == owner) {
            CvarValues()[it->first] = it->second.prior;
            it = g_claims.erase(it);
        } else {
            ++it;
        }
    }
}

sco_result SetBool(sco_plugin* self, const char* name, uint32_t type, uint32_t value) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (!Ready("game.cvars.write")) return SCO_UNAVAILABLE;
    if (!name || !*name || type != SC_CVAR_BOOL || value > 1u) return SCO_BAD_ARG;
    auto& values = CvarValues();
    const auto it = values.find(name);
    if (it == values.end()) return SCO_NOT_FOUND;
    const auto claim = g_claims.find(name);
    if (claim != g_claims.end() && claim->second.owner != self) return SCO_FAILED;
    if (claim == g_claims.end()) g_claims[name] = Claim{ self, it->second };
    it->second = value;
    return SCO_OK;
}

sco_result RestoreAll(sco_plugin* self) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    RestoreOwner(self);
    return SCO_OK;
}

void Reset() { g_claims.clear(); }
void Release(const void* owner) { RestoreOwner(owner); }

const sc_cvars_write_v1 kTable = { sizeof(sc_cvars_write_v1), 0, SetBool, RestoreAll };

}  // namespace

const ServiceDef& CvarsWriteService() {
    static const ServiceDef def{ SC_CVARS_WRITE_NAME, SC_CVARS_WRITE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
