// game.input 1.0: static names from the locked public declaration registry.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"

#include <algorithm>
#include <cstring>

namespace scuba::hostd::mock_game {
namespace {

struct Action { const char* name; uint32_t id; uint32_t flags; };
#include "generated_mock_input_actions.inc"
static_assert(sizeof(actions) / sizeof(actions[0]) == SC_INPUT_ACTION_COUNT);

sco_result Check(sco_plugin* self) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!self || !LivePlugin(self)) return SCO_BAD_ARG;
    const auto services = Services();
    const auto service = std::find_if(services.begin(), services.end(), [](const ServiceInfo& item) {
        return std::strcmp(item.name.c_str(), SC_INPUT_SERVICE_NAME) == 0;
    });
    if (!Published() || !Ready("game.input.registry") || service == services.end() || !service->published)
        return SCO_UNAVAILABLE;
    return SCO_OK;
}

void Copy(const Action& action, sc_input_action_info* out) {
    const uint32_t size = out->size;
    *out = {};
    out->size = size;
    out->id = action.id;
    out->flags = action.flags;
    std::memcpy(out->name, action.name, std::strlen(action.name) + 1);
}

sco_result Count(sco_plugin* self, uint32_t* out) {
    const sco_result result = Check(self);
    if (result != SCO_OK) return result;
    if (!out) return SCO_BAD_ARG;
    *out = SC_INPUT_ACTION_COUNT;
    return SCO_OK;
}

sco_result ById(sco_plugin* self, uint32_t id, sc_input_action_info* out) {
    const sco_result result = Check(self);
    if (result != SCO_OK) return result;
    if (!out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    if (id == 0 || id > SC_INPUT_ACTION_COUNT) return SCO_NOT_FOUND;
    Copy(actions[id - 1], out);
    return SCO_OK;
}

sco_result ByName(sco_plugin* self, const char* name, sc_input_action_info* out) {
    const sco_result result = Check(self);
    if (result != SCO_OK) return result;
    if (!name || !out || out->size < sizeof(*out)) return SCO_BAD_ARG;
    const size_t length = std::strlen(name);
    if (!length || length >= SC_INPUT_ACTION_NAME_BYTES) return SCO_BAD_ARG;
    const auto found = std::find_if(std::begin(actions), std::end(actions), [name](const Action& action) {
        return std::strcmp(name, action.name) == 0;
    });
    if (found == std::end(actions)) return SCO_NOT_FOUND;
    Copy(*found, out);
    return SCO_OK;
}

void Reset() {}
void Release(const void*) {}

const sc_input_v1 kTable{ sizeof(sc_input_v1), 0, Count, ById, ByName };

}  // namespace

const ServiceDef& InputService() {
    static const ServiceDef def{ SC_INPUT_SERVICE_NAME, SC_INPUT_SERVICE_VERSION,
                                 &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
