// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/app.h"
#include "sco/runtime.h"
#include <sc_actor_stats.h>
#include <string>
#include <utility>
namespace mg = scuba::hostd::mock_game;
static sco_result Read(int64_t entity, int64_t stat, std::string& text) {
    sco_arg args[2]{};
    args[0].type = args[1].type = SCO_ARG_INT;
    args[0].v.i = entity; args[1].v.i = stat;
    sco_result completed = SCO_FAILED;
    text.clear();
    std::pair<sco_result*, std::string*> context{&completed, &text};
    const auto immediate = sco::Invoke("actor_conditions.read", reinterpret_cast<const sco::Arg*>(args), 2,
        [](sco::Result r, const char* reply, void* ctx) {
            auto& result = *static_cast<std::pair<sco_result*, std::string*>*>(ctx);
            *result.first = static_cast<sco_result>(r); *result.second = reply ? reply : "";
        }, &context);
    CHECK(static_cast<sco_result>(immediate) == completed);
    return completed;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    sco::app::Platform platform;
    platform.hostVersion = "actor SDK consumer fixture";
    platform.pluginRoot = argv[1]; platform.pluginsEnabled = true;
    mg::Configure(platform); CHECK(sco::app::Start(platform));
    bool loaded = false;
    for (const auto& plugin : sco::app::Plugins())
        if (plugin.manifest.id == "actor_conditions") loaded = plugin.state == sco::plugins::State::Loaded;
    CHECK(loaded);
    std::string text;
    CHECK(Read(256, SC_ACTOR_STAT_HEALTH_HEAD, text) == SCO_OK && text == "stat=15 raw=100");
    CHECK(Read(256, SC_ACTOR_STAT_WEAR_HEAD, text) == SCO_OK && text == "stat=21 raw=0");
    CHECK(Read(999, SC_ACTOR_STAT_HYGIENE, text) == SCO_NOT_FOUND);
    CHECK(Read(256, 30, text) == SCO_BAD_ARG);
    CHECK(mg::SetCapability("game.actor_stats.read", false));
    CHECK(Read(256, SC_ACTOR_STAT_HUNGER, text) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.actor_stats.read", true));
    CHECK(sco::app::Reload("actor_conditions") == sco::Result::Ok);
    CHECK(Read(256, SC_ACTOR_STAT_HEALTH_HEAD, text) == SCO_OK);
    sco::app::Stop(); mg::Unpublish();
    return CheckExit("test_actor_plugin");
}
