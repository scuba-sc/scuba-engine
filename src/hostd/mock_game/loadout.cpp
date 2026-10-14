// game.loadout 1.0 (sc_loadout.h): typed local-player equip of the scene's synthetic items.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_loadout.h"

#include <array>
#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

std::array<std::string, 8> g_equipped;
bool g_applying = false;

bool ValidItemName(const char* name) {
    const void* nul = std::memchr(name, '\0', SC_LOADOUT_ITEM_NAME_BYTES);
    if (!nul) return false;
    const size_t n = static_cast<size_t>(static_cast<const char*>(nul) - name);
    if (n == 0) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

sco_result Apply(sco_plugin* self, const sc_loadout_request* req, sc_loadout_report* report) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (!req || !report || req->size < sizeof(sc_loadout_request) || report->size < sizeof(sc_loadout_report))
        return SCO_BAD_ARG;
    if (g_applying) return SCO_UNAVAILABLE;
    if (req->count > SC_LOADOUT_MAX_ITEMS) return SCO_TOO_MANY;
    if (req->count == 0 || !req->items || req->item_stride != sizeof(sc_loadout_item)) return SCO_BAD_ARG;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < req->count; ++i) {
        const sc_loadout_item& item = req->items[i];
        if (!ValidItemName(item.item_class)) return SCO_BAD_ARG;
        if (!seen.insert(item.slot).second) return SCO_BAD_ARG;
    }
    const uint32_t size = report->size;
    std::memset(report, 0, sizeof(sc_loadout_report));
    report->size = size;
    report->count = req->count;
    if (!Ready("game.loadout.apply")) {
        for (uint32_t i = 0; i < req->count; ++i)
            report->slots[i] = sc_loadout_slot_result{ req->items[i].slot, SC_LOADOUT_UNAVAILABLE };
        return SCO_UNAVAILABLE;
    }
    g_applying = true;
    const Scene& s = GetScene();
    uint32_t applied = 0;
    for (uint32_t i = 0; i < req->count; ++i) {
        const sc_loadout_item& item = req->items[i];
        sc_loadout_status status = SC_LOADOUT_APPLIED;
        if (item.slot < 1 || item.slot > 7)
            status = SC_LOADOUT_UNSUPPORTED;
        else if (!s.items.count(item.item_class))
            status = SC_LOADOUT_ITEM_MISSING;
        else if (!s.spawned)
            status = SC_LOADOUT_FAILED;
        if (status == SC_LOADOUT_APPLIED) {
            g_equipped[item.slot] = item.item_class;
            ++applied;
        }
        report->slots[i] = sc_loadout_slot_result{ item.slot, status };
    }
    g_applying = false;
    report->applied = applied;
    return applied == req->count ? SCO_OK : SCO_FAILED;
}

void Reset() {
    g_equipped.fill(std::string());
    g_applying = false;
}
void Release(const void*) {}

const sc_loadout_v1 kTable = { sizeof(sc_loadout_v1), 0, Apply };

}  // namespace

const std::array<std::string, 8>& EquippedItems() { return g_equipped; }

const ServiceDef& LoadoutService() {
    static const ServiceDef def{ SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
