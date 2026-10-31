// game.inventory 1.1: a bounded read-only snapshot of the synthetic player's ports.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include "sc_inventory.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace scuba::hostd::mock_game {
namespace {

constexpr const char* kCapability = "game.inventory.read";

sco_result Snapshot(sco_plugin*, sc_inventory_snapshot* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!out || out->size < SC_INVENTORY_SNAPSHOT_SIZE_1_0) return SCO_BAD_ARG;
    const uint32_t callerSize = out->size;
    const uint32_t capacity = std::min(out->count, SC_INVENTORY_MAX_PORTS);
    Scene& scene = GetScene();
    const auto services = Services();
    const auto service = std::find_if(services.begin(), services.end(), [](const ServiceInfo& item) {
        return std::strcmp(item.name.c_str(), SC_INVENTORY_SERVICE_NAME) == 0;
    });
    if (!Ready(kCapability) || !Published() || service == services.end() || !service->published)
        return SCO_UNAVAILABLE;
    if (!scene.spawned || !StreamedEntity(scene.player) || !scene.inventoryAvailable) return SCO_UNAVAILABLE;

    const size_t total = scene.inventoryPorts.size();
    const uint32_t written = static_cast<uint32_t>(std::min<size_t>(capacity, total));
    sc_inventory_snapshot result{};
    result.size = callerSize;
    result.count = written;
    result.generation = scene.inventoryGeneration;
    for (uint32_t i = 0; i < written; ++i) result.ports[i] = scene.inventoryPorts[i];
    if (callerSize >= sizeof(sc_inventory_snapshot)) {
        result.flags = total > written ? SC_INVENTORY_FLAG_TRUNCATED : 0;
        result.total_seen = static_cast<uint32_t>(std::min<size_t>(total, std::numeric_limits<uint32_t>::max()));
    }
    out->count = result.count;
    out->generation = result.generation;
    std::memcpy(out->ports, result.ports, written * sizeof(result.ports[0]));
    if (callerSize >= sizeof(sc_inventory_snapshot)) {
        out->flags = result.flags;
        out->total_seen = result.total_seen;
    }
    return SCO_OK;
}

void Reset() {}
void Release(const void*) {}

sco_result SnapshotLayout(sco_plugin*, sc_inventory_layout_snapshot* out, sc_inventory_layout_entry* entries,
                          uint32_t stride, uint32_t capacity) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!out || (reinterpret_cast<uintptr_t>(out) & 7u) || out->size < sizeof(*out) || !entries ||
        (reinterpret_cast<uintptr_t>(entries) & 7u) || !capacity || stride < sizeof(*entries) ||
        (stride & 7u)) return SCO_BAD_ARG;
    if (capacity > SC_INVENTORY_LAYOUT_MAX_PORTS || stride > SC_INVENTORY_LAYOUT_MAX_STRIDE) return SCO_TOO_MANY;
    const uintptr_t outBegin = reinterpret_cast<uintptr_t>(out);
    const uintptr_t entriesBegin = reinterpret_cast<uintptr_t>(entries);
    const size_t entriesBytes = static_cast<size_t>(stride) * capacity;
    if (outBegin > UINTPTR_MAX - out->size || entriesBegin > UINTPTR_MAX - entriesBytes ||
        (outBegin < entriesBegin + entriesBytes && entriesBegin < outBegin + out->size)) return SCO_BAD_ARG;
    for (uint32_t i = 0; i < capacity; ++i) {
        const auto* row = reinterpret_cast<const sc_inventory_layout_entry*>(
            reinterpret_cast<const unsigned char*>(entries) + static_cast<size_t>(i) * stride);
        if (row->size < sizeof(*row) || row->size > stride) return SCO_BAD_ARG;
    }
    return SCO_UNAVAILABLE;
}

const sc_inventory_v1_2 kTable{ { sizeof(sc_inventory_v1_2), 0, Snapshot }, SnapshotLayout };

}  // namespace

const ServiceDef& InventoryService() {
    static const ServiceDef def{ SC_INVENTORY_SERVICE_NAME, SC_INVENTORY_SERVICE_VERSION_1_2,
                                 &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
