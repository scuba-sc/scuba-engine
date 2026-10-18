// game.actor_stats 1.0 (sc_actor_stats.h) and game.actor_stats_write 1.0 (sc_actor_stats_write.h)
// over the mock scene. Every value is synthetic: the real units, ranges and directions are not
// known (nothing here has been run in game), so these are plausible numbers, not game values.
#include "hostd/mock_game/actor_stats.h"

#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_actor_stats_write.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kReadCap = "game.actor_stats.read";
constexpr const char* kWriteCap = "game.actor_stats.write";

struct Range { float lo, hi, initial; };
// Synthetic, by sc_actor_stat - 1: hunger thirst body_temp suit_temp stun distortion oxygen body_rad suit_rad.
constexpr Range kRanges[kActorStatCount] = {
    { 0.0f, 100.0f, 20.0f },   { 0.0f, 100.0f, 15.0f },  { 20.0f, 45.0f, 37.0f },
    { -50.0f, 80.0f, 22.0f },  { 0.0f, 100.0f, 0.0f },   { 0.0f, 100.0f, 0.0f },
    { 0.0f, 100.0f, 100.0f },  { 0.0f, 100.0f, 0.0f },   { 0.0f, 100.0f, 0.0f },
};

ActorStatsValues g_values;
std::map<uint64_t, std::array<float, kActorStatCount>> g_stats;   // by entity id, seeded on first use
uint32_t g_rng = 1;

float Next() {   // deterministic, in [-1, 1)
    g_rng = g_rng * 1664525u + 1013904223u;
    return static_cast<float>(g_rng >> 8) / static_cast<float>(1u << 23) - 1.0f;
}

std::array<float, kActorStatCount>& StatsOf(uint64_t id) {
    auto it = g_stats.find(id);
    if (it == g_stats.end()) {
        std::array<float, kActorStatCount> fresh{};
        for (size_t i = 0; i < kActorStatCount; ++i) fresh[i] = kRanges[i].initial;
        it = g_stats.emplace(id, fresh).first;
    }
    return it->second;
}

bool KnownStat(uint32_t stat) { return stat >= SC_ACTOR_STAT_HUNGER && stat <= SC_ACTOR_STAT_LAST_1_0; }

// ---- game.actor_stats ---------------------------------------------------------------------------

sco_result Read(uint64_t id, uint32_t stat, float* out) {
    if (out) *out = 0.0f;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!Ready(kReadCap)) return SCO_UNAVAILABLE;
    if (id == 0 || !out || !KnownStat(stat)) return SCO_BAD_ARG;
    const Entity* e = StreamedEntity(id);
    if (!e || !e->actor) return SCO_NOT_FOUND;
    if (id == GetScene().player && g_values.missing[stat - 1]) return SCO_NOT_FOUND;
    *out = StatsOf(id)[stat - 1];
    return SCO_OK;
}

const sc_actor_stats_v1 kReadTable = { sizeof(sc_actor_stats_v1), 0, Read };

// ---- game.actor_stats_write ---------------------------------------------------------------------

// The checks every write route shares, in the contract's order. On SCO_OK *index is the stat's slot.
sco_result Enter(sco_plugin* self, uint64_t id, uint32_t stat, size_t* index) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || id == 0 || !KnownStat(stat) || id != GetScene().player) return SCO_BAD_ARG;
    if (!Ready(kWriteCap) || !Ready(kReadCap)) return SCO_UNAVAILABLE;
    if (!GetScene().spawned) return SCO_NOT_FOUND;
    if (g_values.missing[stat - 1]) return SCO_NOT_FOUND;
    *index = stat - 1;
    return SCO_OK;
}

sco_result Mode(sco_plugin* self, uint64_t id, uint32_t stat, uint32_t* outMode) {
    if (outMode) *outMode = SC_ACTOR_STAT_WRITE_DISCARDED;
    size_t i = 0;
    if (!outMode) return SCO_BAD_ARG;
    const sco_result r = Enter(self, id, stat, &i);
    if (r != SCO_OK) return r;
    *outMode = g_values.mode[i];   // reads only: no offline admission
    return SCO_OK;
}

