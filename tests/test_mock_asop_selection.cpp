#include "hostd/mock_game/services.h"
#include "hostd/mock_game/common.h"
#include "sc_asop_selection.h"
#include "check.h"
#include <cstring>
namespace mg = scuba::hostd::mock_game;
#ifdef SC_ASOP_SELECTION_REASON_BUSY
constexpr uint32_t kBusyReason = SC_ASOP_SELECTION_REASON_BUSY;
#else
constexpr uint32_t kBusyReason = SC_ASOP_SELECTION_REASON_CONFLICT;
#endif
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
    CHECK(t->state(a, &state) == SCO_UNAVAILABLE);
    mg::ready = true;
    CHECK(t->state(a, &state) == SCO_OK && state.session && (state.flags & SC_ASOP_SELECTION_FILE_KNOWN));
    uint64_t lease = 0, other = 0;
    CHECK(t->acquire(a, state.session, &lease) == SCO_OK && lease);
    CHECK(t->acquire(b, state.session, &other) == SCO_TOO_MANY);
    sc_asop_selection_read catalog{}; catalog.size = sizeof(catalog); catalog.kind = SC_ASOP_SELECTION_ROWS_CATALOGUE; catalog.session = state.session; catalog.revision = state.catalogue_generation;
    mg::scene.classes.insert("LaterShip");
    uint32_t catalogCount = 99, catalogMore = 99;
    CHECK(t->rows(a, &catalog, nullptr, 0, 0, &catalogCount, &catalogMore) == SCO_OK && catalogMore == 0);
    sc_asop_selection_class item{}; item.size = sizeof(item); std::strcpy(item.class_name, "TestShip");
    sc_asop_selection_request req{}; req.size = sizeof(req); req.session = state.session; req.file_revision = state.file_revision;
    req.client_key = 1; std::memcpy(req.file_digest, state.file_digest, 32); req.classes = &item; req.count = 1; req.class_stride = sizeof(item);
    uint64_t id = 0, retry = 0;
    std::strcpy(item.class_name, "bad/name");
    CHECK(t->replace(a, lease, &req, &id) == SCO_BAD_ARG && id == 0);
    std::strcpy(item.class_name, "TestShip");
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK && id);
    CHECK(t->replace(a, lease, &req, &retry) == SCO_OK && retry == id);
    mg::ready = false;
    def.tick();
    mg::ready = true;
    sc_asop_selection_receipt receipt{}; receipt.size = sizeof(receipt);
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_REVOKED);
    CHECK(t->state(a, &state) == SCO_OK && state.file_count == 0);
    req.client_key = 2;
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_ACCEPTED);
    def.tick();
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_FILE_COMMITTED);
    CHECK(t->state(a, &state) == SCO_OK && state.file_count == 1);
    sc_asop_selection_read read{}; read.size = sizeof(read); read.kind = SC_ASOP_SELECTION_ROWS_FILE; read.session = state.session; read.revision = state.file_revision;
    sc_asop_selection_row row{}; row.size = sizeof(row); uint32_t count = 0, more = 9;
    CHECK(t->rows(a, &read, &row, sizeof(row), 1, &count, &more) == SCO_OK && count == 1 && !more && !std::strcmp(row.class_name, "TestShip"));
    read.revision--; count = 77; CHECK(t->rows(a, &read, &row, sizeof(row), 1, &count, &more) == SCO_NOT_FOUND && count == 77);
    req.file_revision = state.file_revision; std::memcpy(req.file_digest, state.file_digest, 32); req.client_key = 3;
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    mg::ready = false;
    CHECK(t->refresh(a) == SCO_UNAVAILABLE);
    CHECK(t->rows(a, &read, &row, sizeof(row), 1, &count, &more) == SCO_UNAVAILABLE && count == 77);
    CHECK(t->acquire(b, state.session, &other) == SCO_UNAVAILABLE);
    CHECK(t->release(a, lease) == SCO_UNAVAILABLE);
    CHECK(t->replace(a, lease, &req, &retry) == SCO_UNAVAILABLE);
    CHECK(t->status(a, lease, id, &receipt) == SCO_UNAVAILABLE);
    mg::ready = true;
    CHECK(t->release(a, lease) == SCO_OK);
    def.tick();
    CHECK(t->state(a, &state) == SCO_OK && state.file_count == 1);
    CHECK(t->acquire(b, state.session, &other) == SCO_OK && other != lease);
    // Busy is derived from actual scene vehicles, including those streaming in or being
    // removed until the entity leaves the scene. Non-vehicles never block a class.
    mg::scene = mg::Scene{};
    mg::scene.classes.insert("Alpha");
    mg::scene.classes.insert("Beta");
    def.reset();
    CHECK(t->state(a, &state) == SCO_OK);
    CHECK(t->acquire(a, state.session, &lease) == SCO_OK);
    sc_asop_selection_class selected[2]{};
    for (auto& entry : selected) entry.size = sizeof(entry);
    std::strcpy(selected[0].class_name, "Alpha");
    std::strcpy(selected[1].class_name, "Beta");
    req = {}; req.size = sizeof(req); req.session = state.session; req.file_revision = state.file_revision;
    std::memcpy(req.file_digest, state.file_digest, 32); req.client_key = 1;
    req.classes = selected; req.count = 2; req.class_stride = sizeof(selected[0]);
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    def.tick();
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_FILE_COMMITTED);
    CHECK(t->state(a, &state) == SCO_OK && state.file_count == 2);

    mg::Entity unrelated{}; unrelated.cls = "alpha"; unrelated.vehicle = false;
    mg::scene.entities[10] = unrelated;
    mg::Entity ship{}; ship.cls = "aLpHa"; ship.vehicle = true; ship.streamed = false; ship.removing = true;
    mg::scene.entities[11] = ship;
    sc_asop_selection_row rows[2]{};
    for (auto& entry : rows) entry.size = sizeof(entry);
    read = {}; read.size = sizeof(read); read.kind = SC_ASOP_SELECTION_ROWS_FILE;
    read.session = state.session; read.revision = state.file_revision;
    CHECK(t->rows(a, &read, rows, sizeof(rows[0]), 2, &count, &more) == SCO_OK && count == 2 && !more);
