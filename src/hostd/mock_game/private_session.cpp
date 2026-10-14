// game.private_session 1.0 (sc_private_session.h): host/join/leave state only. The mock opens no
// socket: HOSTING waits for peers added through hostd, JOINING connects on the next tick.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_private_session.h"

#include <cstring>
#include <deque>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "game.private_session";
constexpr size_t kRateWindowMs = 10000;
constexpr size_t kRateMax = 5;

uint32_t g_state = SC_PRIVATE_SESSION_IDLE;
uint32_t g_peers = 0;
uint32_t g_maxPeers = 0;
const void* g_owner = nullptr;
std::map<const void*, std::deque<uint32_t>> g_requests;

bool Text(const char* s, uint32_t len, uint32_t minLen, uint32_t maxLen) {
    return s && len >= minLen && len <= maxLen && !std::memchr(s, '\0', len);
}

bool ParseIpv4(const char* s, uint32_t len, uint8_t out[4]) {
    uint32_t part = 0, value = 0, digits = 0;
    for (uint32_t i = 0; i <= len; ++i) {
        const char c = i < len ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            value = value * 10u + static_cast<uint32_t>(c - '0');
            if (++digits > 3 || value > 255) return false;
        } else if (c == '.') {
            if (digits == 0 || part > 3) return false;
            out[part++] = static_cast<uint8_t>(value);
            value = digits = 0;
        } else {
            return false;
        }
    }
    return part == 4;
}

bool PrivateAddress(const uint8_t a[4]) {
    return a[0] == 127 || a[0] == 10 || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168);
}

bool RateLimited(const void* self) {
    auto& q = g_requests[self];
    const uint32_t now = GetScene().nowMs;
    while (!q.empty() && now - q.front() > kRateWindowMs) q.pop_front();
    if (q.size() >= kRateMax) return true;
    q.push_back(now);
    return false;
}

bool Admit(sco_plugin* self, sco_result& r) {
    if (!OnGame()) { r = SCO_WRONG_THREAD; return false; }
    if (!LivePlugin(self)) { r = SCO_BAD_ARG; return false; }
    return true;
}

sco_result Gate(sco_plugin* self, uint32_t consent) {
    if (consent != 1u) return SCO_FAILED;
    if (!Ready(kCap)) return SCO_UNAVAILABLE;
    if (g_state != SC_PRIVATE_SESSION_IDLE) return SCO_UNAVAILABLE;
    if (RateLimited(self)) return SCO_TOO_MANY;
    return SCO_OK;
}

sco_result RequestHost(sco_plugin* self, const sc_private_session_host_request_v1* req) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (!req || req->size < sizeof(*req) || req->consent > 1u || req->_pad || !req->port) return SCO_BAD_ARG;
    if (!Text(req->passphrase, req->passphrase_len, SC_PRIVATE_SESSION_MIN_PASSPHRASE, SC_PRIVATE_SESSION_MAX_PASSPHRASE) ||
        !Text(req->player_name, req->player_name_len, 1, SC_PRIVATE_SESSION_MAX_PLAYER_NAME) || req->max_peers < 2 ||
        req->max_peers > SC_PRIVATE_SESSION_MAX_PEERS)
        return SCO_BAD_ARG;
    r = Gate(self, req->consent);
    if (r != SCO_OK) return r;
    g_state = SC_PRIVATE_SESSION_HOSTING;
    g_owner = self;
    g_peers = 0;
    g_maxPeers = req->max_peers;
    return SCO_OK;
}

sco_result RequestJoin(sco_plugin* self, const sc_private_session_join_request_v1* req) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (!req || req->size < sizeof(*req) || req->consent > 1u || req->_pad || !req->port) return SCO_BAD_ARG;
    uint8_t addr[4] = {};
    if (!Text(req->address, req->address_len, 7, SC_PRIVATE_SESSION_MAX_ADDRESS) ||
        !ParseIpv4(req->address, req->address_len, addr) ||
        !Text(req->passphrase, req->passphrase_len, SC_PRIVATE_SESSION_MIN_PASSPHRASE, SC_PRIVATE_SESSION_MAX_PASSPHRASE) ||
        !Text(req->player_name, req->player_name_len, 1, SC_PRIVATE_SESSION_MAX_PLAYER_NAME))
        return SCO_BAD_ARG;
    if (!PrivateAddress(addr)) return SCO_UNAVAILABLE;
    r = Gate(self, req->consent);
    if (r != SCO_OK) return r;
    g_state = SC_PRIVATE_SESSION_JOINING;
    g_owner = self;
    g_peers = 0;
    g_maxPeers = 0;
    return SCO_OK;
}

sco_result Leave(sco_plugin* self) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (g_state == SC_PRIVATE_SESSION_IDLE) return SCO_OK;
    if (g_owner != self) return SCO_UNAVAILABLE;
    g_state = SC_PRIVATE_SESSION_IDLE;
    g_owner = nullptr;
    g_peers = g_maxPeers = 0;
    return SCO_OK;
}

sco_result GetState(sc_private_session_state_v1* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    out->state = g_state;
    out->peer_count = g_peers;
    out->max_peers = g_maxPeers;
    return SCO_OK;
}

void Tick() {
    if (g_state == SC_PRIVATE_SESSION_JOINING) {
        g_state = SC_PRIVATE_SESSION_CONNECTED;
        g_peers = 1;
    }
    if (g_state != SC_PRIVATE_SESSION_IDLE && !Ready(kCap)) {
        g_state = SC_PRIVATE_SESSION_IDLE;
        g_owner = nullptr;
        g_peers = g_maxPeers = 0;
    }
}

void Reset() {
    g_state = SC_PRIVATE_SESSION_IDLE;
    g_owner = nullptr;
    g_peers = g_maxPeers = 0;
    g_requests.clear();
}

void Release(const void* owner) {
    g_requests.erase(owner);
    if (g_owner == owner) {
        g_state = SC_PRIVATE_SESSION_IDLE;
        g_owner = nullptr;
        g_peers = g_maxPeers = 0;
    }
}

const sc_private_session_v1 kTable = { sizeof(sc_private_session_v1), 0, RequestHost, RequestJoin, Leave, GetState };

}  // namespace

bool AddSessionPeer() {
    if (g_state != SC_PRIVATE_SESSION_HOSTING && g_state != SC_PRIVATE_SESSION_CONNECTED) return false;
    if (g_state == SC_PRIVATE_SESSION_HOSTING && g_peers >= g_maxPeers) return false;
    ++g_peers;
    return true;
}

const ServiceDef& PrivateSessionService() {
    static const ServiceDef def{ SC_PRIVATE_SESSION_NAME, SC_PRIVATE_SESSION_VERSION, &kTable, Reset, Release, Tick };
    return def;
}

}  // namespace scuba::hostd::mock_game
