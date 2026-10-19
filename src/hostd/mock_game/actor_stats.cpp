// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include <sc_actor_stats.h>
#include <cmath>
namespace scuba::hostd::mock_game {
namespace {
std::map<uint64_t, std::map<uint32_t, float>> values;
sco_result Read(uint64_t id, uint32_t stat, float* out) {
    if (out) *out = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!out || !id || !stat || stat > SC_ACTOR_STAT_LAST_1_1) return SCO_BAD_ARG;
    if (!Ready("game.actor_stats.read")) return SCO_UNAVAILABLE;
    const auto* entity = FindEntity(id);
    if (!entity || !entity->actor || !entity->streamed || entity->removing) return SCO_NOT_FOUND;
    const auto actor = values.find(id);
    if (actor == values.end()) return SCO_NOT_FOUND;
    const auto value = actor->second.find(stat);
    if (value == actor->second.end()) return SCO_NOT_FOUND;
    if (!std::isfinite(value->second)) return SCO_FAILED;
    *out = value->second;
    return SCO_OK;
}
void Reset() {
    values.clear();
    auto& player = values[GetScene().player];
    for (uint32_t stat = 1; stat <= SC_ACTOR_STAT_LAST_1_1; ++stat) player[stat] = 0;
    player[SC_ACTOR_STAT_BODY_TEMPERATURE] = 37;
    player[SC_ACTOR_STAT_HEALTH_POOL] = 100;
    for (uint32_t stat = SC_ACTOR_STAT_HEALTH_HEAD; stat <= SC_ACTOR_STAT_HEALTH_RIGHT_LEG; ++stat)
        player[stat] = 100;
}
void Release(const void*) {}
const sc_actor_stats_v1 table{sizeof(table), 0, Read};
}
std::map<uint64_t, std::map<uint32_t, float>>& ActorStatValues() { return values; }
const ServiceDef& ActorStatsService() {
    static const ServiceDef def{SC_ACTOR_STATS_NAME, SC_ACTOR_STATS_VERSION_1_1, &table,
        Reset, Release, nullptr};
    return def;
}
}
