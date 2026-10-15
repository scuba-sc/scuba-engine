// game.economy 1.0 (sc_economy.h): one synthetic local balance, once-only keyed transactions per
// owner, and the advisory game.economy.transaction event.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_economy.h"
#include "sco/host.h"
#include "sco/runtime.h"

#include <cstdio>
#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

constexpr int64_t kStartBalance = 25000;

struct Stored {
    sc_economy_request request{};
    sc_economy_receipt receipt{};
};

int64_t g_balance = kStartBalance;
uint64_t g_sequence = 0;
bool g_notifying = false;
std::map<const void*, std::map<uint64_t, Stored>> g_receipts;

bool Admit(sco_plugin* self, const char* cap, sco_result& r) {
    if (!OnGame()) { r = SCO_WRONG_THREAD; return false; }
    if (!LivePlugin(self)) { r = SCO_BAD_ARG; return false; }
    if (g_notifying) { r = SCO_UNAVAILABLE; return false; }
    if (!Ready(cap)) { r = SCO_UNAVAILABLE; return false; }
    return true;
}

sco_result Balance(sco_plugin* self, sc_economy_balance* out) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.economy.read", r)) return r;
    if (!out || out->size < sizeof(sc_economy_balance)) return SCO_BAD_ARG;
    out->local_balance = g_balance;
    return SCO_OK;
}

bool SameRequest(const sc_economy_request& a, const sc_economy_request& b) {
    return a.key == b.key && a.expected_balance == b.expected_balance && a.delta == b.delta && a.flags == b.flags;
}

void CopyReceipt(const sc_economy_receipt& from, sc_economy_receipt* out) {
    const uint32_t size = out->size;
    *out = from;
    out->size = size;
}

sco_result Apply(sco_plugin* self, const sc_economy_request* req, sc_economy_receipt* out) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.economy.apply", r)) return r;
    if (!req || !out || req->size < sizeof(sc_economy_request) || out->size < sizeof(sc_economy_receipt))
        return SCO_BAD_ARG;
    if (req->flags != 0 || req->key == 0 || req->expected_balance < 0 ||
        req->expected_balance >= SC_ECONOMY_BALANCE_LIMIT || req->delta == 0 || req->delta >= SC_ECONOMY_BALANCE_LIMIT ||
        req->delta <= -SC_ECONOMY_BALANCE_LIMIT)
        return SCO_BAD_ARG;
    auto& mine = g_receipts[self];
    const auto seen = mine.find(req->key);
    if (seen != mine.end()) {
        if (!SameRequest(seen->second.request, *req)) return SCO_BAD_ARG;
        CopyReceipt(seen->second.receipt, out);
        return SCO_OK;
    }
    if (req->expected_balance != g_balance) return SCO_FAILED;
    const int64_t after = req->expected_balance + req->delta;
    if (after < 0 || after >= SC_ECONOMY_BALANCE_LIMIT) return SCO_BAD_ARG;
    size_t total = 0;
    for (const auto& [owner, receipts] : g_receipts) total += receipts.size();
    if (total >= SC_ECONOMY_MAX_RECEIPTS) return SCO_TOO_MANY;

    sc_economy_receipt rc{};
    rc.size = sizeof(rc);
    rc.key = req->key;
    rc.session = GetScene().session;
    rc.before = g_balance;
    rc.after = after;
    rc.delta = req->delta;
    rc.sequence = ++g_sequence;
    g_balance = after;
    Stored stored;
    stored.request = *req;
    stored.receipt = rc;
    const uint64_t key = req->key;
    mine[key] = stored;

    sc_economy_event ev{};
    ev.size = sizeof(ev);
    ev.receipt = rc;
    const char* id = sco::host::PluginId(self);
    std::snprintf(ev.plugin_id, sizeof(ev.plugin_id), "%s", id ? id : "");
    g_notifying = true;
    const bool sent = sco::Dispatch(SC_ECONOMY_TRANSACTION_EVENT, &ev) == sco::Result::Ok;
    g_notifying = false;
    if (!sent) rc.flags |= SC_ECONOMY_SIGNAL_DROPPED;

    const auto ownerReceipts = g_receipts.find(self);
    if (ownerReceipts != g_receipts.end()) ownerReceipts->second[key].receipt = rc;
    CopyReceipt(rc, out);
    return SCO_OK;
}

sco_result Receipt(sco_plugin* self, uint64_t key, sc_economy_receipt* out) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.economy.read", r)) return r;
    if (!out || out->size < sizeof(sc_economy_receipt) || key == 0) return SCO_BAD_ARG;
    const auto mine = g_receipts.find(self);
    if (mine == g_receipts.end()) return SCO_NOT_FOUND;
    const auto it = mine->second.find(key);
    if (it == mine->second.end()) return SCO_NOT_FOUND;
    CopyReceipt(it->second.receipt, out);
    return SCO_OK;
}

void Reset() {
    g_balance = kStartBalance;
    g_notifying = false;
    g_receipts.clear();
}

void Release(const void* owner) { g_receipts.erase(owner); }

const sc_economy_v1 kTable = { sizeof(sc_economy_v1), 0, Balance, Apply, Receipt };

}  // namespace

int64_t& EconomyBalance() { return g_balance; }

const ServiceDef& EconomyService() {
    static const ServiceDef def{ SC_ECONOMY_SERVICE_NAME, SC_ECONOMY_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
