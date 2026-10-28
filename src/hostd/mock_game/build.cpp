// game.build 1.0: deliberately synthetic identity; no executable is inspected.
#include "hostd/mock_game/services.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/runtime.h"

#include <algorithm>
#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCapability = "game.build";

sco_result Read(sco_plugin*, sc_game_build_info* out) {
    if (!sco::OnGameThread()) return SCO_WRONG_THREAD;
    if (!out) return SCO_BAD_ARG;
    const uint32_t callerSize = out->size;
    if (callerSize < offsetof(sc_game_build_info, pin_matches) + sizeof(out->pin_matches))
        return SCO_BAD_ARG;

    sc_game_build_info record{};
    record.size = sizeof(record);
    record.pack_major = 0;
    record.pack_minor = 0;
    record.pack_patch = 0;
    record.pin_matches = 0;
    std::memcpy(out, &record, std::min<size_t>(callerSize, sizeof(record)));
    const auto services = Services();
    const auto service = std::find_if(services.begin(), services.end(), [](const ServiceInfo& item) {
        return std::strcmp(item.name.c_str(), SC_GAME_BUILD_SERVICE_NAME) == 0;
    });
    if (!Ready(kCapability) || !Published() || service == services.end() || !service->published)
        return SCO_UNAVAILABLE;
    return SCO_OK;
}

void Reset() {}
void Release(const void*) {}

const sc_game_build_v1 kTable{ sizeof(sc_game_build_v1), 0, Read };

}  // namespace

const ServiceDef& BuildService() {
    static const ServiceDef def{ SC_GAME_BUILD_SERVICE_NAME, SC_GAME_BUILD_SERVICE_VERSION,
                                 &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
