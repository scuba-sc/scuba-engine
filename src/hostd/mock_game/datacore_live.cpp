// Synthetic, read-only DataCore pools. No access to native game memory.
#include "hostd/mock_game/datacore_live.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_datacore_live.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace scuba::hostd::mock_game {
namespace {
constexpr std::array<uint64_t, SC_DC_POOL_COUNT> kWidths{1,1,2,4,8,1,2,4,8,4,8,16,1};
constexpr uint8_t kUint32Pool[] = {1,0,0,0,2,0,0,0};
constexpr uint8_t kGuid[16] = {1};
sco_result Status(sco_plugin* self) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    return Ready(SC_DATACORE_LIVE_CAPABILITY) ? SCO_OK : SCO_UNAVAILABLE;
}
sco_result IsReady(sco_plugin* self) { return Status(self); }
sco_result Stats(sco_plugin* self, uint32_t type, sc_datacore_pool_stats* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || out->size < sizeof(*out) || type >= SC_DC_POOL_COUNT) return SCO_BAD_ARG;
    if (!Ready(SC_DATACORE_LIVE_CAPABILITY)) return SCO_UNAVAILABLE;
    const uint32_t size = out->size;
    *out = {};
    out->size = size; out->pool_type = type; out->element_size = kWidths[type];
    out->element_count = type == SC_DC_POOL_UINT32 ? 2 : 0;
    out->capacity = out->element_count;
    out->total_bytes = out->capacity * out->element_size;
    return SCO_OK;
}
sco_result Find(sco_plugin* self, const uint8_t guid[16], sc_datacore_live_record_info* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !guid || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    if (!Ready(SC_DATACORE_LIVE_CAPABILITY)) return SCO_UNAVAILABLE;
    if (std::memcmp(guid, kGuid, sizeof(kGuid)) != 0) return SCO_NOT_FOUND;
    const uint32_t size = out->size;
    *out = {}; out->size = size;
    std::memcpy(out->guid, kGuid, sizeof(kGuid));
    out->struct_type = 1; out->field_count = 2;
    return SCO_OK;
}
sco_result Read(sco_plugin* self, uint32_t type, uint64_t offset, void* out, size_t capacity, size_t* count) {
    if (count) *count = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || type >= SC_DC_POOL_COUNT || !count || !out || !capacity) return SCO_BAD_ARG;
    if (!Ready(SC_DATACORE_LIVE_CAPABILITY)) return SCO_UNAVAILABLE;
    const size_t length = type == SC_DC_POOL_UINT32 ? sizeof(kUint32Pool) : 0;
    if (offset > length) return SCO_BAD_ARG;
    const size_t n = std::min(capacity, length - static_cast<size_t>(offset));
    if (n) std::memcpy(out, kUint32Pool + static_cast<size_t>(offset), n);
    *count = n;
    return SCO_OK;
}
sco_result Write(sco_plugin* self, uint32_t type, uint64_t, const void* input, size_t size, size_t* count) {
    if (count) *count = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || type >= SC_DC_POOL_COUNT || !input || !size || !count) return SCO_BAD_ARG;
    return SCO_UNAVAILABLE;
}
void Reset() {}
void Release(const void*) {}
const sc_datacore_live_v1 kTable{sizeof(sc_datacore_live_v1), 0, IsReady, Stats, Find, Read, Write};
}
const ServiceDef& DatacoreLiveService() {
    static const ServiceDef def{SC_DATACORE_LIVE_SERVICE_NAME, SC_DATACORE_LIVE_SERVICE_VERSION,
                                &kTable, Reset, Release, nullptr};
    return def;
}
}
