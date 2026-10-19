// game.loadout 1.1 (sc_loadout.h): typed local-player equip of the scene's synthetic items. Slots
// 8-21 (weapons and multitool attachments, new in 1.1) also need game.loadout.weapons.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_loadout.h"

#include <array>
#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

std::array<std::string, 22> g_equipped;
bool g_applying = false;
struct Job {
    sco_plugin* owner = nullptr;
    uint64_t id = 0, session = 0;
    uint32_t since = 0;
    sc_loadout_item items[SC_LOADOUT_MAX_ITEMS]{};
    sc_loadout_job_report result{};
};
std::array<Job, SC_LOADOUT_MAX_JOBS> g_jobs;
uint64_t g_nextJob = 0;

bool ValidItemName(const char* name) {
    const void* nul = std::memchr(name, '\0', SC_LOADOUT_ITEM_NAME_BYTES);
    if (!nul) return false;
    const size_t n = static_cast<size_t>(static_cast<const char*>(nul) - name);
    if (n == 0) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

sco_result Apply(sco_plugin* self, const sc_loadout_request* req, sc_loadout_report* report) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (!req || !report || req->size < sizeof(sc_loadout_request) || report->size < sizeof(sc_loadout_report))
        return SCO_BAD_ARG;
    if (g_applying) return SCO_UNAVAILABLE;
    if (req->count > SC_LOADOUT_MAX_ITEMS) return SCO_TOO_MANY;
    if (req->count == 0 || !req->items || req->item_stride != sizeof(sc_loadout_item)) return SCO_BAD_ARG;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < req->count; ++i) {
        const sc_loadout_item& item = req->items[i];
        if (item._pad || !ValidItemName(item.item_class) || req->_pad) return SCO_BAD_ARG;
        if (!seen.insert(item.slot).second) return SCO_BAD_ARG;
    }
    const uint32_t size = report->size;
    std::memset(report, 0, sizeof(sc_loadout_report));
    report->size = size;
    report->count = req->count;
    if (!Ready("game.loadout.apply")) {
        for (uint32_t i = 0; i < req->count; ++i)
            report->slots[i] = sc_loadout_slot_result{ req->items[i].slot, SC_LOADOUT_UNAVAILABLE };
        return SCO_UNAVAILABLE;
    }
    g_applying = true;
    const Scene& s = GetScene();
    uint32_t applied = 0;
    for (uint32_t i = 0; i < req->count; ++i) {
        const sc_loadout_item& item = req->items[i];
        sc_loadout_status status = SC_LOADOUT_APPLIED;
        if (item.slot < SC_LOADOUT_SLOT_HEAD || item.slot > SC_LOADOUT_SLOT_SIDEARM_TOOL_BATTERY)
            status = SC_LOADOUT_UNSUPPORTED;
        else if (item.slot >= SC_LOADOUT_SLOT_PRIMARY && !Ready("game.loadout.weapons"))
            status = SC_LOADOUT_UNAVAILABLE;  // the kit's rule: no engine call, the slot stays UNAVAILABLE
        else if (!s.items.count(item.item_class))
            status = SC_LOADOUT_ITEM_MISSING;
        else if (!s.spawned)
            status = SC_LOADOUT_FAILED;
        if (status == SC_LOADOUT_APPLIED) {
            g_equipped[item.slot] = item.item_class;
            ++applied;
        }
        report->slots[i] = sc_loadout_slot_result{ item.slot, status };
    }
    g_applying = false;
    report->applied = applied;
    return applied == req->count ? SCO_OK : SCO_FAILED;
}

