// game.vehicles 1.0 (sc_vehicles.h): the ship you're aboard, seats and occupants, seating and
// ejecting your own actors, and Flight Ready.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_vehicles.h"

#include <cstdio>
#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kSeats = "game.vehicles.seats";
constexpr const char* kSeat = "game.vehicles.seat";
constexpr const char* kFlight = "game.vehicles.flight_ready";
ErrorBook g_err;

sco_result Unready(const void* who, const char* cap) {
    return g_err.Fail(who, SCO_UNAVAILABLE, std::string(cap) + " isn't ready");
}

const Entity* Ship(uint64_t id) {
    const Entity* e = StreamedEntity(id);
    return e && e->vehicle ? e : nullptr;
}

bool MayControlActor(uint64_t actor, const void* self) {
    if (actor == GetScene().player) return true;
    const Entity* e = FindEntity(actor);
    return e && e->actor && e->owner == self && !e->removing &&
           (e->origin == Origin::Npc || e->origin == Origin::SpawnAs);
}

sco_result PlayerShip(uint64_t* outShip) {
    if (outShip) *outShip = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kSeats)) return Unready(nullptr, kSeats);
    if (!GetScene().spawned) return g_err.Fail(nullptr, SCO_UNAVAILABLE, "you haven't spawned yet");
    if (!outShip) return g_err.Fail(nullptr, SCO_BAD_ARG, "out_ship is NULL");
    if (GetScene().playerShip == 0) return g_err.Fail(nullptr, SCO_NOT_FOUND, "you're in no ship");
    *outShip = GetScene().playerShip;
    return SCO_OK;
}

sco_result Seats(uint64_t ship, sc_vehicle_seat* out, uint32_t max, uint32_t* outCount, uint32_t* outMore) {
    if (outCount) *outCount = 0;
    if (outMore) *outMore = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kSeats)) return Unready(nullptr, kSeats);
    if (!outCount || !outMore || (!out && max > 0)) return g_err.Fail(nullptr, SCO_BAD_ARG, "a null output");
    const Entity* s = Ship(ship);
    if (!s) return g_err.Fail(nullptr, SCO_NOT_FOUND, "the ship isn't streamed in or isn't a vehicle");
    uint32_t n = 0;
    for (size_t i = 0; i < s->seats.size() && n < max; ++i, ++n) {
        const Seat& seat = s->seats[i];
        sc_vehicle_seat& o = out[n];
        std::memset(&o, 0, sizeof(o));
        o.index = n;
        o.flags = (Ready(kSeat) ? SC_SEAT_USABLE_KNOWN | (seat.usable ? SC_SEAT_USABLE : 0u) : 0u) |
                  (seat.occupant ? SC_SEAT_OCCUPIED : 0u) |
                  (seat.priority >= 1000 ? SC_SEAT_PILOT : 0u);
        o.seat_id = seat.seatId;
        o.occupant_id = seat.occupant;
        o.priority = seat.priority;
        std::snprintf(o.name, sizeof(o.name), "%s", seat.name.c_str());
    }
    *outCount = n;
    *outMore = s->seats.size() > n ? 1u : 0u;
    return SCO_OK;
}

sco_result SeatOccupant(uint64_t ship, uint32_t index, uint64_t* outActor) {
    if (outActor) *outActor = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kSeats)) return Unready(nullptr, kSeats);
    if (!outActor) return g_err.Fail(nullptr, SCO_BAD_ARG, "out_actor is NULL");
    const Entity* s = Ship(ship);
    if (!s || index >= s->seats.size()) return g_err.Fail(nullptr, SCO_NOT_FOUND, "no such ship or seat");
    *outActor = s->seats[index].occupant;
    return SCO_OK;
}

sco_result SeatFn(sco_plugin* self, uint64_t actor, uint64_t ship, uint32_t index) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kSeat)) return Unready(self, kSeat);
    if (!MayControlActor(actor, self)) return g_err.Fail(self, SCO_BAD_ARG, "not your player or your own actor");
    Entity* s = FindEntity(ship);
    if (!s || !s->streamed || !s->vehicle || index >= s->seats.size())
        return g_err.Fail(self, SCO_NOT_FOUND, "no such ship or seat");
    Seat& seat = s->seats[index];
    if (!seat.usable) return g_err.Fail(self, SCO_FAILED, "the seat isn't usable");
    if (seat.occupant != 0 && seat.occupant != actor) return g_err.Fail(self, SCO_FAILED, "someone else is in it");
    const Entity* a = StreamedEntity(actor);
    if (!a || (actor == GetScene().player && !GetScene().spawned))
        return g_err.Fail(self, SCO_FAILED, "the actor isn't streamed in yet");
    Unseat(actor);
    seat.occupant = actor;
    if (actor == GetScene().player) GetScene().playerShip = ship;
    FireVehicleSeat(true, ship, static_cast<int32_t>(index));
    return SCO_OK;
}

sco_result Eject(sco_plugin* self, uint64_t actor) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kSeat)) return Unready(self, kSeat);
    if (!MayControlActor(actor, self)) return g_err.Fail(self, SCO_BAD_ARG, "not your player or your own actor");
    for (auto& [id, e] : GetScene().entities) {
        for (size_t i = 0; i < e.seats.size(); ++i) {
            if (e.seats[i].occupant != actor) continue;
            const uint64_t shipId = id;
            Unseat(actor);
            FireVehicleSeat(false, shipId, static_cast<int32_t>(i));
            return SCO_OK;
        }
    }
    return g_err.Fail(self, SCO_FAILED, "the actor isn't seated");
}

sco_result PowerOn(sco_plugin* self, uint64_t ship) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kFlight)) return Unready(self, kFlight);
    Entity* s = FindEntity(ship);
    const bool allowed = s && s->vehicle &&
                         (ship == GetScene().playerShip || s->playerVehicle ||
                          (s->owner == self && s->origin == Origin::SpawnAs));
    if (!allowed) return g_err.Fail(self, SCO_BAD_ARG, "not your ship to power on");
    if (!s->streamed || !s->dashboard) return g_err.Fail(self, SCO_FAILED, "the ship has no dashboard streamed in yet");
    s->poweredOn = true;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

void Reset() { g_err.Reset(); }
void Release(const void* owner) { g_err.Forget(owner); }

const sc_vehicles_v1 kTable = { sizeof(sc_vehicles_v1), 0, PlayerShip, Seats, SeatOccupant, SeatFn, Eject, PowerOn, LastError };

}  // namespace

const ServiceDef& VehiclesService() {
    static const ServiceDef def{ SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
