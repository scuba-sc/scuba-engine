#include "hostd/mock_game/datacore_live.h"
#include "hostd/mock_game/services.h"
#include "sc_datacore_live.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

namespace mg = scuba::hostd::mock_game;
static bool game = true, ready = true;
static auto* owner = reinterpret_cast<sco_plugin*>(1);
namespace scuba::hostd::mock_game {
bool OnGame() { return game; }
bool LivePlugin(const sco_plugin* self) { return self == owner; }
bool Ready(const char*) { return ready; }
}
int main() {
    const auto& def = mg::DatacoreLiveService();
    def.reset();
    const auto* t = static_cast<const sc_datacore_live_v1*>(def.table);
    assert(t->is_ready(owner) == SCO_OK);
    assert(t->is_ready(nullptr) == SCO_BAD_ARG);
    sc_datacore_pool_stats stats{}; stats.size = sizeof(stats);
    assert(t->get_pool_stats(owner, SC_DC_POOL_UINT32, &stats) == SCO_OK);
    assert(stats.element_size == 4 && stats.element_count == 2 && stats.total_bytes == 8);
    uint8_t bytes[8]{}; size_t n = 99;
    assert(t->read_pool(owner, SC_DC_POOL_UINT32, 0, bytes, sizeof(bytes), &n) == SCO_OK && n == 8);
    const uint8_t expected[8] = {1,0,0,0,2,0,0,0};
    assert(std::memcmp(bytes, expected, 8) == 0);
    assert(t->read_pool(owner, SC_DC_POOL_UINT32, 6, bytes, 8, &n) == SCO_OK && n == 2);
    assert(t->read_pool(owner, SC_DC_POOL_UINT32, 8, bytes, 8, &n) == SCO_OK && n == 0);
    assert(t->read_pool(owner, SC_DC_POOL_UINT32, std::numeric_limits<uint64_t>::max(), bytes, 8, &n) == SCO_BAD_ARG && n == 0);
    assert(t->read_pool(owner, SC_DC_POOL_COUNT, 0, bytes, 8, &n) == SCO_BAD_ARG);
    assert(t->read_pool(owner, 0, 0, nullptr, 1, &n) == SCO_BAD_ARG);
    assert(t->read_pool(owner, 0, 0, bytes, 1, nullptr) == SCO_BAD_ARG);
    stats.size = 1;
    assert(t->get_pool_stats(owner, 0, &stats) == SCO_BAD_ARG);
    uint8_t guid[16] = {1}; sc_datacore_live_record_info record{}; record.size = sizeof(record);
    assert(t->find_record(owner, guid, &record) == SCO_OK && record.struct_type == 1 && record.field_count == 2);
    guid[0] = 2;
    assert(t->find_record(owner, guid, &record) == SCO_NOT_FOUND);
    assert(t->find_record(owner, nullptr, &record) == SCO_BAD_ARG);
    n = 99;
    assert(t->write_pool(owner, SC_DC_POOL_UINT32, 0, bytes, 1, &n) == SCO_UNAVAILABLE && n == 0);
    auto* invalid = reinterpret_cast<sco_plugin*>(2);
    stats.size = sizeof(stats);
    assert(t->is_ready(invalid) == SCO_BAD_ARG);
    assert(t->get_pool_stats(invalid, 0, &stats) == SCO_BAD_ARG);
    assert(t->find_record(invalid, guid, &record) == SCO_BAD_ARG);
    assert(t->read_pool(invalid, 0, 0, bytes, 1, &n) == SCO_BAD_ARG && n == 0);
    assert(t->write_pool(invalid, 0, 0, bytes, 1, &n) == SCO_BAD_ARG && n == 0);
    assert(t->read_pool(owner, 0, 0, bytes, 0, &n) == SCO_BAD_ARG);
    assert(t->read_pool(owner, 0, 0, nullptr, 0, &n) == SCO_BAD_ARG);
    assert(t->write_pool(owner, SC_DC_POOL_COUNT, 0, bytes, 1, &n) == SCO_BAD_ARG);
    assert(t->write_pool(owner, 0, 0, nullptr, 1, &n) == SCO_BAD_ARG);
    assert(t->write_pool(owner, 0, 0, bytes, 0, &n) == SCO_BAD_ARG);
    assert(t->write_pool(owner, 0, 0, bytes, 1, nullptr) == SCO_BAD_ARG);
    ready = false;
    assert(t->is_ready(owner) == SCO_UNAVAILABLE);
    assert(t->read_pool(owner, 0, 0, bytes, 1, &n) == SCO_UNAVAILABLE && n == 0);
    game = false;
    assert(t->is_ready(owner) == SCO_WRONG_THREAD);
    return 0;
}
