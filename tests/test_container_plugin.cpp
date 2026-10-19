// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/app.h"
#include "sco/host.h"
#include "sco/runtime.h"
#include <sc_container_contents.h>
#include <cstdio>
#include <string>
#include <utility>
namespace mg = scuba::hostd::mock_game;
static std::string command_name = "container_reader.read";
static sco_result Read(int64_t id, std::string& text) {
    sco_arg arg{}; arg.type = SCO_ARG_INT; arg.v.i = id;
    sco_result completed = SCO_FAILED;
    text.clear();
    std::pair<sco_result*, std::string*> context{&completed, &text};
    const auto queued = sco::Invoke(command_name.c_str(), reinterpret_cast<const sco::Arg*>(&arg), 1,
        [](sco::Result result, const char* message, void* context) {
            auto* pair = static_cast<std::pair<sco_result*, std::string*>*>(context);
            *pair->first = static_cast<sco_result>(result);
            *pair->second = message ? message : "";
        }, &context);
    CHECK(static_cast<sco_result>(queued) == completed);
    return completed;
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    const std::string plugin_id = argc == 3 ? argv[2] : "container_reader";
    command_name = plugin_id + ".read";
    sco::app::Platform platform;
    platform.hostVersion = "container SDK plugin fixture";
    platform.pluginRoot = argv[1]; platform.pluginsEnabled = true;
    mg::Configure(platform);
    CHECK(sco::app::Start(platform));
    const auto& plugins = sco::app::Plugins();
    bool loaded = false;
    for (const auto& plugin : plugins)
        if (plugin.manifest.id == plugin_id) loaded = plugin.state == sco::plugins::State::Loaded;
    CHECK(loaded);
    std::string text;
    CHECK(Read(289, text) == SCO_OK);
    CHECK(text.find("items=2 total=2 generation=1") != std::string::npos);
    CHECK(Read(290, text) == SCO_OK && text.find("items=0 total=0") != std::string::npos);
    CHECK(Read(999, text) == SCO_NOT_FOUND);
    CHECK(mg::SetCapability("game.inventory.contents.read", false));
    CHECK(Read(289, text) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.inventory.contents.read", true));
    CHECK(sco::app::Reload(plugin_id) == sco::Result::Ok);
    CHECK(Read(289, text) == SCO_OK);
    sco::app::Stop(); mg::Unpublish();
    return CheckExit("test_container_plugin");
}