sco_result Submit(sco_plugin* self, const sc_loadout_request* req, uint64_t* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || !req || req->size < sizeof(*req) || req->_pad ||
        !req->count || !req->items || req->item_stride != sizeof(sc_loadout_item)) return SCO_BAD_ARG;
    if (req->count > SC_LOADOUT_MAX_ITEMS) return SCO_TOO_MANY;
    for (uint32_t i = 0; i < req->count; ++i) {
        if (req->items[i]._pad || !ValidItemName(req->items[i].item_class)) return SCO_BAD_ARG;
        for (uint32_t j = 0; j < i; ++j) if (req->items[i].slot == req->items[j].slot) return SCO_BAD_ARG;
    }
    if (!Ready("game.loadout.async") || !GetScene().spawned) return SCO_UNAVAILABLE;
    Job* free = nullptr;
    for (auto& job : g_jobs) {
        if (job.owner == self) return SCO_TOO_MANY;
        if (!job.owner && !free) free = &job;
    }
    if (!free || g_nextJob == UINT64_MAX) return SCO_TOO_MANY;
    *free = {}; free->owner = self; free->id = ++g_nextJob; free->session = GetScene().session;
    free->since = GetScene().nowMs;
    free->result.size = sizeof(free->result); free->result.report.size = sizeof(sc_loadout_report);
    free->result.report.count = req->count;
    for (uint32_t i = 0; i < req->count; ++i) {
        free->items[i] = req->items[i];
        free->result.report.slots[i] = {req->items[i].slot, SC_LOADOUT_UNAVAILABLE};
    }
    *out = free->id; return SCO_OK;
}
sco_result Status(sco_plugin* self, uint64_t id, sc_loadout_job_report* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !id || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    for (const auto& job : g_jobs) if (job.owner == self && job.id == id) { *out = job.result; return SCO_OK; }
    return SCO_NOT_FOUND;
}
sco_result Cancel(sco_plugin* self, uint64_t id) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !id) return SCO_BAD_ARG;
    for (auto& job : g_jobs) if (job.owner == self && job.id == id) { job = {}; return SCO_OK; }
    return SCO_NOT_FOUND;
}
void Tick() {
    for (auto& job : g_jobs) {
        if (!job.owner || job.result.state != SC_LOADOUT_JOB_PENDING) continue;
        if (!Ready("game.loadout.async") || job.session != GetScene().session || !GetScene().spawned ||
            static_cast<uint32_t>(GetScene().nowMs - job.since) >= SC_LOADOUT_TIMEOUT_MS) {
            job.result.state = SC_LOADOUT_JOB_FAILED; continue;
        }
        const uint32_t index = job.result.completed;
        const auto& item = job.items[index];
        auto& status = job.result.report.slots[index].status;
        status = item.slot < SC_LOADOUT_SLOT_HEAD || item.slot > SC_LOADOUT_SLOT_SIDEARM_TOOL_BATTERY
            ? SC_LOADOUT_UNSUPPORTED : !GetScene().items.count(item.item_class)
            ? SC_LOADOUT_ITEM_MISSING : item.slot >= SC_LOADOUT_SLOT_PRIMARY && !Ready("game.loadout.weapons")
            ? SC_LOADOUT_UNAVAILABLE : SC_LOADOUT_APPLIED;
        if (status == SC_LOADOUT_APPLIED) { g_equipped[item.slot] = item.item_class; ++job.result.report.applied; }
        if (++job.result.completed == job.result.report.count) job.result.state = SC_LOADOUT_JOB_COMPLETE;
    }
}
void Reset() {
    g_equipped.fill(std::string());
    g_jobs = {};
    g_applying = false;
}
void Release(const void* owner) { for (auto& job : g_jobs) if (job.owner == owner) job = {}; }

const sc_loadout_v1_2 kTable = { sizeof(kTable), 0, Apply, Submit, Status, Cancel };

}  // namespace

const std::array<std::string, 22>& EquippedItems() { return g_equipped; }

const ServiceDef& LoadoutService() {
    static const ServiceDef def{ SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_VERSION_1_2, &kTable, Reset, Release, Tick };
    return def;
}

}  // namespace scuba::hostd::mock_game
