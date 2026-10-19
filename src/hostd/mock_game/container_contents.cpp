// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include <sc_container_contents.h>
#include <cstring>
#include <limits>
namespace scuba::hostd::mock_game {
namespace {
std::map<uint64_t, sc_container_snapshot> values;
sco_result Snapshot(sco_plugin* self, uint64_t id, sc_container_snapshot* out) {
    const bool sized = out && out->size >= sizeof(*out);
    const uint32_t size = sized ? out->size : 0;
    const uint32_t capacity = sized ? out->count : 0;
    if (sized) { std::memset(out, 0, sizeof(*out)); out->size = size; }
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !id || !sized || capacity > SC_CONTAINER_CONTENTS_MAX_ITEMS) return SCO_BAD_ARG;
    if (!Ready("game.inventory.contents.read")) return SCO_UNAVAILABLE;
    const auto it = values.find(id);
    if (it == values.end()) return SCO_NOT_FOUND;
    const auto& full = it->second;
    if (full.size != sizeof(full) || full.container_id != id || !full.generation ||
        full.count > SC_CONTAINER_CONTENTS_MAX_ITEMS || full.total_seen < full.count ||
        (full.flags & ~(SC_CONTAINER_CONTENTS_TRUNCATED | SC_CONTAINER_CONTENTS_VOLUME_KNOWN)) ||
        (full.total_seen > full.count && !(full.flags & SC_CONTAINER_CONTENTS_TRUNCATED))) return SCO_FAILED;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < full.count; ++i) {
        const auto& item = full.items[i];
        if (item.size != sizeof(item) || !item.item_id || !item.class_id || !item.quantity) return SCO_FAILED;
        for (uint32_t j = 0; j < i; ++j) if (full.items[j].item_id == item.item_id) return SCO_FAILED;
        if (full.flags & SC_CONTAINER_CONTENTS_VOLUME_KNOWN) {
            if (item.unit_volume > std::numeric_limits<uint64_t>::max() / item.quantity) return SCO_FAILED;
            const uint64_t volume = item.unit_volume * item.quantity;
            if (sum > std::numeric_limits<uint64_t>::max() - volume) return SCO_FAILED;
            sum += volume;
        } else if (item.unit_volume) return SCO_FAILED;
    }
    if (!(full.flags & SC_CONTAINER_CONTENTS_VOLUME_KNOWN) && full.used_volume) return SCO_FAILED;
    if ((full.flags & SC_CONTAINER_CONTENTS_VOLUME_KNOWN) &&
        (sum > full.used_volume || (full.total_seen == full.count && sum != full.used_volume))) return SCO_FAILED;
    const uint32_t count = full.count < capacity ? full.count : capacity;
    out->count = count; out->total_seen = full.total_seen;
    out->container_id = id; out->generation = full.generation;
    out->used_volume = full.used_volume;
    out->flags = full.flags & SC_CONTAINER_CONTENTS_VOLUME_KNOWN;
    if (full.total_seen > count) out->flags |= SC_CONTAINER_CONTENTS_TRUNCATED;
    std::memcpy(out->items, full.items, count * sizeof(full.items[0]));
    return SCO_OK;
}
void Reset() {
    values.clear();
    sc_container_snapshot full{};
    full.size = sizeof(full); full.container_id = 289; full.generation = 1;
    full.count = 2; full.total_seen = 2;
    full.flags = SC_CONTAINER_CONTENTS_VOLUME_KNOWN; full.used_volume = 16;
    full.items[0] = {sizeof(sc_container_item), 2, 5001, 7001, 3};
    full.items[1] = {sizeof(sc_container_item), 1, 5002, 7002, 10};
    values.emplace(full.container_id, full);
    full = {}; full.size = sizeof(full); full.container_id = 290; full.generation = 1;
    values.emplace(full.container_id, full);
}
void Release(const void*) {}
const sc_container_contents_v1 table{sizeof(table), 0, Snapshot};
}
std::map<uint64_t, sc_container_snapshot>& ContainerContentsValues() { return values; }
const ServiceDef& ContainerContentsService() {
    static const ServiceDef def{SC_CONTAINER_CONTENTS_NAME, SC_CONTAINER_CONTENTS_VERSION_1_0,
        &table, Reset, Release, nullptr};
    return def;
}
}
