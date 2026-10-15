// game.missions 1.0 (sc_missions.h): synthetic offers, owner-scoped once-only accepts, objective
// and phase progress, and declarative script ingestion (each script adds one offer).
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_missions.h"

#include <cstring>
#include <sstream>
#include <vector>

namespace scuba::hostd::mock_game {

namespace {

struct Mission {
    const void* owner = nullptr;
    uint64_t key = 0;
    sc_mission_status status{};
    sc_mission_offer offer{};
};

std::vector<sc_mission_offer> g_offers;
std::map<std::string, std::string> g_scripts;
std::map<uint64_t, Mission> g_missions;
uint64_t g_nextMission = 0x3000;
uint64_t g_generation = 1;

sc_mission_offer MakeOffer(uint64_t id, uint64_t broker, uint32_t phases, uint32_t objectives, int64_t reward) {
    sc_mission_offer o{};
    o.size = sizeof(o);
    o.offer_id = id;
    o.broker_id = broker;
    o.phase_count = phases;
    o.objectives_in_phase = objectives;
    o.local_reward = reward;
    o.generation = g_generation;
    return o;
}

bool Admit(sco_plugin* self, const char* cap, sco_result& r) {
    if (!OnGame()) { r = SCO_WRONG_THREAD; return false; }
    if (!LivePlugin(self)) { r = SCO_BAD_ARG; return false; }
    if (!Ready(cap)) { r = SCO_UNAVAILABLE; return false; }
    return true;
}

Mission* Mine(sco_plugin* self, uint64_t id) {
    const auto it = g_missions.find(id);
    return it == g_missions.end() || it->second.owner != self ? nullptr : &it->second;
}

void CopyStatus(const sc_mission_status& from, sc_mission_status* out) {
    const uint32_t size = out->size;
    *out = from;
    out->size = size;
}

bool SameOffer(const sc_mission_offer& a, const sc_mission_offer& b) {
    return a.flags == b.flags && a.offer_id == b.offer_id && a.broker_id == b.broker_id &&
           a.phase_count == b.phase_count && a.objectives_in_phase == b.objectives_in_phase &&
           a.local_reward == b.local_reward && a.generation == b.generation;
}

sco_result Offer(sco_plugin* self, uint32_t index, sc_mission_offer* out) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.read", r)) return r;
    if (!out || out->size < sizeof(sc_mission_offer) || index >= SC_MISSIONS_MAX_OFFERS) return SCO_BAD_ARG;
    if (index >= g_offers.size()) return SCO_NOT_FOUND;
    const uint32_t size = out->size;
    *out = g_offers[index];
    out->size = size;
    return SCO_OK;
}

sco_result Status(sco_plugin* self, uint64_t id, sc_mission_status* out) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.read", r)) return r;
    if (!out || out->size < sizeof(sc_mission_status) || id == 0) return SCO_BAD_ARG;
    const Mission* m = Mine(self, id);
    if (!m) return SCO_NOT_FOUND;
    CopyStatus(m->status, out);
    return SCO_OK;
}

sco_result Accept(sco_plugin* self, const sc_mission_offer* offer, uint64_t key, uint64_t* outMission) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.accept", r)) return r;
    if (!offer || !outMission || offer->size < sizeof(sc_mission_offer) || key == 0) return SCO_BAD_ARG;
    if (offer->flags || !offer->offer_id || !offer->broker_id || !offer->generation ||
        !offer->phase_count || offer->phase_count > SC_MISSIONS_MAX_STEPS ||
        !offer->objectives_in_phase || offer->objectives_in_phase > SC_MISSIONS_MAX_STEPS ||
        offer->local_reward < 0 || offer->local_reward >= INT64_C(1000000000000)) return SCO_BAD_ARG;
    for (const auto& [id, m] : g_missions) {
        if (m.owner != self || m.key != key) continue;
        if (!SameOffer(m.offer, *offer)) return SCO_BAD_ARG;
        *outMission = id;
        return SCO_OK;
    }
    const sc_mission_offer* current = nullptr;
    for (const auto& o : g_offers)
        if (o.offer_id == offer->offer_id) current = &o;
    if (!current) return SCO_NOT_FOUND;
    if (!SameOffer(*current, *offer)) return SCO_NOT_FOUND;
    size_t mine = 0;
    for (const auto& [id, m] : g_missions)
        if (m.owner == self) ++mine;
    if (mine >= SC_MISSIONS_MAX_ACTIVE) return SCO_TOO_MANY;
    Mission m;
    m.owner = self;
    m.key = key;
    m.offer = *offer;
    m.status.size = sizeof(sc_mission_status);
    m.status.state = SC_MISSION_ACTIVE;
    m.status.mission_id = g_nextMission++;
    m.status.offer_id = current->offer_id;
    m.status.phase_count = current->phase_count;
    m.status.objectives_in_phase = current->objectives_in_phase;
    m.status.session = GetScene().session;
    *outMission = m.status.mission_id;
    g_missions[m.status.mission_id] = m;
    return SCO_OK;
}

sco_result Objective(sco_plugin* self, uint64_t id, uint32_t phase, uint32_t objective) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.progress", r)) return r;
    Mission* m = Mine(self, id);
    if (!m) return SCO_NOT_FOUND;
    sc_mission_status& s = m->status;
    if (objective >= s.objectives_in_phase || phase >= s.phase_count) return SCO_BAD_ARG;
    if (phase < s.phase || (phase == s.phase && objective < s.objective)) return SCO_OK;
    if (s.state != SC_MISSION_ACTIVE) return SCO_FAILED;
    if (phase != s.phase || objective != s.objective) return SCO_BAD_ARG;
    ++s.objective;
    return SCO_OK;
}

