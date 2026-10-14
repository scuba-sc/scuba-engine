// game.cvars 1.0 (sc_cvars.h): scoped bool reads of synthetic console variables.
#include "hostd/mock_game/cvars_store.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_cvars.h"

namespace scuba::hostd::mock_game {

namespace {

std::map<std::string, uint32_t> g_values;

sco_result GetBool(sco_plugin* self, const char* name, uint32_t type, uint32_t* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (!Ready("game.cvars.read")) return SCO_UNAVAILABLE;
    if (!name || !*name || !out || type != SC_CVAR_BOOL) return SCO_BAD_ARG;
    const auto it = g_values.find(name);
    if (it == g_values.end()) return SCO_NOT_FOUND;
    *out = it->second;
    return SCO_OK;
}

void Reset() { g_values = { { "mock_hud_enabled", 1u }, { "mock_debug_draw", 0u }, { "mock_volumetric_fog", 1u } }; }
void Release(const void*) {}

const sc_cvars_v1 kTable = { sizeof(sc_cvars_v1), 0, GetBool };

}  // namespace

std::map<std::string, uint32_t>& CvarValues() { return g_values; }

const ServiceDef& CvarsService() {
    static const ServiceDef def{ SC_CVARS_NAME, SC_CVARS_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
