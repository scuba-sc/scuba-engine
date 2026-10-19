// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include "hostd/product_bundle.h"
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    CHECK(argc == 2);
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
    const auto bundle = root.parent_path() / "product-bundle-fixture";
    std::filesystem::remove_all(bundle);
    std::string exportError;
    CHECK(scuba::hostd::ExportProductBundle(root, bundle, exportError));
    CHECK(std::filesystem::exists(bundle / "SHA256SUMS"));
    CHECK(!scuba::hostd::ExportProductBundle(root, bundle, exportError));
    CHECK(!scuba::hostd::ExportProductBundle(root, plugins / "nested-export", exportError));
    options.productRoot = bundle;
    CHECK(host.Start(options, nullptr));
    CHECK(host.plugins().size() == 4);
    host.Stop();
    std::filesystem::remove_all(bundle);
    CHECK(!std::filesystem::exists(bundle));
    std::filesystem::remove_all(root);
    CHECK(!std::filesystem::exists(root));
    return 0;
}
