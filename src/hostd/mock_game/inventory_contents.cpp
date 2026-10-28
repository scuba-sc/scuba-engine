// Synthetic game.inventory.contents; no engine data or writes.
#include "hostd/mock_game/inventory_contents.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace scuba::hostd::mock_game {
namespace {
InventoryContentsValues values;
void Reset() { values = {}; }
void Release(const void*) {}
sco_result Snapshot(sco_plugin* self, uint64_t id, sc_container_snapshot* out) {
    const bool sized = out && out->size >= sizeof(*out);
    const uint32_t size = sized ? out->size : 0;
    const uint32_t capacity = sized ? out->count : 0;
    if (sized) { std::memset(out, 0, sizeof(*out)); out->size = size; }
    try {
        if (!OnGame()) return SCO_WRONG_THREAD;
        if (!LivePlugin(self) || !id || !sized || capacity > SC_CONTAINER_CONTENTS_MAX_ITEMS) return SCO_BAD_ARG;
        const auto services = Services();
        const auto service = std::find_if(services.begin(), services.end(), [](const ServiceInfo& s) { return s.name == SC_CONTAINER_CONTENTS_NAME; });
        if (!Ready("game.inventory.contents.read") || !Published() || service == services.end() || !service->published || !values.available) return SCO_UNAVAILABLE;
        const auto it = values.containers.find(id);
        if (it == values.containers.end()) return SCO_NOT_FOUND;
        const auto& items = it->second;
        if (!values.generation || items.size() > SC_CONTAINER_CONTENTS_MAX_ITEMS) return SCO_FAILED;
        uint64_t volume = 0;
        std::set<uint64_t> ids;
        for (const auto& item : items) {
            if (item.size != sizeof(item) || !item.quantity || !item.item_id || !item.class_id || !ids.insert(item.item_id).second) return SCO_FAILED;
            if (values.volumeKnown) {
                if (item.unit_volume > std::numeric_limits<uint64_t>::max() / item.quantity) return SCO_FAILED;
                const auto part = item.unit_volume * item.quantity;
                if (volume > std::numeric_limits<uint64_t>::max() - part) return SCO_FAILED;
                volume += part;
            } else if (item.unit_volume) return SCO_FAILED;
        }
        out->count = static_cast<uint32_t>(std::min<size_t>(capacity, items.size()));
        out->generation = values.generation;
        out->container_id = id;
        out->used_volume = volume;
        out->total_seen = static_cast<uint32_t>(items.size());
        out->flags = (values.volumeKnown ? SC_CONTAINER_CONTENTS_VOLUME_KNOWN : 0) | (out->count < items.size() ? SC_CONTAINER_CONTENTS_TRUNCATED : 0);
        std::copy_n(items.begin(), out->count, out->items);
        return SCO_OK;
    } catch (...) {
        if (sized) { std::memset(out, 0, sizeof(*out)); out->size = size; }
        return SCO_FAILED;
    }
}
const sc_container_contents_v1 table{sizeof(table), 0, Snapshot};
}
InventoryContentsValues& InventoryContents() { return values; }
const ServiceDef& InventoryContentsService() {
    static const ServiceDef def{SC_CONTAINER_CONTENTS_NAME, SC_CONTAINER_CONTENTS_VERSION_1_0, &table, Reset, Release, nullptr};
    return def;
}
}
