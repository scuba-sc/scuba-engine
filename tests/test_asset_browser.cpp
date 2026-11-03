// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/asset/browser.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; ++failures; \
} } while (false)

}  // namespace

int main() {
    using scuba::asset::BrowserContents;
    using scuba::asset::BrowserItemKind;
    using scuba::asset::BuildBrowserContents;
    using scuba::asset::FindBrowserFile;
    using scuba::vfs::Entry;

    const std::vector<Entry> catalog{
        {"content/ships/a.glb", 100},
        {"content/ships/readme.txt", 20},
        {"content/ships/models/b.obj", 60},
        {"content/ships/models/info.xml", 30},
        {"content/ships-extra/hidden.cgf", 40},
        {"content/textures/panel.dds", 50},
        {"content/ships/ship.gltf", 70},
        {"content/ships/ship.fbx", 80},
        {"content/ships/ship.cgf", 90},
        {"content/ships/ship.cga", 91},
        {"content/ships/ship.png", 92},
    };

    BrowserContents contents;
    CHECK(BuildBrowserContents(catalog, "", "", contents).ok());
    CHECK(contents.directories.size() == 1 && contents.directories[0].name == "content");
    CHECK(contents.files.empty());

    CHECK(BuildBrowserContents(catalog, "content", "", contents).ok());
    CHECK(contents.directories.size() == 3);
    CHECK(contents.directories[0].path == "content/ships");
    CHECK(contents.directories[1].path == "content/ships-extra");
    CHECK(contents.directories[2].path == "content/textures");
    CHECK(contents.files.empty());

    CHECK(BuildBrowserContents(catalog, "content/ships", "", contents).ok());
    CHECK(contents.directories.size() == 1 && contents.directories[0].path == "content/ships/models");
    CHECK(contents.files.size() == 7);
    const auto* glb = FindBrowserFile(contents, "content/ships/a.glb");
    const auto* gltf = FindBrowserFile(contents, "content/ships/ship.gltf");
    const auto* fbx = FindBrowserFile(contents, "content/ships/ship.fbx");
    const auto* cgf = FindBrowserFile(contents, "content/ships/ship.cgf");
    const auto* cga = FindBrowserFile(contents, "content/ships/ship.cga");
    const auto* png = FindBrowserFile(contents, "content/ships/ship.png");
    CHECK(glb && glb->supportedModelFormat());
    CHECK(gltf && gltf->supportedModelFormat());
    CHECK(fbx && fbx->supportedModelFormat());
    CHECK(cgf && cgf->supportedModelFormat());
    CHECK(cga && cga->supportedModelFormat());
    CHECK(png && png->kind == BrowserItemKind::File && !png->supportedModelFormat());
    CHECK(FindBrowserFile(contents, "content/ships/readme.txt") != nullptr);
    CHECK(FindBrowserFile(contents, "content/ships-extra/hidden.cgf") == nullptr);

    CHECK(BuildBrowserContents(catalog, "content/ships/models", "", contents).ok());
    CHECK(contents.directories.empty() && contents.files.size() == 2);
    CHECK(FindBrowserFile(contents, "content/ships/models/b.obj") == &contents.files[0]);
    CHECK(contents.files[0].supportedModelFormat());
    CHECK(!contents.files[1].supportedModelFormat());

    // Search is case-insensitive, recursive within the current VFS folder, and returns paths
    // relative to that folder while preserving the full path as the stable selection key.
    CHECK(BuildBrowserContents(catalog, "content/ships", "MoDeLs", contents).ok());
    CHECK(contents.files.size() == 2);
    CHECK(contents.files[0].name == "models/b.obj");
    CHECK(contents.files[1].name == "models/info.xml");
    CHECK(contents.files[0].path == "content/ships/models/b.obj" && contents.files[0].supportedModelFormat());
    CHECK(!contents.files[1].supportedModelFormat());
    CHECK(FindBrowserFile(contents, "content/ships/models/b.obj") == &contents.files[0]);
    CHECK(FindBrowserFile(contents, "content/ships/models/missing.obj") == nullptr);

    // A prefix must end at a path boundary: searching/listing "ships" cannot leak in "ships-extra".
    CHECK(BuildBrowserContents(catalog, "content/ships", "hidden", contents).ok());
    CHECK(contents.files.empty());
    CHECK(contents.directories.size() == 1 && contents.directories[0].path == "content/ships/models");

    BrowserContents beforeFailure;
    CHECK(BuildBrowserContents(catalog, "content/ships", "", beforeFailure).ok());
    const std::string firstFileBefore = beforeFailure.files.front().path;
    CHECK(!BuildBrowserContents(catalog, "../outside", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);

    CHECK(failures == 0);
    return failures == 0 ? 0 : 1;
}
