#include "check.h"
#include "hostd/designer_export.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace scuba::hostd;

int main() try {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() / ("scuba-export-" + std::to_string(nonce));
    const DesignerExportOptions options{TEST_DESIGNER_SDK_ROOT, root, TEST_DESIGNER_PYTHON};
    Json params = Json::MakeObject();
    params.set("id", "synthetic_panel"); params.set("language", "cpp");
    params.set("source", "// synthetic export fixture\n");
    const Json out = ExportDesignerPlugin(params, options);
    CHECK(out.find("folder")->as_string() == "synthetic_panel");
    CHECK(out.find("files")->size() == 3);
    const auto folder = root / "synthetic_panel";
    CHECK(std::filesystem::is_regular_file(folder / "CMakeLists.txt"));
    CHECK(std::filesystem::is_regular_file(folder / "plugin.ini"));
    std::ifstream file(folder / "synthetic_panel.cpp", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(text == "// synthetic export fixture\n");
    bool refused = false;
    try { (void)ExportDesignerPlugin(params, options); } catch (const HandlerError& e) { refused = e.code() == -32010; }
    CHECK(refused);
    for (const auto* id : {"../escape", "sco", "has space", "x;command"}) {
        params.set("id", id); refused = false;
        try { (void)ExportDesignerPlugin(params, options); } catch (const HandlerError& e) { refused = e.code() == -32602; }
        CHECK(refused);
    }
    params.set("id", "failed_panel");
    params.set("language", "lua");
    refused = false;
    try { (void)ExportDesignerPlugin(params, options); } catch (const HandlerError& e) { refused = e.code() == -32602; }
    CHECK(refused);
    params.set("language", "c");
    auto broken = options;
    broken.python = root / "missing-python";
    refused = false;
    try { (void)ExportDesignerPlugin(params, broken); } catch (const HandlerError& e) { refused = e.code() == -32011; }
    CHECK(refused);
    CHECK(!std::filesystem::exists(root / "failed_panel"));
    std::filesystem::remove_all(root);
    return CheckExit("test_designer_export");
} catch (const std::exception& error) {
    std::fprintf(stderr, "test_designer_export: unexpected exception: %s\n", error.what());
    return 1;
}
