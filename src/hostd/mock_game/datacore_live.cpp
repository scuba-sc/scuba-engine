// SPDX-License-Identifier: GPL-3.0-only
// Explicit synthetic pool fixture for product workflows; never maps game memory.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_datacore_live.h"
#include <array>
#include <cstring>

namespace scuba::hostd::mock_game {
namespace {
std::array<std::array<unsigned char, 256>, SC_DC_POOL_COUNT> pools{};
sco_result Admit(sco_plugin* self) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    return Ready(SC_DATACORE_LIVE_CAPABILITY) ? SCO_OK : SCO_UNAVAILABLE;
}
sco_result ReadyPool(sco_plugin* self) { return Admit(self); }
sco_result Stats(sco_plugin* self, uint32_t type, sc_datacore_pool_stats* out) {
    const auto result = Admit(self); if (result != SCO_OK) return result;
    if (type >= SC_DC_POOL_COUNT || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    const sc_datacore_pool_stats next{sizeof(*out), type, 256, 256, 1, 256};
    *out = next; return SCO_OK;
}
sco_result Find(sco_plugin* self, const uint8_t* id, sc_datacore_live_record_info* out) {
    const auto result = Admit(self); if (result != SCO_OK) return result;
    if (!id || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    // No record registry exists in this synthetic scalar-pool fixture.
    return SCO_NOT_FOUND;
}
sco_result Read(sco_plugin* self, uint32_t type, uint64_t offset, void* out, size_t size, size_t* copied) {
    const auto result = Admit(self); if (result != SCO_OK) return result;
    if (!copied || (!out && size) || type >= SC_DC_POOL_COUNT || offset > pools[type].size() ||
        size > pools[type].size() - static_cast<size_t>(offset)) return SCO_BAD_ARG;
    if (size) std::memcpy(out, pools[type].data() + offset, size);
    *copied = size; return SCO_OK;
}
sco_result Write(sco_plugin* self, uint32_t type, uint64_t offset, const void* value, size_t size, size_t* copied) {
    const auto result = Admit(self); if (result != SCO_OK) return result;
    if (!Ready("game.datacore_live.write")) return SCO_UNAVAILABLE;
    if (!copied || (!value && size) || type >= SC_DC_POOL_COUNT || offset > pools[type].size() ||
        size > pools[type].size() - static_cast<size_t>(offset)) return SCO_BAD_ARG;
    if (size) std::memcpy(pools[type].data() + offset, value, size);
    *copied = size; return SCO_OK;
}
void Reset() { pools = {}; }
void Release(const void*) {}
const sc_datacore_live_v1 table{sizeof(table), 0, ReadyPool, Stats, Find, Read, Write};
}
const ServiceDef& DataCoreLiveService() {
    static const ServiceDef def{SC_DATACORE_LIVE_SERVICE_NAME, SC_DATACORE_LIVE_SERVICE_VERSION,
                               &table, Reset, Release, nullptr}; return def;
}
}
