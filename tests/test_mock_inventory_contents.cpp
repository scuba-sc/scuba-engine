// Synthetic-only contract test; no game or engine adapter is involved.
#include "hostd/mock_game/inventory_contents.h"
#include "hostd/mock_game/services.h"
#include "hostd/mock_game/mock_game.h"
#include <cassert>
#include <cstring>
#include <thread>

namespace mg = scuba::hostd::mock_game;
namespace scuba::hostd::mock_game {
static bool game = true, ready = true, published = true, live = true, throwServices = false, throwGame = false;
bool OnGame() { if (throwGame) throw 7; return game; }
bool LivePlugin(const sco_plugin* p) { return p && live; }
bool Ready(const char*) { return ready; }
bool Published() { return published; }
std::vector<ServiceInfo> Services() { if (throwServices) throw 42; return {{SC_CONTAINER_CONTENTS_NAME, SC_CONTAINER_CONTENTS_VERSION_1_0, published}}; }
}
int main() {
    auto& def = mg::InventoryContentsService();
    def.reset();
    auto* table = static_cast<const sc_container_contents_v1*>(def.table);
    auto* self = reinterpret_cast<sco_plugin*>(1);
    auto& values = mg::InventoryContents();
    values.containers[7] = { {sizeof(sc_container_item), 2, 11, 22, 5}, {sizeof(sc_container_item), 1, 12, 23, 3} };
    struct Extended { sc_container_snapshot out; unsigned char tail[8]; } e{};
    auto prep = [&](uint32_t capacity) { std::memset(&e, 0xa5, sizeof(e)); e.out.size = sizeof(e); e.out.count = capacity; };
    prep(1);
    mg::throwServices = true;
    bool escaped = false;
    sco_result exceptionResult = SCO_OK;
    try { exceptionResult = table->snapshot(self, 7, &e.out); } catch (...) { escaped = true; }
    mg::throwServices = false;
    assert(!escaped && exceptionResult == SCO_FAILED);
    sc_container_snapshot zero{}; zero.size = sizeof(e);
    assert(std::memcmp(&e.out, &zero, sizeof(zero)) == 0 && e.tail[0] == 0xa5);
    mg::throwGame = true;
    prep(1); e.out.size = sizeof(e.out) - 1;
    sc_container_snapshot unchanged = e.out;
    assert(table->snapshot(self, 7, &e.out) == SCO_FAILED);
    assert(std::memcmp(&e.out, &unchanged, sizeof(unchanged)) == 0 && e.tail[0] == 0xa5);
    assert(table->snapshot(self, 7, nullptr) == SCO_FAILED);
    mg::throwGame = false;
    prep(1);
    assert(table->snapshot(self, 7, &e.out) == SCO_OK);
    assert(e.out.size == sizeof(e) && e.out.count == 1 && e.out.total_seen == 2 && e.out.generation != 0);
    assert(e.out.container_id == 7 && e.out.flags == (SC_CONTAINER_CONTENTS_TRUNCATED | SC_CONTAINER_CONTENTS_VOLUME_KNOWN));
    assert(e.out.used_volume == 13 && e.out.items[0].item_id == 11 && e.tail[0] == 0xa5);
    prep(0); assert(table->snapshot(self, 7, &e.out) == SCO_OK && e.out.count == 0 && e.out.total_seen == 2 && (e.out.flags & SC_CONTAINER_CONTENTS_TRUNCATED));
    prep(64); assert(table->snapshot(self, 7, &e.out) == SCO_OK && e.out.count == 2 && !(e.out.flags & SC_CONTAINER_CONTENTS_TRUNCATED));
    prep(64); assert(table->snapshot(self, 8, &e.out) == SCO_NOT_FOUND && e.out.size == sizeof(e) && e.out.count == 0 && e.tail[0] == 0xa5);
    prep(65); assert(table->snapshot(self, 7, &e.out) == SCO_BAD_ARG && e.out.count == 0);
    prep(1); assert(table->snapshot(nullptr, 7, &e.out) == SCO_BAD_ARG && e.out.count == 0);
    prep(1); assert(table->snapshot(self, 0, &e.out) == SCO_BAD_ARG && e.out.count == 0);
    prep(1); mg::ready = false; assert(table->snapshot(self, 7, &e.out) == SCO_UNAVAILABLE && e.out.count == 0); mg::ready = true;
    prep(1); mg::published = false; assert(table->snapshot(self, 7, &e.out) == SCO_UNAVAILABLE); mg::published = true;
    prep(1); mg::game = false; assert(table->snapshot(self, 7, &e.out) == SCO_WRONG_THREAD && e.out.count == 0); mg::game = true;
    prep(1); mg::live = false; assert(table->snapshot(self, 7, &e.out) == SCO_BAD_ARG); mg::live = true;
    prep(1); e.out.size = sizeof(e.out)-1; assert(table->snapshot(self, 7, &e.out) == SCO_BAD_ARG && e.out.count == 1);
    assert(table->snapshot(self, 7, nullptr) == SCO_BAD_ARG);
    values.containers[8] = {}; prep(0); assert(table->snapshot(self, 8, &e.out) == SCO_OK && e.out.total_seen == 0 && !(e.out.flags & SC_CONTAINER_CONTENTS_TRUNCATED));
    values.containers[7][1].item_id = 11; prep(2); assert(table->snapshot(self, 7, &e.out) == SCO_FAILED && e.out.count == 0);
    values.containers[7][1].item_id = 12; values.volumeKnown = false; prep(2); assert(table->snapshot(self, 7, &e.out) == SCO_FAILED);
    values.containers[7][0].unit_volume = values.containers[7][1].unit_volume = 0; prep(2); assert(table->snapshot(self, 7, &e.out) == SCO_OK && e.out.flags == 0 && e.out.used_volume == 0);
    values.available = false; prep(2); assert(table->snapshot(self, 7, &e.out) == SCO_UNAVAILABLE && e.out.count == 0);
    def.reset(); prep(1); assert(table->snapshot(self, 7, &e.out) == SCO_NOT_FOUND);
}
