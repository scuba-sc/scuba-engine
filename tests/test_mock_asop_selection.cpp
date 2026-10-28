#include "hostd/mock_game/services.h"
#include "hostd/mock_game/common.h"
#include "sc_asop_selection.h"
#include <cassert>
#include <cstring>
namespace mg = scuba::hostd::mock_game;
namespace scuba::hostd::mock_game {
const ServiceDef& AsopSelectionService();
static Scene scene;
static bool ready = true;
bool Ready(const char*) { return ready; }
Scene& GetScene() { return scene; }
bool OnGame() { return true; }
bool LivePlugin(const sco_plugin* p) { return p != nullptr; }
}
int main() {
    auto& def = mg::AsopSelectionService(); def.reset();
    auto* t = static_cast<const sc_asop_selection_v1*>(def.table);
    auto* a = reinterpret_cast<sco_plugin*>(1); auto* b = reinterpret_cast<sco_plugin*>(2);
    sc_asop_selection_state state{}; state.size = sizeof(state);
    mg::ready = false;
    assert(t->state(a, &state) == SCO_UNAVAILABLE);
    mg::ready = true;
    assert(t->state(a, &state) == SCO_OK && state.session && (state.flags & SC_ASOP_SELECTION_FILE_KNOWN));
    uint64_t lease = 0, other = 0;
    assert(t->acquire(a, state.session, &lease) == SCO_OK && lease);
    assert(t->acquire(b, state.session, &other) == SCO_TOO_MANY);
    sc_asop_selection_read catalog{}; catalog.size = sizeof(catalog); catalog.kind = SC_ASOP_SELECTION_ROWS_CATALOGUE; catalog.session = state.session; catalog.revision = state.catalogue_generation;
    mg::scene.classes.insert("LaterShip");
    uint32_t catalogCount = 99, catalogMore = 99;
    assert(t->rows(a, &catalog, nullptr, 0, 0, &catalogCount, &catalogMore) == SCO_OK && catalogMore == 0);
    sc_asop_selection_class item{}; item.size = sizeof(item); std::strcpy(item.class_name, "TestShip");
    sc_asop_selection_request req{}; req.size = sizeof(req); req.session = state.session; req.file_revision = state.file_revision;
    req.client_key = 1; std::memcpy(req.file_digest, state.file_digest, 32); req.classes = &item; req.count = 1; req.class_stride = sizeof(item);
    uint64_t id = 0, retry = 0;
    std::strcpy(item.class_name, "bad/name");
    assert(t->replace(a, lease, &req, &id) == SCO_BAD_ARG && id == 0);
    std::strcpy(item.class_name, "TestShip");
    assert(t->replace(a, lease, &req, &id) == SCO_OK && id);
    assert(t->replace(a, lease, &req, &retry) == SCO_OK && retry == id);
    mg::ready = false;
    def.tick();
    mg::ready = true;
    sc_asop_selection_receipt receipt{}; receipt.size = sizeof(receipt);
    assert(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_REVOKED);
    assert(t->state(a, &state) == SCO_OK && state.file_count == 0);
    req.client_key = 2;
    assert(t->replace(a, lease, &req, &id) == SCO_OK);
    assert(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_ACCEPTED);
    def.tick();
    assert(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_FILE_COMMITTED);
    assert(t->state(a, &state) == SCO_OK && state.file_count == 1);
    sc_asop_selection_read read{}; read.size = sizeof(read); read.kind = SC_ASOP_SELECTION_ROWS_FILE; read.session = state.session; read.revision = state.file_revision;
    sc_asop_selection_row row{}; row.size = sizeof(row); uint32_t count = 0, more = 9;
    assert(t->rows(a, &read, &row, sizeof(row), 1, &count, &more) == SCO_OK && count == 1 && !more && !std::strcmp(row.class_name, "TestShip"));
    read.revision--; count = 77; assert(t->rows(a, &read, &row, sizeof(row), 1, &count, &more) == SCO_NOT_FOUND && count == 77);
    req.file_revision = state.file_revision; std::memcpy(req.file_digest, state.file_digest, 32); req.client_key = 3;
    assert(t->replace(a, lease, &req, &id) == SCO_OK);
    mg::ready = false;
    assert(t->refresh(a) == SCO_UNAVAILABLE);
    assert(t->rows(a, &read, &row, sizeof(row), 1, &count, &more) == SCO_UNAVAILABLE && count == 77);
    assert(t->acquire(b, state.session, &other) == SCO_UNAVAILABLE);
    assert(t->release(a, lease) == SCO_UNAVAILABLE);
    assert(t->replace(a, lease, &req, &retry) == SCO_UNAVAILABLE);
    assert(t->status(a, lease, id, &receipt) == SCO_UNAVAILABLE);
    mg::ready = true;
    assert(t->release(a, lease) == SCO_OK);
    def.tick();
    assert(t->state(a, &state) == SCO_OK && state.file_count == 1);
    assert(t->acquire(b, state.session, &other) == SCO_OK && other != lease);
}
