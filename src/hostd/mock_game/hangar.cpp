// game.hangar 1.0 (sc_hangar.h): lifts found from their landing area, lift open/close requests
// and the local player's ATC request cancel. Nothing here is owned.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_hangar.h"

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kLift = "game.hangar.lift";
constexpr const char* kAtc = "game.hangar.atc_cancel";
ErrorBook g_err;

bool Admit(sco_plugin* self, const char* cap, sco_result& r) {
    if (!OnGame()) { r = WrongThread(g_err); return false; }
    if (!LivePlugin(self)) { r = g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle"); return false; }
    if (!Ready(cap)) { r = g_err.Fail(self, SCO_UNAVAILABLE, std::string(cap) + " isn't ready"); return false; }
    return true;
}

sco_result LiftOfLandingArea(sco_plugin* self, uint64_t area, uint64_t* outLift) {
    if (outLift) *outLift = 0;
    sco_result r = SCO_OK;
    if (!Admit(self, kLift, r)) return r;
    if (area == 0 || !outLift) return g_err.Fail(self, SCO_BAD_ARG, "an id of 0, or out_lift_id NULL");
    const Entity* a = StreamedEntity(area);
    if (!a) return g_err.Fail(self, SCO_NOT_FOUND, "the landing area isn't streamed in");
    if (!a->landingArea) return g_err.Fail(self, SCO_BAD_ARG, "the entity isn't a landing area");
    for (const auto& [id, e] : GetScene().entities) {
        if (e.lift && e.streamed && e.liftOfArea == area && e.zone == a->zone) {
            *outLift = id;
            return SCO_OK;
        }
    }
    return g_err.Fail(self, SCO_NOT_FOUND, "no lift goes with that landing area");
}

sco_result LiftRequest(sco_plugin* self, uint64_t liftId, uint32_t op) {
    sco_result r = SCO_OK;
    if (!Admit(self, kLift, r)) return r;
    if (liftId == 0 || (op != SC_HANGAR_LIFT_OPEN && op != SC_HANGAR_LIFT_CLOSE))
        return g_err.Fail(self, SCO_BAD_ARG, "an id of 0 or an unknown op");
    Entity* e = FindEntity(liftId);
    if (e && e->streamed && !e->lift) return g_err.Fail(self, SCO_BAD_ARG, "the entity isn't a ship lift");
    if (!e || !e->streamed) return g_err.Fail(self, SCO_NOT_FOUND, "the lift isn't streamed in");
    e->liftState = op;
    return SCO_OK;
}

sco_result AtcCancel(sco_plugin* self, uint64_t atcId) {
    sco_result r = SCO_OK;
    if (!Admit(self, kAtc, r)) return r;
    if (atcId == 0) return g_err.Fail(self, SCO_BAD_ARG, "an id of 0");
    Entity* e = StreamedEntity(atcId);
    if (!e || !e->atc) return g_err.Fail(self, SCO_NOT_FOUND, "that entity has no ATC data");
    if (!GetScene().spawned) return g_err.Fail(self, SCO_FAILED, "you're not spawned");
    e->atcPending = false;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

void Reset() { g_err.Reset(); }
void Release(const void* owner) { g_err.Forget(owner); }

const sc_hangar_v1 kTable = { sizeof(sc_hangar_v1), 0, LiftOfLandingArea, LiftRequest, AtcCancel, LastError };

}  // namespace

const ServiceDef& HangarService() {
    static const ServiceDef def{ SC_HANGAR_SERVICE_NAME, SC_HANGAR_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
