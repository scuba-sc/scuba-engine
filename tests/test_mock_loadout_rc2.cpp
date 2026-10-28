#include "check.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sco/app.h"
#include "sco/host.h"
#include "sco/caps.h"
#include "sc_loadout.h"
#include <cstring>
namespace mg = scuba::hostd::mock_game;
int main() {
    sco::app::Platform p; CHECK(sco::app::Start(p)); mg::ResetScene();
    CHECK(sco::caps::Set("game.loadout.apply", true, nullptr) == sco::Result::Ok);
    const auto& def = mg::LoadoutService();
    CHECK(def.version == SC_LOADOUT_VERSION_1_2);
    auto* t = static_cast<const sc_loadout_v1_2*>(def.table);
    if (def.version == SC_LOADOUT_VERSION_1_2) {
        CHECK(t->submit && t->status && t->cancel && def.tick);
        sco_plugin* owner = sco::host::NewPlugin("rc2_test");
        sc_loadout_item item{}; item.slot = SC_LOADOUT_SLOT_HEAD;
        const auto name = *mg::GetScene().items.begin(); std::strcpy(item.item_class, name.c_str());
        sc_loadout_request req{sizeof(req), 1, sizeof(item), 0, &item};
        uint64_t id = 0; CHECK(t->submit(owner, &req, &id) == SCO_OK && id);
        std::strcpy(item.item_class, "changed");
        sc_loadout_job_report report{}; report.size = sizeof(report);
        CHECK(t->status(owner, id, &report) == SCO_OK && report.state == SC_LOADOUT_JOB_PENDING && !report.completed);
        def.tick(); report.size = sizeof(report);
        CHECK(t->status(owner, id, &report) == SCO_OK && report.state == SC_LOADOUT_JOB_COMPLETE && report.completed == 1 && report.report.applied == 1);
        CHECK(mg::EquippedItems()[SC_LOADOUT_SLOT_HEAD] == name);
        CHECK(t->cancel(owner, id) == SCO_OK);
        CHECK(t->status(owner, id, &report) == SCO_NOT_FOUND);
    }
    sco::app::Stop(); return CheckExit("test_mock_loadout_rc2");
}
