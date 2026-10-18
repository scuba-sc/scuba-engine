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
    std::memcpy(out, &result, std::min<size_t>(callerSize, sizeof(result)));
    return SCO_OK;
}

void Reset() {}
void Release(const void*) {}

const sc_inventory_v1 kTable{ sizeof(sc_inventory_v1), 0, Snapshot };

}  // namespace

const ServiceDef& InventoryService() {
    static const ServiceDef def{ SC_INVENTORY_SERVICE_NAME, SC_INVENTORY_SERVICE_VERSION,
                                 &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
