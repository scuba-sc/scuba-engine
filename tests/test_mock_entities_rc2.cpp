#include "check.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sco/app.h"
#include "sc_entities.h"
#include <cstring>
namespace mg = scuba::hostd::mock_game;
int main() {
    sco::app::Platform p; CHECK(sco::app::Start(p));
    mg::ResetScene();
    const auto& def = mg::EntitiesService();
    CHECK(def.version == SC_ENTITIES_VERSION_1_2);
    auto* e = static_cast<const sc_entities_v1*>(def.table);
    CHECK(e->list_classes && e->find_class && e->find_by_name && e->component);
    if (e->list_classes && e->find_class && e->find_by_name && e->component) {
        sc_entity_class_page page{}; page.size = sizeof(page); page.count = 1;
        CHECK(e->list_classes(0, 0, &page) == SCO_OK && page.count == 1 && page.more > 0 && page.epoch);
        sc_entity_class_record r{}; r.size = sizeof(r);
        CHECK(e->find_class(page.records[0].name, &r) == SCO_OK && r.class_id == page.records[0].class_id);
        CHECK(e->list_classes(page.epoch + 1, 0, &page) == SCO_NOT_FOUND);
        sc_entity_component_record c{}; c.size = sizeof(c);
        CHECK(e->component(mg::GetScene().player, SC_ENTITY_COMPONENT_ACTOR, &c) == SCO_OK && c.generation);
        CHECK(e->component(mg::GetScene().player, 99, &c) == SCO_BAD_ARG);
    }
    sco::app::Stop(); return CheckExit("test_mock_entities_rc2");
}
