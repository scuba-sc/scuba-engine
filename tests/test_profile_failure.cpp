// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/app.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
void Fixture(const fs::path& root, const char* id, const fs::path& binary) {
    const fs::path dir = root / id;
    fs::create_directories(dir);
    fs::copy_file(binary, dir / "plugin.dll");
    std::ofstream(dir / "plugin.ini") << "id = " << id
        << "\nname = Lifecycle test\nversion = 1.0.0\napi = 1.1\nkind = native\nentry = plugin.dll\n";
}
std::string Read(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    if (argc != 5) return 1;
    const fs::path root = fs::absolute(argv[1]);
    fs::remove_all(root);
    fs::create_directories(root);
    const auto trace = (root / "trace.txt").string();
#ifdef _WIN32
    if (_putenv_s("SCUBA_PROFILE_TEST_TRACE", trace.c_str()) != 0) return 1;
#else
    if (setenv("SCUBA_PROFILE_TEST_TRACE", trace.c_str(), 1) != 0) return 1;
#endif
    Fixture(root, "a_good", argv[2]);
    Fixture(root, "b_bad", argv[3]);
    Fixture(root, "c_unselected", argv[4]);
    scuba::hostd::Kernel host;
    scuba::hostd::KernelOptions options;
    options.pluginRoot = root;
    options.selectiveProfile = true;
    options.profilePlugins = {"a_good", "b_bad"};
    CHECK(!host.Start(options, nullptr));
    CHECK(!host.started() && !scuba::hostd::mock_game::Published());
    CHECK(host.profileError().find("b_bad") != std::string::npos);
    CHECK(host.profileError().find("built for api 99.") != std::string::npos);
    const auto& plugins = sco::app::Plugins();
    CHECK(plugins.size() == 3);
    const auto staged = plugins.back().dir.parent_path();
    CHECK(!staged.empty() && !fs::exists(staged));
    CHECK(Read(trace) == "a_good:query\na_good:load\nb_bad:query\na_good:exit\na_good:unload\n");
    // A successful restart must not inherit the failed attempt or run the other modules.
    std::ofstream(trace, std::ios::trunc).close();
    options.profilePlugins = {"a_good"};
    CHECK(host.Start(options, nullptr));
    CHECK(host.plugins().size() == 2);
    host.Stop();
    CHECK(!scuba::hostd::mock_game::Published());
    CHECK(Read(trace) == "a_good:query\na_good:load\na_good:exit\na_good:unload\n");
    std::ofstream(trace, std::ios::trunc).close();
    options.profilePlugins = {"c_unselected", "a_good"};
    CHECK(host.Start(options, nullptr));
    host.Stop();
    CHECK(Read(trace) == "a_good:query\na_good:load\nc_unselected:query\nc_unselected:load\n"
                         "a_good:exit\nc_unselected:exit\nc_unselected:unload\na_good:unload\n");
#ifdef _WIN32
    _putenv_s("SCUBA_PROFILE_TEST_TRACE", "");
#else
    unsetenv("SCUBA_PROFILE_TEST_TRACE");
#endif
    fs::remove_all(root);
    return CheckExit("profile_failure");
}
