// SPDX-License-Identifier: GPL-3.0-only
// Copied SDK2.4 observations over synthetic state, never live game memory.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_actor_stats.h"
#include "sc_inventory.h"
#include "sc_gamerules.h"
#include "sc_loadout.h"
#include <algorithm>
#include <cstring>

namespace scuba::hostd::mock_game {
namespace {
sco_result Stat(uint64_t id, uint32_t stat, float* out) {
    if (out) *out = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!id || !out || stat < SC_ACTOR_STAT_HUNGER || stat > SC_ACTOR_STAT_LAST_1_0) return SCO_BAD_ARG;
    if (!Ready("game.actor_stats.read")) return SCO_UNAVAILABLE;
    const auto* entity = FindEntity(id);
    if (!entity || !entity->actor || !entity->streamed) return SCO_NOT_FOUND;
    *out = stat == SC_ACTOR_STAT_BODY_TEMPERATURE ? 37.0f : stat == SC_ACTOR_STAT_OXYGEN ? 100.0f : 0.0f;
    return SCO_OK;
}
sco_result Inventory(sco_plugin* self, sc_inventory_snapshot* out) {
    if (!OnGame()) return SCO_UNAVAILABLE;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (!out || out->size < SC_INVENTORY_SNAPSHOT_SIZE_1_0 || !Ready("game.inventory.read") || !GetScene().spawned)
        return SCO_UNAVAILABLE;
    const uint32_t size = out->size, capacity = std::min(out->count, SC_INVENTORY_MAX_PORTS);
    sc_inventory_snapshot next{};
    next.size = size;
    next.generation = GetScene().tick + 1;
    const auto& equipped = EquippedItems();
    for (uint32_t slot = 1; slot < equipped.size(); ++slot) {
        if (equipped[slot].empty()) continue;
        if (next.count < capacity) {
            auto& port = next.ports[next.count++];
            port.size = sizeof(port); port.valid = 1;
            port.item_class_id = slot < SC_LOADOUT_SLOT_PRIMARY ? SC_ITEM_TYPE_ARMOR : SC_ITEM_TYPE_WEAPON_ATTACHMENT;
            port.entity_id = 0x70000u + slot;
        }
        ++next.total_seen;
    }
    next.flags = next.total_seen > next.count ? SC_INVENTORY_FLAG_TRUNCATED : 0;
    out->count = next.count;
    out->generation = next.generation;
    std::memcpy(out->ports, next.ports, next.count * sizeof(sc_inventory_port));
    if (size >= sizeof(next)) {
        out->flags = next.flags;
        out->total_seen = next.total_seen;
    }
    return SCO_OK;
}
sco_result Rules(sco_plugin* self, sc_gamerules_state* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    if (!Ready("game.gamerules.read")) return SCO_UNAVAILABLE;
    sc_gamerules_state next{}; next.size = out->size; next.valid = 1;
    next.generation = GetScene().session;
    next.quantum_travel_allowed = 1; next.quantum_boost_allowed = 1;
    *out = next; return SCO_OK;
}
void Reset() {}
void Release(const void*) {}
constexpr sc_gamerules_param_info parameters[] = {
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "capturableDebug", "control"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "phaseDebug", "control"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "objectiveDebug", "control"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_FLOAT, "terminalsDisabledTime", "control"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "log", "state"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "enabled", "scoring"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "log", "scoring"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "log", "damagehandling"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "disableCollisionFilter", "damagehandling"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "QTEMode", "accessibility"},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "showRaceCheckpoints", ""},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "showRaceInfo", ""},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_FLOAT, "overrideRespawnTimer", ""},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "editorUseSpawnpoints", ""},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "allowNonLocationSpawnpoints", ""},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "despawnOnProjectileHit", ""},
    {sizeof(sc_gamerules_param_info), SC_GAMERULES_PARAM_INT, "allowAllPlayerCameras", ""},
};
sco_result Params(sco_plugin* self, uint32_t offset, sc_gamerules_param_info* out,
                  uint32_t stride, uint32_t cap, uint32_t* count, uint32_t* total) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !count || !total || count == total || offset > std::size(parameters) ||
        cap > SC_GAMERULES_PARAM_MAX_PAGE || (!cap && out) ||
        (cap && (!out || stride < sizeof(*out) || stride > SC_GAMERULES_PARAM_MAX_STRIDE))) return SCO_BAD_ARG;
    const auto n = std::min(cap, static_cast<uint32_t>(std::size(parameters)) - offset);
    for (uint32_t i = 0; i < cap; ++i) {
        uint32_t size = 0;
        std::memcpy(&size, reinterpret_cast<unsigned char*>(out) + static_cast<size_t>(i) * stride, sizeof(size));
        if (size < sizeof(*out)) return SCO_BAD_ARG;
    }
    for (uint32_t i = 0; i < n; ++i)
        std::memcpy(reinterpret_cast<unsigned char*>(out) + static_cast<size_t>(i) * stride + sizeof(uint32_t),
                    reinterpret_cast<const unsigned char*>(&parameters[offset + i]) + sizeof(uint32_t), sizeof(*out) - sizeof(uint32_t));
    *count = n; *total = static_cast<uint32_t>(std::size(parameters)); return SCO_OK;
}
const sc_actor_stats_v1 stats{sizeof(stats), 0, Stat};
const sc_inventory_v1 inventory{sizeof(inventory), 0, Inventory};
const sc_gamerules_v1 rules{sizeof(rules), 0, Rules, Params};
}
const ServiceDef& ActorStatsService() {
    static const ServiceDef def{SC_ACTOR_STATS_NAME, SC_ACTOR_STATS_VERSION_1_0, &stats, Reset, Release, nullptr}; return def;
}
const ServiceDef& InventoryService() {
    static const ServiceDef def{SC_INVENTORY_SERVICE_NAME, SC_INVENTORY_SERVICE_VERSION_1_1, &inventory, Reset, Release, nullptr}; return def;
}
const ServiceDef& GameRulesService() {
    static const ServiceDef def{SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_1, &rules, Reset, Release, nullptr}; return def;
}
}
