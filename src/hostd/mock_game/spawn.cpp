// spawn.entities 1.2 (sc_spawn.h), the product service name the game pack keeps: spawn near the
// player, your ids, and moving what you spawned with spawn_as.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_spawn.h"

#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "spawn.ship";

const char* SpawnAt(const void* owner, Origin origin, const char* cls, const double offset[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (!OnGame()) return "game thread only";
    if (!Ready(kCap)) return "spawn.ship is unavailable on this game build";
    if (!cls || !offset || !outId || !Finite(offset, 3)) return "bad argument";
    if (!ClassExists(cls)) return "unknown entity class";
    const Scene& s = GetScene();
    const Entity* player = FindEntity(s.player);
    if (!s.spawned || !player) return "you haven't spawned yet";
    Entity e;
    e.cls = cls;
    e.zone = player->zone;
    e.pos = { player->pos[0] + offset[0], player->pos[1] + offset[1], player->pos[2] + offset[2] };
    e.owner = owner;
    e.origin = origin;
    if (std::strncmp(cls, "MOCK_Ship", 9) == 0) {
        e.vehicle = true;
        e.radius = 12.0;
        e.seats = { Seat{ 0, 0, 1000, true, "seat_pilot" } };
    }
    if (std::strncmp(cls, "MockNPC", 7) == 0) {
        e.actor = true;
        e.radius = 0.6;
    }
    *outId = AddEntity(std::move(e));
    return nullptr;
}

const char* SpawnNearPlayer(const char* cls, const double offset[3], uint64_t* outId) {
    return SpawnAt(nullptr, Origin::SpawnNear, cls, offset, outId);
}

int ClassExistsFn(const char* cls) { return OnGame() && Ready(kCap) && ClassExists(cls) ? 1 : 0; }

uint64_t LocalPlayerId() { return OnGame() && Ready(kCap) && GetScene().spawned ? GetScene().player : 0; }

uint64_t PlayerShipId() { return OnGame() && Ready(kCap) && GetScene().spawned ? GetScene().playerShip : 0; }

int EntityAlive(uint64_t id) { return OnGame() && Ready(kCap) && StreamedEntity(id) ? 1 : 0; }

int SetEntityTransform(sco_plugin* self, uint64_t id, uint64_t zone, const double pos[3], const double rot[4]) {
    if (!OnGame() || !Ready(kCap) || !LivePlugin(self) || !pos || !rot || !Finite(pos, 3)) return 0;
    Quat q;
    if (!NormalizeQuat(rot, q)) return 0;
    Entity* e = StreamedEntity(id);
    if (!e || !ZoneKnown(zone)) return 0;
    const bool mine = e->origin == Origin::SpawnAs && e->owner == self && !e->removing;
    if (!mine && !e->playerVehicle) return 0;
    Vec3 local;
    if (!ZoneToZone(zone, e->zone, Vec3{ pos[0], pos[1], pos[2] }, local)) return 0;
    e->pos = local;
    e->rot = q;
    return 1;
}

const char* SpawnAs(sco_plugin* self, const char* cls, const double offset[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (OnGame() && !LivePlugin(self)) return "bad argument";
    return SpawnAt(self, Origin::SpawnAs, cls, offset, outId);
}

void Reset() {}
void Release(const void*) {}

const sc_spawn_service_v1 kTable = { sizeof(sc_spawn_service_v1), SpawnNearPlayer, ClassExistsFn, LocalPlayerId,
                                     PlayerShipId, EntityAlive, SetEntityTransform, SpawnAs };

}  // namespace

const ServiceDef& SpawnService() {
    static const ServiceDef def{ SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
