// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/scenario_runner.h"
#include "sco/app.h"
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 4) return 1;
    const fs::path root = argv[1];
    fs::remove_all(root);
    fs::create_directories(root / "mock_game_probe");
    fs::copy_file(argv[2], root / "mock_game_probe" / "mock_game_probe.dll");
    std::ofstream(root / "mock_game_probe" / "plugin.ini")
        << "id = mock_game_probe\nname = Native probe\nversion = 1.0.0\napi = 1.1\nkind = native\n"
           "entry = mock_game_probe.dll\nrequires = game.actors, game.entities, game.world, teleport.spatial, spawn.entities\n";
    fs::create_directories(root / "unselected");
    std::ofstream(root / "unselected" / "plugin.ini") << "invalid manifest\n";
    scuba::hostd::Kernel host;
    scuba::hostd::KernelOptions options;
    options.pluginRoot = root;
    options.selectiveProfile = true;
    options.mockEvents = true;
    options.profilePlugins = {"mock_game_probe"};
    for (const auto& service : scuba::hostd::mock_game::Services())
        options.profileServices.push_back(service.name);
    for (int iteration = 0; iteration < 2; ++iteration) {
        if (!host.Start(options, [](const char* line) { std::fprintf(stderr, "%s\n", line); })) { std::fprintf(stderr, "startup: %s\n", host.profileError().c_str()); return 1; }
        const auto list = host.plugins();
        CHECK(list.size() == 2);
        CHECK(list.back().id == "mock_game_probe" && list.back().state == "loaded");
        const auto staged = sco::app::Plugins().back().dir.parent_path();
        CHECK(staged != root && fs::exists(staged));
#ifdef SCUBA_HAVE_ACTOR_STATS
        bool found = false;
        for (const auto& cap : scuba::hostd::mock_game::Capabilities()) {
            if (cap.name == "game.actor_stats.write") { found = true; CHECK(!cap.ready); }
        }
        CHECK(found);
#endif
        scuba::hostd::Json scenario;
        std::string error;
        CHECK(scuba::hostd::LoadScenario(argv[3], scenario, error));
        CHECK(scuba::hostd::RunScenario(scenario));
        host.Tick(1);
        host.Stop();
        CHECK(!fs::exists(staged));
    }
    options.profileServices.clear();
    CHECK(!host.Start(options, nullptr));
    CHECK(!host.started() && host.profileError().find("failed to load") != std::string::npos);
    options.profilePlugins = {"missing"};
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("unavailable") != std::string::npos);
    for (const std::string& id : {std::string("../escape"), std::string("/absolute"), std::string("A"), std::string(32, 'a')}) {
        options.profilePlugins = {id};
        CHECK(!host.Start(options, nullptr));
        CHECK(host.profileError().find("invalid") != std::string::npos);
    }
    options.profilePlugins.clear();
    for (int i = 0; i < 129; ++i) options.profilePlugins.push_back("item_" + std::to_string(i));
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("too many") != std::string::npos);
    options.profilePlugins = {"mock_game_probe", "mock_game_probe"};
    CHECK(!host.Start(options, nullptr));
    options.profilePlugins = {"mock_game_probe"};
    std::ofstream(root / "mock_game_probe" / "disabled") << "";
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("refused") != std::string::npos);
    fs::remove(root / "mock_game_probe" / "disabled");
    fs::create_directories(root / "lua_probe");
    std::ofstream(root / "lua_probe" / "plugin.ini") << "id = lua_probe\nname = Lua probe\nversion = 1.0.0\napi = 1.1\nkind = lua\nentry = main.lua\n";
    std::ofstream(root / "lua_probe" / "main.lua") << "sco.log('info', 'profile Lua loaded')\n";
    options.profilePlugins = {"lua_probe"};
    options.lua = false;
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("no script runtime") != std::string::npos);
    options.lua = true;
    CHECK(host.Start(options, nullptr));
    host.Stop();
    fs::create_directories(root / "catalog" / "lists");
    std::ofstream(root / "catalog" / "plugin.ini") << "id = catalog\nname = Catalog\nversion = 1.0.0\napi = 1.1\nkind = data\n";
    std::ofstream(root / "catalog" / "lists" / "ships.txt") << "Synthetic_Ship\n";
    std::ofstream(root / "lua_probe" / "plugin.ini") << "id = lua_probe\nname = Lua probe\nversion = 1.0.0\napi = 1.1\nkind = lua\nentry = main.lua\nrequires = catalog\n";
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("failed to load") != std::string::npos);
    options.profilePlugins = {"lua_probe", "catalog"};
    CHECK(host.Start(options, nullptr));
    const auto composed = host.plugins();
    CHECK(composed.size() == 3 && composed[1].id == "catalog" && composed[1].state == "loaded" &&
          composed[2].id == "lua_probe" && composed[2].state == "loaded");
    host.Stop();
    options.profilePlugins = {"lua_probe"};
#ifndef _WIN32
    const fs::path outside = root.string() + "-outside";
    fs::create_directories(outside);
    std::ofstream(outside / "outside.lua") << "error('outside source executed')\n";
    fs::create_directory_symlink(outside, root / "linked_plugin");
    options.profilePlugins = {"linked_plugin"};
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("unavailable") != std::string::npos);
    fs::remove(root / "linked_plugin");
    options.profilePlugins = {"lua_probe"};
    fs::create_symlink(outside / "outside.lua", root / "lua_probe" / "link");
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("symlink") != std::string::npos);
    fs::remove_all(outside);
#endif
    fs::remove(root / "lua_probe" / "link");
    auto deep = root / "lua_probe" / "deep";
    for (int i = 0; i < 34; ++i) deep /= "d";
    fs::create_directories(deep);
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("depth limit") != std::string::npos);
    fs::remove_all(root / "lua_probe" / "deep");
    const auto many = root / "lua_probe" / "many";
    fs::create_directories(many);
    for (int i = 0; i < 4097; ++i) std::ofstream(many / ("f" + std::to_string(i)));
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("entry or depth limit") != std::string::npos);
    fs::remove_all(many);
    const auto huge = root / "lua_probe" / "huge";
    std::ofstream(huge) << "x";
    fs::resize_file(huge, 256u * 1024u * 1024u + 1u);
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("byte limit") != std::string::npos);
    fs::remove_all(root);
    return CheckExit("selected_profile");
}
