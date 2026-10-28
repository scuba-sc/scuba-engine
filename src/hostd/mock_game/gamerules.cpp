// game.gamerules 1.0: read-only values from the synthetic session snapshot.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include "sc_gamerules.h"

#include <algorithm>
#include <cstring>

namespace scuba::hostd::mock_game {
namespace {

constexpr const char* kCapability = "game.gamerules.read";

#define PARAM(type, group, name) { sizeof(sc_gamerules_param_info), type, name, group }
constexpr sc_gamerules_param_info kParams[] = {
    PARAM(SC_GAMERULES_PARAM_INT, "control", "capturableDebug"),
    PARAM(SC_GAMERULES_PARAM_INT, "control", "phaseDebug"),
    PARAM(SC_GAMERULES_PARAM_INT, "control", "objectiveDebug"),
    PARAM(SC_GAMERULES_PARAM_INT, "state", "log"),
    PARAM(SC_GAMERULES_PARAM_INT, "scoring", "enabled"),
    PARAM(SC_GAMERULES_PARAM_INT, "scoring", "log"),
    PARAM(SC_GAMERULES_PARAM_INT, "damagehandling", "log"),
    PARAM(SC_GAMERULES_PARAM_INT, "damagehandling", "disableCollisionFilter"),
    PARAM(SC_GAMERULES_PARAM_INT, "", "showRaceCheckpoints"),
    PARAM(SC_GAMERULES_PARAM_INT, "", "showRaceInfo"),
    PARAM(SC_GAMERULES_PARAM_FLOAT, "", "overrideRespawnTimer"),
    PARAM(SC_GAMERULES_PARAM_INT, "", "editorUseSpawnpoints"),
    PARAM(SC_GAMERULES_PARAM_INT, "", "allowNonLocationSpawnpoints"),
    PARAM(SC_GAMERULES_PARAM_INT, "", "despawnOnProjectileHit"),
    PARAM(SC_GAMERULES_PARAM_INT, "", "allowAllPlayerCameras"),
};
#undef PARAM

sco_result Read(sco_plugin* self, sc_gamerules_state* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if ((self && !LivePlugin(self)) || !out || out->size < sizeof(sc_gamerules_state)) return SCO_BAD_ARG;
    Scene& scene = GetScene();
    const auto services = Services();
    const auto service = std::find_if(services.begin(), services.end(), [](const ServiceInfo& item) {
        return std::strcmp(item.name.c_str(), SC_GAMERULES_SERVICE_NAME) == 0;
    });
    if (!Ready(kCapability) || !Published() || service == services.end() || !service->published ||
        !scene.gamerulesReceived) return SCO_UNAVAILABLE;
    const uint32_t callerSize = out->size;
    sc_gamerules_state result = scene.gamerules;
    result.size = callerSize;
    std::memcpy(out, &result, std::min<size_t>(callerSize, sizeof(result)));
    return SCO_OK;
}

void Reset() {}
void Release(const void*) {}

sco_result ListParams(sco_plugin* self, uint32_t offset, sc_gamerules_param_info* out,
                      uint32_t stride, uint32_t cap, uint32_t* outCount, uint32_t* outTotal) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!self || !LivePlugin(self) || !outCount || !outTotal || outCount == outTotal) return SCO_BAD_ARG;
    constexpr uint32_t total = static_cast<uint32_t>(sizeof(kParams) / sizeof(kParams[0]));
    if (offset > total || cap > SC_GAMERULES_PARAM_MAX_PAGE) return SCO_BAD_ARG;
    if ((cap == 0 && out) || (cap != 0 && (!out || stride < sizeof(sc_gamerules_param_info) ||
                                           stride > SC_GAMERULES_PARAM_MAX_STRIDE))) return SCO_BAD_ARG;
    const uint32_t count = std::min(cap, total - offset);
    auto* bytes = reinterpret_cast<unsigned char*>(out);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = 0;
        std::memcpy(&size, bytes + static_cast<size_t>(i) * stride, sizeof(size));
        if (size < sizeof(sc_gamerules_param_info)) return SCO_BAD_ARG;
    }
    if (!Published()) return SCO_UNAVAILABLE;
    for (uint32_t i = 0; i < count; ++i) {
        auto* slot = bytes + static_cast<size_t>(i) * stride;
        const auto& param = kParams[offset + i];
        std::memcpy(slot + offsetof(sc_gamerules_param_info, type), &param.type, sizeof(param.type));
        std::memcpy(slot + offsetof(sc_gamerules_param_info, name), param.name, sizeof(param.name));
        std::memcpy(slot + offsetof(sc_gamerules_param_info, group), param.group, sizeof(param.group));
    }
    *outCount = count;
    *outTotal = total;
    return SCO_OK;
}

const sc_gamerules_v1 kTable{ sizeof(sc_gamerules_v1), 0, Read, ListParams };

}  // namespace

const ServiceDef& GameRulesService() {
    static const ServiceDef def{ SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_1,
                                 &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