#ifdef SC_ASOP_SELECTION_ROW_BUSY
    CHECK((rows[0].flags & SC_ASOP_SELECTION_ROW_BUSY) && !(rows[1].flags & SC_ASOP_SELECTION_ROW_BUSY));
#endif
    read.kind = SC_ASOP_SELECTION_ROWS_CATALOGUE; read.revision = state.catalogue_generation;
    CHECK(t->rows(a, &read, rows, sizeof(rows[0]), 2, &count, &more) == SCO_OK && count == 2 && !more);
#ifdef SC_ASOP_SELECTION_ROW_BUSY
    CHECK((rows[0].flags & SC_ASOP_SELECTION_ROW_BUSY) && !(rows[1].flags & SC_ASOP_SELECTION_ROW_BUSY));
#endif

    // Retaining a busy class, even with a different case and order, is allowed.
    std::strcpy(selected[0].class_name, "Beta"); std::strcpy(selected[1].class_name, "ALPHA");
    req.file_revision = state.file_revision; std::memcpy(req.file_digest, state.file_digest, 32); req.client_key = 2;
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    def.tick();
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_FILE_COMMITTED);
    CHECK(t->state(a, &state) == SCO_OK);
    const auto before = state;

    // Busy can appear after acceptance: recheck at commit and retain the document.
    mg::scene.entities.erase(11);
    req.file_revision = state.file_revision; std::memcpy(req.file_digest, state.file_digest, 32); req.client_key = 3; req.count = 1;
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_ACCEPTED);
    mg::scene.entities[11] = ship;
    def.tick();
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_CONFLICT && receipt.reason == kBusyReason && receipt.file_revision == before.file_revision);
    CHECK(t->replace(a, lease, &req, &retry) == SCO_OK && retry == id);
    CHECK(t->status(a, lease, retry, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_CONFLICT && receipt.reason == kBusyReason);
    CHECK(t->state(a, &state) == SCO_OK && state.file_revision == before.file_revision && state.semantic_revision == before.semantic_revision && state.flags == before.flags && state.file_count == before.file_count && !std::memcmp(state.file_digest, before.file_digest, 32));
    read.kind = SC_ASOP_SELECTION_ROWS_FILE; read.revision = state.file_revision;
    CHECK(t->rows(a, &read, rows, sizeof(rows[0]), 2, &count, &more) == SCO_OK && count == 2 && !std::strcmp(rows[0].class_name, "Beta") && !std::strcmp(rows[1].class_name, "ALPHA"));
#ifdef SC_ASOP_SELECTION_ROW_BUSY
    CHECK(!(rows[0].flags & SC_ASOP_SELECTION_ROW_BUSY) && (rows[1].flags & SC_ASOP_SELECTION_ROW_BUSY));
#endif

    mg::scene.entities.erase(11); // the remaining matching non-vehicle does not block
#ifdef SC_ASOP_SELECTION_ROW_BUSY
    CHECK(t->rows(a, &read, rows, sizeof(rows[0]), 2, &count, &more) == SCO_OK && !(rows[1].flags & SC_ASOP_SELECTION_ROW_BUSY));
#endif
    const uint64_t conflictId = id;
    req.client_key = 4;
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    def.tick();
    CHECK(t->status(a, lease, id, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_FILE_COMMITTED && receipt.reason == SC_ASOP_SELECTION_REASON_NONE);
    CHECK(t->state(a, &state) == SCO_OK && state.file_count == 1 && state.file_revision == before.file_revision + 1);
    req.client_key = 3;
    CHECK(t->replace(a, lease, &req, &retry) == SCO_OK && retry == conflictId);
    CHECK(t->status(a, lease, retry, &receipt) == SCO_OK && receipt.phase == SC_ASOP_SELECTION_EDIT_CONFLICT && receipt.reason == kBusyReason);
    const auto committed = state;
    req.file_revision = state.file_revision; std::memcpy(req.file_digest, state.file_digest, 32); req.client_key = 5; req.count = 0;
    CHECK(t->replace(a, lease, &req, &id) == SCO_OK);
    CHECK(t->release(a, lease) == SCO_OK);
    def.tick();
    CHECK(t->state(a, &state) == SCO_OK && state.file_revision == committed.file_revision && state.semantic_revision == committed.semantic_revision && state.file_count == committed.file_count && !std::memcmp(state.file_digest, committed.file_digest, 32));
    return CheckExit("test_mock_asop_selection");

}
