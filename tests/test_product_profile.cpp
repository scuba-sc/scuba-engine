// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::filesystem::path root = argv[1];
    std::filesystem::remove_all(root);
    scuba::hostd::Kernel host;
    scuba::hostd::KernelOptions options;
    options.productRoot = root;
    CHECK(!host.Start(options, nullptr));
    CHECK(!host.profileError().empty() && !host.started());
    const auto plugins = root / "data" / "plugins";
    std::filesystem::create_directories(plugins);
    CHECK(!host.Start(options, nullptr));
    for (const char* name : {"catalog_b", "catalog_a"}) {
        const auto folder = plugins / name;
        std::filesystem::create_directories(folder / "lists");
        std::ofstream(folder / "plugin.ini") << "id = " << name
            << "\nname = Synthetic catalog\nversion = 1.0.0\napi = 1.1\nkind = data\n";
        std::ofstream(folder / "lists" / "ships.txt") << "Synthetic_Ship\n";
    }
    CHECK(host.Start(options, nullptr));
    const auto loaded = host.plugins();
    CHECK(loaded.size() == 3); // mock initializer plus both data packs
    CHECK(loaded[1].id == "catalog_a" && loaded[2].id == "catalog_b");
    host.Tick(1);
    host.Stop();
    CHECK(!host.started());
    CHECK(host.Start(options, nullptr));
    CHECK(host.plugins().size() == loaded.size());
    host.Stop();
    const auto duplicate = plugins / "duplicate";
    std::filesystem::create_directories(duplicate);
    std::ofstream(duplicate / "plugin.ini") << "id = catalog_a\nname = Duplicate\nversion = 1.0.0\napi = 1.1\nkind = data\n";
    CHECK(!host.Start(options, nullptr));
    std::filesystem::remove_all(duplicate);
    std::filesystem::create_directories(plugins / "malformed");
    std::ofstream(plugins / "malformed" / "plugin.ini") << "id = other\n";
    CHECK(!host.Start(options, nullptr));
    CHECK(host.profileError().find("refused") != std::string::npos);
    std::filesystem::remove_all(plugins / "malformed");
    const auto lua = plugins / "lua_probe";
    std::filesystem::create_directories(lua);
    std::ofstream(lua / "plugin.ini") << "id = lua_probe\nname = Lua profile probe\nversion = 1.0.0\napi = 1.1\nkind = lua\nentry = main.lua\n";
    std::ofstream(lua / "main.lua") << "sco.log('info', 'synthetic product profile Lua loaded')\n";
    CHECK(host.Start(options, nullptr));
    bool luaLoaded = false;
    for (const auto& plugin : host.plugins()) if (plugin.id == "lua_probe") luaLoaded = plugin.state == "loaded";
    CHECK(luaLoaded);
    host.Stop();
    const auto native = plugins / "mock_game_probe";
    std::filesystem::create_directories(native);
    std::filesystem::copy_file(argv[2], native / "mock_game_probe.dll");
    std::ofstream(native / "plugin.ini") << "id = mock_game_probe\nname = Native probe\nversion = 1.0.0\napi = 1.1\nkind = native\nentry = mock_game_probe.dll\nrequires = game.actors, game.entities, game.world, teleport.spatial, spawn.entities\n";
    CHECK(host.Start(options, nullptr));
    bool nativeLoaded = false;
    for (const auto& plugin : host.plugins()) if (plugin.id == "mock_game_probe") nativeLoaded = plugin.state == "loaded";
    CHECK(nativeLoaded);
    host.Stop();
    CHECK(host.Start(options, nullptr));
    host.Stop();
    std::filesystem::remove_all(root);
    return CheckExit("product_profile");
}
