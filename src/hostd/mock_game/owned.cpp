// game.owned 1.0 (sc_owned.h): retained build placements and owner-scoped removal receipts.
// Receipts stay REQUESTED: initiation is never reported as completion.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_owned.h"

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "game.owned";

struct Ticket {
    const void* owner = nullptr;
    uint64_t entity = 0;
    uint32_t kind = 0;
};

std::map<uint64_t, Ticket> g_tickets;
uint64_t g_nextTicket = 1;

bool Admit(sco_plugin* self, sco_result& r) {
    if (!OnGame()) { r = SCO_WRONG_THREAD; return false; }
    if (!LivePlugin(self)) { r = SCO_BAD_ARG; return false; }
    if (!Ready(kCap)) { r = SCO_UNAVAILABLE; return false; }
    return true;
}

size_t CountRetained() {
    size_t n = 0;
    for (const auto& [id, e] : GetScene().entities)
        if (e.retained) ++n;
    return n;
}

sco_result Issue(sco_plugin* self, Entity& e, uint32_t kind, uint64_t* outTicket) {
    if (g_tickets.size() >= SC_OWNED_MAX_RECEIPTS) return SCO_TOO_MANY;
    BeginRemoval(e);
    const uint64_t t = g_nextTicket++;
    g_tickets[t] = Ticket{ self, e.id, kind };
    *outTicket = t;
    return SCO_OK;
}

sco_result RetainBuild(sco_plugin* self, uint64_t id) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (id == 0) return SCO_BAD_ARG;
    if (!Ready("game.entities.spawn")) return SCO_UNAVAILABLE;
    Entity* e = FindEntity(id);
    if (!e || e->owner != self || e->kept || e->removing ||
        (e->origin != Origin::Entities && e->origin != Origin::Prefab))
        return SCO_NOT_FOUND;
    if (e->retained) return SCO_OK;
    if (CountRetained() >= SC_OWNED_MAX_RETAINED) return SCO_TOO_MANY;
    e->retained = true;
    return SCO_OK;
}

sco_result RemoveBuild(sco_plugin* self, uint64_t id, uint64_t* outTicket) {
    if (outTicket) *outTicket = 0;
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (id == 0 || !outTicket) return SCO_BAD_ARG;
    if (!Ready("game.entities.spawn")) return SCO_UNAVAILABLE;
    Entity* e = FindEntity(id);
    if (!e || !e->retained || e->owner != self || e->removing) return SCO_NOT_FOUND;
    return Issue(self, *e, SC_REMOVAL_BUILD, outTicket);
}

sco_result RemoveNpc(sco_plugin* self, uint64_t id, uint64_t* outTicket) {
    if (outTicket) *outTicket = 0;
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (id == 0 || !outTicket) return SCO_BAD_ARG;
    if (!Ready("game.actors.despawn")) return SCO_UNAVAILABLE;
    Entity* e = FindEntity(id);
    if (!e || e->origin != Origin::Npc || e->owner != self || e->removing) return SCO_NOT_FOUND;
    return Issue(self, *e, SC_REMOVAL_NPC, outTicket);
}

sco_result RemoveSeatNpc(sco_plugin* self, uint64_t ship, uint32_t seat, uint64_t expected, uint64_t* outTicket) {
    if (outTicket) *outTicket = 0;
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (ship == 0 || expected == 0 || !outTicket) return SCO_BAD_ARG;
    if (!Ready("game.actors.despawn") || !Ready("game.vehicles.seats")) return SCO_UNAVAILABLE;
    const Entity* s = StreamedEntity(ship);
    if (!s || !s->vehicle || seat >= s->seats.size()) return SCO_NOT_FOUND;
    Entity* npc = FindEntity(expected);
    if (!npc || npc->origin != Origin::Npc || npc->owner != self || npc->removing) return SCO_NOT_FOUND;
    if (s->seats[seat].occupant != expected) return SCO_FAILED;
    if (g_tickets.size() >= SC_OWNED_MAX_RECEIPTS) return SCO_TOO_MANY;
    Unseat(expected);
    return Issue(self, *npc, SC_REMOVAL_SEAT_NPC, outTicket);
}

sco_result Receipt(sco_plugin* self, uint64_t ticket, sc_removal_receipt* out) {
    if (out && out->size >= sizeof(sc_removal_receipt)) {
        out->state = 0;
        out->ticket = 0;
        out->entity_id = 0;
        out->kind = 0;
        out->_pad = 0;
    }
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (!out || out->size < sizeof(sc_removal_receipt)) return SCO_BAD_ARG;
    if (ticket == 0) return SCO_BAD_ARG;
    const auto it = g_tickets.find(ticket);
    if (it == g_tickets.end() || it->second.owner != self) return SCO_NOT_FOUND;
    out->state = SC_REMOVAL_REQUESTED;
    out->ticket = ticket;
    out->entity_id = it->second.entity;
    out->kind = it->second.kind;
    return SCO_OK;
}

sco_result Forget(sco_plugin* self, uint64_t ticket) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (ticket == 0) return SCO_BAD_ARG;
    const auto it = g_tickets.find(ticket);
    if (it == g_tickets.end() || it->second.owner != self) return SCO_NOT_FOUND;
    g_tickets.erase(it);
    return SCO_OK;
}

void Reset() { g_tickets.clear(); }

void Release(const void* owner) {
    for (auto it = g_tickets.begin(); it != g_tickets.end();)
        it = it->second.owner == owner ? g_tickets.erase(it) : std::next(it);
}

const sc_owned_v1 kTable = { sizeof(sc_owned_v1), 0, RetainBuild, RemoveBuild, RemoveNpc, RemoveSeatNpc, Receipt, Forget };

}  // namespace

const ServiceDef& OwnedService() {
    static const ServiceDef def{ SC_OWNED_NAME, SC_OWNED_VERSION_1_0, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
