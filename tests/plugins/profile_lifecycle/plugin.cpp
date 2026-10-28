// SPDX-License-Identifier: GPL-3.0-only
#include "sco_api.h"
#include <cstdlib>
#include <fstream>

namespace {
void Trace(const char* action) {
    if (const char* path = std::getenv("SCUBA_PROFILE_TEST_TRACE"))
        std::ofstream(path, std::ios::app) << PROFILE_PLUGIN_ID << ':' << action << '\n';
}
void OnExit(const char*, const void*, void*) { Trace("exit"); }
}
extern "C" SCO_EXPORT const sco_plugin_info* sco_plugin_query() {
    Trace("query");
    static const sco_plugin_info info{sizeof(sco_plugin_info), PROFILE_API_MAJOR, SCO_API_MINOR,
                                      PROFILE_PLUGIN_ID, "1.0.0", "test"};
    return &info;
}
extern "C" SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    Trace("load");
    return api->subscribe(self, "game.exit", OnExit, nullptr);
}
extern "C" SCO_EXPORT void sco_plugin_unload() { Trace("unload"); }
