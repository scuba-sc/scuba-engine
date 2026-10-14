// SPDX-License-Identifier: GPL-3.0-only
#include "services.h"

#include "sco_datacore.h"

#include <cstddef>
#include <cmath>
#include <cstring>

#define SHOP_COVERS(table, type, member) \
    ((table)->size >= offsetof(type, member) + sizeof((table)->member) && (table)->member != nullptr)

namespace shop {

const char* ResultName(sco_result r) noexcept {
    switch (r) {
        case SCO_OK: return "ok";
        case SCO_UNAVAILABLE: return "unavailable";
        case SCO_NOT_FOUND: return "not_found";
        case SCO_BAD_ARG: return "bad_arg";
        case SCO_CRASHED: return "crashed";
        case SCO_WRONG_THREAD: return "wrong_thread";
        case SCO_TOO_MANY: return "too_many";
        case SCO_FAILED: return "failed";
        default: return "unknown";
    }
}

namespace {

const char* LoadoutStatusName(sc_loadout_status s) noexcept {
    switch (s) {
        case SC_LOADOUT_APPLIED: return "applied";
        case SC_LOADOUT_UNSUPPORTED: return "unsupported";
        case SC_LOADOUT_ITEM_MISSING: return "item missing";
        case SC_LOADOUT_FAILED: return "failed";
        case SC_LOADOUT_UNAVAILABLE: return "unavailable";
        default: return "unknown";
    }
}

}  // namespace

bool Services::Has(const char* capability) const noexcept {
    return api_ && api_->has && api_->has(capability) != 0;
}

sco_result Services::Query(const char* name, uint32_t version, const void** out, std::string& why) const {
    *out = nullptr;
    if (!api_ || !self_) {
        why = "plugin is not loaded";
        return SCO_UNAVAILABLE;
    }
    if (api_->size < offsetof(sco_api, query_service) + sizeof(api_->query_service) || !api_->query_service) {
        why = std::string(name) + ": no services (sco_api 1.0)";
        return SCO_UNAVAILABLE;
    }
    const void* table = nullptr;
    const sco_result r = api_->query_service(name, version, &table);
    if (r != SCO_OK || !table) {
        why = std::string(name) + " is not published by this host (" + ResultName(r) + ")";
        return SCO_UNAVAILABLE;
    }
    uint32_t size = 0;
    std::memcpy(&size, table, sizeof(size));
    if (size < 8) {
        why = std::string(name) + " table is too short";
        return SCO_UNAVAILABLE;
    }
    *out = table;
    return SCO_OK;
}

sco_result Services::Balance(int64_t& out, std::string& why) const {
    out = 0;
    const void* raw = nullptr;
    if (const sco_result r = Query(SC_ECONOMY_SERVICE_NAME, SC_ECONOMY_SERVICE_VERSION, &raw, why); r != SCO_OK) return r;
    if (!Has(kCapEconomyRead)) {
        why = std::string(kCapEconomyRead) + " is off on this host";
        return SCO_UNAVAILABLE;
    }
    const auto* t = static_cast<const sc_economy_v1*>(raw);
    if (!SHOP_COVERS(t, sc_economy_v1, balance)) {
        why = "game.economy table has no balance";
        return SCO_UNAVAILABLE;
    }
    sc_economy_balance b{};
    b.size = sizeof(b);
    const sco_result r = t->balance(self_, &b);
    if (r != SCO_OK) {
        why = std::string("balance: ") + ResultName(r);
        return r;
    }
    out = b.local_balance;
    why.clear();
    return SCO_OK;
}

sco_result Services::Pay(uint64_t key, int64_t price, sc_economy_receipt& out, std::string& why) const {
    out = {};
    if (key == 0 || price <= 0 || price >= SC_ECONOMY_BALANCE_LIMIT) {
        why = "bad payment key or price";
        return SCO_BAD_ARG;
    }
    int64_t balance = 0;
    if (const sco_result r = Balance(balance, why); r != SCO_OK) return r;
    if (balance < price) {
        why = "insufficient local balance: have " + FormatCredits(balance) + ", need " + FormatCredits(price);
        return SCO_FAILED;
    }
    const void* raw = nullptr;
    if (const sco_result r = Query(SC_ECONOMY_SERVICE_NAME, SC_ECONOMY_SERVICE_VERSION, &raw, why); r != SCO_OK) return r;
    if (!Has(kCapEconomyApply)) {
        why = std::string(kCapEconomyApply) + " is off on this host";
        return SCO_UNAVAILABLE;
    }
    const auto* t = static_cast<const sc_economy_v1*>(raw);
    if (!SHOP_COVERS(t, sc_economy_v1, apply)) {
        why = "game.economy table has no apply";
        return SCO_UNAVAILABLE;
    }
    sc_economy_request request{};
    request.size = sizeof(request);
    request.key = key;
    request.expected_balance = balance;
    request.delta = -price;
    out.size = sizeof(out);
    const sco_result r = t->apply(self_, &request, &out);
    if (r != SCO_OK) {
        why = std::string("apply: ") + ResultName(r);
        return r;
    }
    why.clear();
    return SCO_OK;
}

sco_result Services::Equip(const Item& item, sc_loadout_report& report, std::string& why) const {
    report = {};
    report.size = sizeof(report);
    if (item.slot == 0 || !item.itemClass || !ValidItemClass(item.itemClass)) {
        why = std::string(item.id) + " is not wearable";
        return SCO_BAD_ARG;
    }
    const void* raw = nullptr;
    if (const sco_result r = Query(SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_SERVICE_VERSION, &raw, why); r != SCO_OK) return r;
    if (!Has(kCapLoadoutApply)) {
        why = std::string(kCapLoadoutApply) + " is off on this host";
        return SCO_UNAVAILABLE;
    }
    const auto* t = static_cast<const sc_loadout_v1*>(raw);
    if (!SHOP_COVERS(t, sc_loadout_v1, apply)) {
        why = "game.loadout table has no apply";
        return SCO_UNAVAILABLE;
    }
    sc_loadout_item entry{};
    entry.slot = item.slot;
    std::memcpy(entry.item_class, item.itemClass, std::strlen(item.itemClass) + 1);
    sc_loadout_request request{};
    request.size = sizeof(request);
    request.count = 1;
    request.item_stride = sizeof(sc_loadout_item);
    request.items = &entry;
    const sco_result r = t->apply(self_, &request, &report);
    if (report.count >= 1)
        why = std::string(SlotName(item.slot)) + ": " + LoadoutStatusName(report.slots[0].status);
    else
        why = std::string("apply: ") + ResultName(r);
    return r;
}

sco_result Services::Spawn(const Item& item, uint64_t zone, const double pos[3], uint64_t& entity, std::string& why) const {
    entity = 0;
    if (!item.prefabPath || !ValidPrefabPath(item.prefabPath)) {
        why = std::string(item.id) + " is not a prefab";
        return SCO_BAD_ARG;
    }
    if (zone == 0) {
        why = "zone id must be non-zero";
        return SCO_BAD_ARG;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(pos[axis])) {
            why = "position must be finite";
            return SCO_BAD_ARG;
        }
    }
    const void* raw = nullptr;
    if (const sco_result r = Query(SC_PREFABS_NAME, SC_PREFABS_VERSION_1_0, &raw, why); r != SCO_OK) return r;
    if (!Has(kCapPrefabsSpawn)) {
        why = std::string(kCapPrefabsSpawn) + " is off on this host";
        return SCO_UNAVAILABLE;
    }
    const auto* t = static_cast<const sc_prefabs_v1*>(raw);
    if (!SHOP_COVERS(t, sc_prefabs_v1, spawn)) {
        why = "game.prefabs table has no spawn";
        return SCO_UNAVAILABLE;
    }
    const double rot[4] = {0.0, 0.0, 0.0, 1.0};
    const sco_result r = t->spawn(self_, item.prefabPath, zone, pos, rot, &entity);
    why = r == SCO_OK ? "spawn started" : std::string("spawn: ") + ResultName(r);
    return r;
}

