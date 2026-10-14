// The game.* bus events (sc_game_events.h): no service table, only sco::Dispatch with the
// payload structs, each gated on its game.events.* capability.
#include "hostd/mock_game/common.h"
#include "sc_game_events.h"
#include "sco/runtime.h"

#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

struct EventCap {
    const char* event;
    const char* capability;
};

constexpr EventCap kEventCaps[] = {
    { SC_GAME_EVENT_PLAYER_SPAWNED, "game.events.player_spawned" },
    { SC_GAME_EVENT_PLAYER_DIED, "game.events.player_died" },
    { SC_GAME_EVENT_ACTOR_DIED, "game.events.actor_died" },
    { SC_GAME_EVENT_ZONE_CHANGED, "game.events.zone_changed" },
    { SC_GAME_EVENT_VEHICLE_BOARDED, "game.events.vehicle_seat" },
    { SC_GAME_EVENT_VEHICLE_EXITED, "game.events.vehicle_seat" },
};

bool Fire(const char* event, const void* data) {
    const char* cap = EventCapability(event);
    if (!cap || !Ready(cap) || !OnGame()) return false;
    return sco::Dispatch(event, data) == sco::Result::Ok;
}

}  // namespace

const char* EventCapability(const char* event) {
    if (!event) return nullptr;
    for (const auto& e : kEventCaps)
        if (std::strcmp(e.event, event) == 0) return e.capability;
    return nullptr;
}

bool SpawnPlayer(uint64_t zone, const Vec3* pos) {
    Scene& s = GetScene();
    Entity* player = FindEntity(s.player);
    if (!player) return false;
    if (zone != 0 && ZoneStreamed(zone)) player->zone = zone;
    if (pos) player->pos = *pos;
    player->health = 100.0f;
    player->actorState = 0;
    s.spawned = true;
    sc_game_player_spawned ev{};
    ev.size = sizeof(ev);
    ev.entity_id = s.player;
    ev.zone_id = player->zone;
    return Fire(SC_GAME_EVENT_PLAYER_SPAWNED, &ev);
}

bool KillActor(uint64_t entityId) {
    Scene& s = GetScene();
    Entity* e = StreamedEntity(entityId);
    if (!e || !e->actor) return false;
    const bool isPlayer = entityId == s.player;
    if (isPlayer && !s.spawned) return false;
    e->health = 0.0f;
    e->actorState = 2;
    const bool wasSeated = Unseat(entityId);
    (void)wasSeated;
    sc_game_actor_died died{};
    died.size = sizeof(died);
    died.entity_id = entityId;
    bool fired = Fire(SC_GAME_EVENT_ACTOR_DIED, &died);
    if (isPlayer) {
        s.spawned = false;
        sc_game_player_died pd{};
        pd.size = sizeof(pd);
        pd.entity_id = entityId;
        fired = Fire(SC_GAME_EVENT_PLAYER_DIED, &pd) || fired;
    }
    return fired;
}

bool KillPlayer() { return KillActor(GetScene().player); }

bool MovePlayer(uint64_t zone, const Vec3& pos, const Quat* rot) {
    Scene& s = GetScene();
    Entity* player = FindEntity(s.player);
    if (!player || !ZoneStreamed(zone)) return false;
    const uint64_t old = player->zone;
    player->zone = zone;
    player->pos = pos;
    if (rot) player->rot = *rot;
    if (old == zone || !s.spawned) return false;
    sc_game_zone_changed ev{};
    ev.size = sizeof(ev);
    ev.old_zone_id = old;
    ev.new_zone_id = zone;
    return Fire(SC_GAME_EVENT_ZONE_CHANGED, &ev);
}

bool FireVehicleSeat(bool boarded, uint64_t ship, int32_t seatIndex) {
    sc_game_vehicle_seat ev{};
    ev.size = sizeof(ev);
    ev.seat_index = seatIndex;
    ev.vehicle_entity_id = ship;
    return Fire(boarded ? SC_GAME_EVENT_VEHICLE_BOARDED : SC_GAME_EVENT_VEHICLE_EXITED, &ev);
}

}  // namespace scuba::hostd::mock_game
