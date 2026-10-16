#pragma once
// Knobs of the synthetic game.actor_stats and game.actor_stats_write tables (actor_stats.cpp).
// Compiled only when the kit provides sc_actor_stats.h (SCUBA_HAVE_ACTOR_STATS, cmake/ScubaActorStats.cmake).
#include <array>
#include <cstddef>
#include <cstdint>

namespace scuba::hostd::mock_game {

constexpr size_t kActorStatCount = 9;   // sc_actor_stat 1..9

struct ActorStatsValues {
    // Plausible drift on every tick, deterministic from `seed`. Off by default so tests see exact
    // values; SCUBA_ACTOR_STATS_DRIFT=1 in the environment turns it on at reset.
    bool drift = false;
    uint32_t seed = 1;
    // What the host answers to a fresh offline-admission ask before each write. A real session
    // that is not certified offline answers no; the mock defaults to yes.
    bool offlineAdmission = true;
    // The game ignores hunger and thirst changes while their pl_status.enable* switch is off.
    bool hungerThirstEnabled = true;
    // What mode() answers per stat (sc_actor_stat_write_mode, index = stat - 1).
    std::array<uint32_t, kActorStatCount> mode{};
    // The player's actor has no record of this stat (read and write answer SCO_NOT_FOUND).
    std::array<bool, kActorStatCount> missing{};
};
ActorStatsValues& ActorStats();

}  // namespace scuba::hostd::mock_game
