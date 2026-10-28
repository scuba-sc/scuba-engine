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
        if (!ValidItemName(item.item_class)) return SCO_BAD_ARG;
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

struct Job {
    const void* owner = nullptr;
    std::vector<sc_loadout_item> items;
    sc_loadout_job_report report{};
    uint64_t session = 0;
    uint32_t started = 0;
    bool startedSet = false;
};
std::map<uint64_t, Job> g_jobs;
uint64_t g_nextJob = 1;

sco_result Submit(sco_plugin* self, const sc_loadout_request* req, uint64_t* out) {
    if (out) *out = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || !req || req->size < sizeof(*req)) return SCO_BAD_ARG;
    if (req->count > SC_LOADOUT_MAX_ITEMS) return SCO_TOO_MANY;
    if (!req->count || !req->items || req->item_stride != sizeof(sc_loadout_item)) return SCO_BAD_ARG;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < req->count; ++i)
        if (!ValidItemName(req->items[i].item_class) || !seen.insert(req->items[i].slot).second) return SCO_BAD_ARG;
    if (!Ready("game.loadout.apply")) return SCO_UNAVAILABLE;
    for (const auto& [id, job] : g_jobs) { (void)id; if (job.owner == self) return SCO_TOO_MANY; }
    if (g_jobs.size() >= SC_LOADOUT_MAX_JOBS) return SCO_TOO_MANY;
    Job job; job.owner = self; job.session = GetScene().session;
    job.items.assign(req->items, req->items + req->count);
    job.report.size = sizeof(job.report); job.report.state = SC_LOADOUT_JOB_PENDING;
    job.report.report.size = sizeof(job.report.report); job.report.report.count = req->count;
    for (uint32_t i = 0; i < req->count; ++i)
        job.report.report.slots[i] = { req->items[i].slot, SC_LOADOUT_UNAVAILABLE };
    const uint64_t id = g_nextJob++; g_jobs.emplace(id, std::move(job)); *out = id; return SCO_OK;
}

sco_result Status(sco_plugin* self, uint64_t id, sc_loadout_job_report* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    const auto it = g_jobs.find(id);
    if (it == g_jobs.end() || it->second.owner != self) return SCO_NOT_FOUND;
    const uint32_t size = out->size; *out = it->second.report; out->size = size; return SCO_OK;
}

sco_result Cancel(sco_plugin* self, uint64_t id) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    const auto it = g_jobs.find(id);
    if (it == g_jobs.end() || it->second.owner != self) return SCO_NOT_FOUND;
    g_jobs.erase(it); return SCO_OK;
}

void Tick() {
    for (auto& [id, job] : g_jobs) {
        (void)id;
        if (job.report.state != SC_LOADOUT_JOB_PENDING) continue;
        const Scene& scene = GetScene();
        if (!job.startedSet) { job.started = scene.nowMs; job.startedSet = true; }
        if (!Ready("game.loadout.apply") || scene.session != job.session ||
            static_cast<uint32_t>(scene.nowMs - job.started) >= SC_LOADOUT_TIMEOUT_MS) {
            job.report.state = SC_LOADOUT_JOB_FAILED; continue;
        }
        const uint32_t index = job.report.completed;
        sc_loadout_request req{ sizeof(req), 1, sizeof(sc_loadout_item), 0, &job.items[index] };
        sc_loadout_report report{}; report.size = sizeof(report);
        const sco_result result = Apply(static_cast<sco_plugin*>(const_cast<void*>(job.owner)), &req, &report);
        if (result == SCO_UNAVAILABLE || result == SCO_BAD_ARG || result == SCO_WRONG_THREAD) {
            job.report.state = SC_LOADOUT_JOB_FAILED; continue;
        }
        job.report.report.slots[index] = report.slots[0];
        job.report.report.applied += report.applied;
        if (++job.report.completed == job.items.size()) job.report.state = SC_LOADOUT_JOB_COMPLETE;
    }
}

void Reset() {
    g_jobs.clear();
    g_equipped.fill(std::string());
    g_applying = false;
}
void Release(const void* owner) {
    for (auto it = g_jobs.begin(); it != g_jobs.end();) {
        if (it->second.owner == owner) it = g_jobs.erase(it); else ++it;
    }
}

const sc_loadout_v1_2 kTable = { sizeof(sc_loadout_v1_2), 0, Apply, Submit, Status, Cancel };

}  // namespace

const std::array<std::string, 22>& EquippedItems() { return g_equipped; }

const ServiceDef& LoadoutService() {
    static const ServiceDef def{ SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_VERSION_1_2, &kTable, Reset, Release, Tick };
    return def;
}

}  // namespace scuba::hostd::mock_game