std::string Services::DataCoreState() const {
    const void* raw = nullptr;
    std::string why;
    if (Query(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0, &raw, why) != SCO_OK) return "not published";
    const auto* t = static_cast<const sco_datacore_v1*>(raw);
    if (!SHOP_COVERS(t, sco_datacore_v1, state)) return "no state()";
    const uint32_t s = t->state();
    return s == SCO_DC_OPEN ? "open" : s == SCO_DC_LOADED ? "loaded" : "unknown state";
}

std::string Services::Describe() const {
    auto clause = [this](const char* label, const char* name, uint32_t version, const char* capA, const char* capB) {
        const void* raw = nullptr;
        std::string why;
        std::string out = std::string(label) + ": ";
        if (Query(name, version, &raw, why) != SCO_OK) {
            if (api_ && api_->size < offsetof(sco_api, query_service) + sizeof(api_->query_service))
                return out + "no services (sco_api 1.0)";
            return out + "not published";
        }
        const bool a = Has(capA);
        const bool b = capB ? Has(capB) : a;
        if (a && b) return out + "ready";
        if (a || b) return out + "partial (" + (a ? capA : capB) + " only)";
        return out + "off";
    };
    std::string s = clause("economy", SC_ECONOMY_SERVICE_NAME, SC_ECONOMY_SERVICE_VERSION, kCapEconomyRead, kCapEconomyApply);
    s += "; " + clause("loadout", SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_SERVICE_VERSION, kCapLoadoutApply, nullptr);
    s += "; " + clause("prefabs", SC_PREFABS_NAME, SC_PREFABS_VERSION_1_0, kCapPrefabsSpawn, nullptr);
    s += "; datacore: " + DataCoreState();
    return s;
}

}  // namespace shop