sco_result Advance(sco_plugin* self, uint64_t id, uint32_t expectedPhase) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.progress", r)) return r;
    Mission* m = Mine(self, id);
    if (!m) return SCO_NOT_FOUND;
    sc_mission_status& s = m->status;
    if (expectedPhase >= s.phase_count) return SCO_BAD_ARG;
    if (expectedPhase < s.phase || (expectedPhase == s.phase && s.state == SC_MISSION_COMPLETED)) return SCO_OK;
    if (s.state != SC_MISSION_ACTIVE || s.phase != expectedPhase || s.objective < s.objectives_in_phase)
        return SCO_FAILED;
    if (s.phase + 1 == s.phase_count) {
        s.state = SC_MISSION_COMPLETED;
    } else {
        ++s.phase;
        s.objective = 0;
    }
    return SCO_OK;
}

sco_result Abandon(sco_plugin* self, uint64_t id) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.progress", r)) return r;
    Mission* m = Mine(self, id);
    if (!m) return SCO_NOT_FOUND;
    if (m->status.state == SC_MISSION_ABANDONED) return SCO_OK;
    if (m->status.state != SC_MISSION_ACTIVE) return SCO_FAILED;
    m->status.state = SC_MISSION_ABANDONED;
    return SCO_OK;
}

bool ValidScriptPath(const char* path) {
    const void* nul = std::memchr(path, '\0', 128);
    if (!nul) return false;
    const std::string p(path, static_cast<size_t>(static_cast<const char*>(nul) - path));
    const std::string prefix = "missions/", suffix = ".scom";
    if (p.size() <= prefix.size() + suffix.size() || p.compare(0, prefix.size(), prefix) != 0 ||
        p.compare(p.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    for (size_t i = prefix.size(); i < p.size() - suffix.size(); ++i) {
        const char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    const std::string id = p.substr(prefix.size(), p.size() - prefix.size() - suffix.size());
    if (id.size() > 100 || id == "con" || id == "prn" || id == "aux" || id == "nul") return false;
    return !(id.size() == 4 && (id.starts_with("com") || id.starts_with("lpt")) && id[3] >= '1' && id[3] <= '9');
}

sco_result Ingest(sco_plugin* self, const sc_mission_script* script) {
    sco_result r = SCO_OK;
    if (!Admit(self, "game.missions.scripts", r)) return r;
    if (!script || script->size < sizeof(sc_mission_script) || script->flags != 0 || script->_pad != 0) return SCO_BAD_ARG;
    if (!ValidScriptPath(script->relative_path)) return SCO_BAD_ARG;
    if (!script->bytes || script->byte_count == 0 || script->byte_count > SC_MISSIONS_SCRIPT_BYTES) return SCO_BAD_ARG;
    if (std::memchr(script->bytes, '\0', script->byte_count)) return SCO_BAD_ARG;
    const std::string text(script->bytes, script->byte_count);
    std::istringstream input(text);
    std::string magic, phasesKey, objectivesKey;
    uint32_t version = 0, phases = 0, objectives = 0;
    if (!(input >> magic >> version >> phasesKey >> phases >> objectivesKey >> objectives)) return SCO_BAD_ARG;
    if (magic != "sco-mission" || version != 1 || phasesKey != "phases" || objectivesKey != "objectives" ||
        phases == 0 || phases > SC_MISSIONS_MAX_STEPS || objectives == 0 || objectives > SC_MISSIONS_MAX_STEPS)
        return SCO_BAD_ARG;
    const std::string canonical = "sco-mission 1\nphases " + std::to_string(phases) + "\nobjectives " +
                                  std::to_string(objectives) + "\n";
    if (text != canonical) return SCO_BAD_ARG;
    const auto prior = g_scripts.find(script->relative_path);
    if (prior != g_scripts.end()) return prior->second == text ? SCO_OK : SCO_FAILED;
    if (g_scripts.size() >= SC_MISSIONS_MAX_SCRIPTS || g_offers.size() >= SC_MISSIONS_MAX_OFFERS) return SCO_TOO_MANY;
    g_scripts[script->relative_path] = text;
    ++g_generation;
    for (auto& o : g_offers) o.generation = g_generation;
    g_offers.push_back(MakeOffer(0x2100 + g_scripts.size(), 0x2001, phases, objectives, 0));
    return SCO_OK;
}

void Reset() {
    g_generation = 1;
    g_offers = { MakeOffer(0x2010, 0x2001, 2, 2, 5000), MakeOffer(0x2011, 0x2001, 1, 3, 1200),
                 MakeOffer(0x2012, 0x2002, 3, 1, 9000) };
    g_scripts.clear();
    g_missions.clear();
    g_nextMission = 0x3000;
}

void Release(const void* owner) {
    for (auto it = g_missions.begin(); it != g_missions.end();)
        it = it->second.owner == owner ? g_missions.erase(it) : std::next(it);
}

const sc_missions_v1 kTable = { sizeof(sc_missions_v1), 0, Offer, Status, Accept, Objective, Advance, Abandon, Ingest };

}  // namespace

const ServiceDef& MissionsService() {
    static const ServiceDef def{ SC_MISSIONS_SERVICE_NAME, SC_MISSIONS_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
