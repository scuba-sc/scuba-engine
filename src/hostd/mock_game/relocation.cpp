// game.relocation 1.0 (sc_relocation.h): one global pending request moved by the tick; COMPLETE
// once the player's pose matches the target, FAILED on timeout or a system change.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_relocation.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "game.relocation";
constexpr size_t kRetained = 32;

struct Request {
    const void* owner = nullptr;
    sc_relocation_status status{};
    uint64_t zone = 0;
    uint64_t root = 0;
    Vec3 pos{};
    Quat rot{};
    uint32_t deadline = 0;
};

std::map<uint64_t, Request> g_requests;
std::deque<uint64_t> g_terminal;
uint64_t g_active = 0;
uint64_t g_nextTicket = 0x7000;

void Finish(Request& r, uint32_t state, uint32_t reason) {
    r.status.state = state;
    r.status.reason = reason;
    const uint64_t ticket = r.status.ticket;
    if (g_active == ticket) g_active = 0;
    g_terminal.push_back(ticket);
    while (g_terminal.size() > kRetained) {
        g_requests.erase(g_terminal.front());
        g_terminal.pop_front();
    }
}

sco_result RequestFn(sco_plugin* self, const sc_relocation_request* req, uint64_t* outTicket) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !req || !outTicket || req->size < sizeof(sc_relocation_request)) return SCO_BAD_ARG;
    Quat q;
    if (req->timeout_ms == 0 || req->timeout_ms > 60000 || req->zone_id == 0 || !Finite(req->pos, 3) ||
        !NormalizeQuat(req->rot_xyzw, q))
        return SCO_BAD_ARG;
    for (double v : req->pos)
        if (std::fabs(v) > 1e12) return SCO_BAD_ARG;
    if (!Ready(kCap)) return SCO_UNAVAILABLE;
    const Scene& s = GetScene();
    const Entity* player = FindEntity(s.player);
    if (!s.spawned || !player) return SCO_FAILED;
    if (!ZoneStreamed(req->zone_id)) return SCO_NOT_FOUND;
    const uint64_t root = RootOf(req->zone_id);
    if (root == 0 || root != RootOf(player->zone)) return SCO_FAILED;
    if (g_active != 0) return SCO_FAILED;
    Request r;
    r.owner = self;
    r.status.size = sizeof(sc_relocation_status);
    r.status.state = SC_RELOCATION_PENDING;
    r.status.ticket = g_nextTicket++;
    r.zone = req->zone_id;
    r.root = root;
    r.pos = { req->pos[0], req->pos[1], req->pos[2] };
    r.rot = q;
    r.deadline = s.nowMs + req->timeout_ms;
    g_active = r.status.ticket;
    *outTicket = r.status.ticket;
    g_requests[r.status.ticket] = r;
    return SCO_OK;
}

sco_result Cancel(sco_plugin* self, uint64_t ticket) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || ticket == 0) return SCO_BAD_ARG;
    const auto it = g_requests.find(ticket);
    if (it == g_requests.end() || it->second.owner != self) return SCO_NOT_FOUND;
    if (it->second.status.state != SC_RELOCATION_PENDING) return SCO_FAILED;
    Finish(it->second, SC_RELOCATION_CANCELLED, SC_RELOCATION_REASON_NONE);
    return SCO_OK;
}

sco_result Status(sco_plugin* self, uint64_t ticket, sc_relocation_status* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || ticket == 0 || !out || out->size < sizeof(sc_relocation_status)) return SCO_BAD_ARG;
    const auto it = g_requests.find(ticket);
    if (it == g_requests.end() || it->second.owner != self) return SCO_NOT_FOUND;
    const uint32_t size = out->size;
    *out = it->second.status;
    out->size = size;
    return SCO_OK;
}

void Tick() {
    if (g_active == 0) return;
    Request& r = g_requests[g_active];
    const Scene& s = GetScene();
    const Entity* player = FindEntity(s.player);
    if (!s.spawned || !player || !Ready(kCap)) {
        Finish(r, SC_RELOCATION_FAILED, SC_RELOCATION_REASON_SESSION);
        return;
    }
    if (RootOf(player->zone) != r.root) {
        Finish(r, SC_RELOCATION_FAILED, SC_RELOCATION_REASON_SYSTEM);
        return;
    }
    if (s.nowMs >= r.deadline) {
        Finish(r, SC_RELOCATION_FAILED, SC_RELOCATION_REASON_TIMEOUT);
        return;
    }
    if (!r.status.applied) {
        r.status.applied = 1;
        const uint64_t zone = r.zone;
        const Vec3 pos = r.pos;
        const Quat rot = r.rot;
        MovePlayer(zone, pos, &rot);
        return;
    }
    const double d = std::sqrt((player->pos[0] - r.pos[0]) * (player->pos[0] - r.pos[0]) +
                               (player->pos[1] - r.pos[1]) * (player->pos[1] - r.pos[1]) +
                               (player->pos[2] - r.pos[2]) * (player->pos[2] - r.pos[2]));
    double dot = 0;
    for (size_t i = 0; i < 4; ++i) dot += player->rot[i] * r.rot[i];
    if (player->zone == r.zone && d <= 0.05 && std::fabs(dot) >= 0.999999)
        Finish(r, SC_RELOCATION_COMPLETE, SC_RELOCATION_REASON_NONE);
}

void Reset() {
    g_requests.clear();
    g_terminal.clear();
    g_active = 0;
}

void Release(const void* owner) {
    for (auto it = g_requests.begin(); it != g_requests.end();) {
        if (it->second.owner != owner) {
            ++it;
            continue;
        }
        if (g_active == it->first) g_active = 0;
        const uint64_t ticket = it->first;
        g_terminal.erase(std::remove(g_terminal.begin(), g_terminal.end(), ticket), g_terminal.end());
        it = g_requests.erase(it);
    }
}

const sc_relocation_v1 kTable = { sizeof(sc_relocation_v1), RequestFn, Cancel, Status };

}  // namespace

const ServiceDef& RelocationService() {
    static const ServiceDef def{ SC_RELOCATION_NAME, SC_RELOCATION_VERSION, &kTable, Reset, Release, Tick };
    return def;
}

}  // namespace scuba::hostd::mock_game