// adjust (delta) and set (target) after the shared checks.
sco_result Apply(sco_plugin* self, uint64_t id, uint32_t stat, float delta, float* outAfter, bool isSet) {
    if (outAfter) *outAfter = 0.0f;
    if (!OnGame()) return SCO_WRONG_THREAD;
    size_t i = 0;
    const sco_result r = Enter(self, id, stat, &i);
    if (r != SCO_OK) return r;
    auto& values = StatsOf(id);
    if (!std::isfinite(delta)) return SCO_BAD_ARG;
    if (isSet) delta = delta - values[i];   // `delta` carried the target
    if (!std::isfinite(delta) || std::fabs(delta) > SC_ACTOR_STATS_WRITE_MAX_STEP) return SCO_BAD_ARG;
    // Offline admission is asked fresh before every write, ahead of anything that touches a stat.
    if (!g_values.offlineAdmission) return SCO_UNAVAILABLE;
    // A write the game would discard is refused, never a silent OK.
    if (g_values.mode[i] == SC_ACTOR_STAT_WRITE_DISCARDED) return SCO_UNAVAILABLE;
    const bool ignored = !g_values.hungerThirstEnabled && (stat == SC_ACTOR_STAT_HUNGER || stat == SC_ACTOR_STAT_THIRST);
    // A step under FLT_EPSILON is not passed on: the game would log it and change nothing.
    if (!ignored && std::fabs(delta) >= FLT_EPSILON)
        values[i] = std::clamp(values[i] + delta, kRanges[i].lo, kRanges[i].hi);
    if (outAfter) *outAfter = values[i];   // the fresh read-back: what took effect
    return SCO_OK;
}

sco_result Adjust(sco_plugin* self, uint64_t id, uint32_t stat, float delta, float* outAfter) {
    return Apply(self, id, stat, delta, outAfter, false);
}

sco_result Set(sco_plugin* self, uint64_t id, uint32_t stat, float value, float* outAfter) {
    return Apply(self, id, stat, value, outAfter, true);
}

const sc_actor_stats_write_v1 kWriteTable = { sizeof(sc_actor_stats_write_v1), 0, Adjust, Set, Mode };

// ---- lifecycle ----------------------------------------------------------------------------------

void Reset() {
    g_stats.clear();
    g_values = ActorStatsValues{};
    g_values.mode.fill(SC_ACTOR_STAT_WRITE_AUTHORITATIVE);
    const char* env = std::getenv("SCUBA_ACTOR_STATS_DRIFT");
    g_values.drift = env && *env == '1';
    g_rng = g_values.seed;
}

void Tick() {
    if (!g_values.drift) return;
    // Slow, bounded wander on the player's stats: hunger and thirst creep up, temperatures and
    // oxygen jitter, the rest hold. Only so a log has something to show; not a model of the game.
    auto& v = StatsOf(GetScene().player);
    const auto bump = [&v](size_t i, float step) { v[i] = std::clamp(v[i] + step, kRanges[i].lo, kRanges[i].hi); };
    bump(0, 0.05f);
    bump(1, 0.08f);
    bump(2, 0.05f * Next());
    bump(3, 0.2f * Next());
    bump(6, 0.1f * Next());
}

void NoReset() {}
void Release(const void*) {}   // nothing is restored on unload (docs/game/actor-stats.md)

}  // namespace

ActorStatsValues& ActorStats() { return g_values; }

const ServiceDef& ActorStatsService() {
    static const ServiceDef def{ SC_ACTOR_STATS_NAME, SC_ACTOR_STATS_VERSION_1_0, &kReadTable, Reset, Release, Tick };
    return def;
}

const ServiceDef& ActorStatsWriteService() {
    static const ServiceDef def{ SC_ACTOR_STATS_WRITE_NAME, SC_ACTOR_STATS_WRITE_VERSION_1_0, &kWriteTable,
                                 NoReset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
