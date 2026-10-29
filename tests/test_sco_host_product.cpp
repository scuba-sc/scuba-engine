// SPDX-License-Identifier: GPL-3.0-only
// The engine's product-profile path: ScoHost::Start with productRoot mounts <root>/data/plugins,
// fails closed on a missing root or a refused plugin, and reports the roster it loaded.
#include "check.h"
#include "scuba/sco/sco_host.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void WriteDataPlugin(const fs::path& folder, const char* id) {
    fs::create_directories(folder / "lists");
    std::ofstream(folder / "plugin.ini") << "id = " << id
        << "\nname = Synthetic product plugin\nversion = 1.0.0\napi = 1.1\nkind = data\n";
    std::ofstream(folder / "lists" / "ships.txt") << "Synthetic_Ship\n";
}

bool Loaded(const std::vector<scuba::ScoPlugin>& plugins, const std::string& id) {
    for (const auto& plugin : plugins)
        if (plugin.id == id) return plugin.state == "loaded";
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const fs::path root = argv[1];
    std::error_code ec;
    fs::remove_all(root, ec);

    scuba::ScoHost host;
    scuba::ScoHostOptions options;
    options.productRoot = root;

    // No data/plugins: the product profile refuses before anything loads, and names the root.
    auto st = host.Start(options);
    CHECK(!st);
    CHECK(!host.started());
    CHECK(host.profileError().find("data/plugins") != std::string::npos);

    // A real roster loads, and every folder is reported.
    const auto plugins = root / "data" / "plugins";
    WriteDataPlugin(plugins / "catalog_a", "catalog_a");
    WriteDataPlugin(plugins / "catalog_b", "catalog_b");
    st = host.Start(options);
    CHECK(st);
    CHECK(host.started());
    CHECK(host.profileError().empty());
    const auto roster = host.Plugins();
    CHECK(roster.size() == 3);   // the mock initializer plus both data packs
    CHECK(Loaded(roster, "catalog_a"));
    CHECK(Loaded(roster, "catalog_b"));
    host.Tick();
    host.Stop();
    CHECK(!host.started());

    // Restart over the same root is stable.
    st = host.Start(options);
    CHECK(st);
    CHECK(host.Plugins().size() == roster.size());
    host.Stop();

    // A duplicate plugin id refuses the whole profile.
    const auto duplicate = plugins / "duplicate";
    WriteDataPlugin(duplicate, "catalog_a");
    st = host.Start(options);
    CHECK(!st);
    CHECK(!host.started());
    CHECK(host.profileError().find("refused") != std::string::npos);
    fs::remove_all(duplicate, ec);

    // A malformed manifest refuses the whole profile too.
    const auto malformed = plugins / "malformed";
    fs::create_directories(malformed);
    std::ofstream(malformed / "plugin.ini") << "id = other\n";
    st = host.Start(options);
    CHECK(!st);
    CHECK(host.profileError().find("refused") != std::string::npos);
    fs::remove_all(malformed, ec);

    // A plugin that discovers cleanly but fails to load is fatal as well: the roster never half-mounts.
    const auto broken = plugins / "broken_lua";
    fs::create_directories(broken);
    std::ofstream(broken / "plugin.ini")
        << "id = broken_lua\nname = Broken\nversion = 1.0.0\napi = 1.1\nkind = lua\nentry = main.lua\n";
    std::ofstream(broken / "main.lua") << "this is not lua (((\n";
    st = host.Start(options);
    CHECK(!st);
    CHECK(host.profileError().find("failed to load") != std::string::npos);
    CHECK(host.profileError().find("broken_lua") != std::string::npos);
    fs::remove_all(broken, ec);

    // With the offenders gone the same root starts again: the refusal is not sticky.
    st = host.Start(options);
    CHECK(st);
    host.Stop();

    fs::remove_all(root, ec);
    return CheckExit("sco_host_product");
}
