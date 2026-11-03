// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/asset/browser.h"

#include <algorithm>
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

    std::vector<Entry> catalog{
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
    std::sort(catalog.begin(), catalog.end(), [](const Entry& left, const Entry& right) {
        return left.path < right.path;
    });

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
    CHECK(contents.directories.empty());
    CHECK(BuildBrowserContents(catalog, "content/ships", "MODELS", contents).ok());
    CHECK(contents.directories.size() == 1 && contents.directories[0].path == "content/ships/models");

    BrowserContents beforeFailure;
    CHECK(BuildBrowserContents(catalog, "content/ships", "", beforeFailure).ok());
    const std::string firstFileBefore = beforeFailure.files.front().path;
    CHECK(!BuildBrowserContents(catalog, "../outside", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);

    const std::vector<Entry> unsorted{{"z.obj", 1}, {"a.obj", 1}};
    CHECK(!BuildBrowserContents(unsorted, "", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);
    const std::vector<Entry> duplicate{{"a.obj", 1}, {"a.obj", 2}};
    CHECK(!BuildBrowserContents(duplicate, "", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);
    const std::vector<Entry> nonCanonical{{"Content/a.obj", 1}};
    CHECK(!BuildBrowserContents(nonCanonical, "", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);
    CHECK(!BuildBrowserContents(catalog, "Content/ships", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);
    const std::string invalidUtf8Path = std::string("bad\xff.obj", 8);
    const std::vector<Entry> invalidUtf8{{invalidUtf8Path, 1}};
    CHECK(!BuildBrowserContents(invalidUtf8, "", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);
    const std::string longSearch(scuba::asset::kMaxBrowserSearchLength + 1, 'x');
    CHECK(!BuildBrowserContents(catalog, "", longSearch, beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);

    std::vector<Entry> oversized(scuba::asset::kMaxBrowserCatalogEntries + 1, {"a.obj", 1});
    CHECK(!BuildBrowserContents(oversized, "", "", beforeFailure).ok());
    CHECK(beforeFailure.files.size() == 7 && beforeFailure.files.front().path == firstFileBefore);

    std::vector<Entry> manyFiles;
    manyFiles.reserve(scuba::asset::kMaxBrowserFiles + 7);
    for (size_t i = 0; i < scuba::asset::kMaxBrowserFiles + 7; ++i) {
        std::string number = std::to_string(i);
        number.insert(0, 5 - number.size(), '0');
        manyFiles.push_back({"large/file_" + number + ".obj", 1});
    }
    CHECK(BuildBrowserContents(manyFiles, "large", "", contents).ok());
    CHECK(contents.files.size() == scuba::asset::kMaxBrowserFiles);
    CHECK(contents.omittedFiles == 7 && contents.omittedDirectories == 0);
    CHECK(FindBrowserFile(contents, "large/file_00000.obj") == &contents.files.front());
    CHECK(FindBrowserFile(contents, "large/file_04102.obj") == nullptr);

    std::vector<Entry> manyFolders;
    manyFolders.reserve(scuba::asset::kMaxBrowserFolders + 7);
    for (size_t i = 0; i < scuba::asset::kMaxBrowserFolders + 7; ++i) {
        std::string number = std::to_string(i);
        number.insert(0, 5 - number.size(), '0');
        manyFolders.push_back({"large/folder_" + number + "/item.obj", 1});
    }
    CHECK(BuildBrowserContents(manyFolders, "large", "", contents).ok());
    CHECK(contents.directories.size() == scuba::asset::kMaxBrowserFolders);
    CHECK(contents.omittedDirectories == 7 && contents.files.empty());
    CHECK(BuildBrowserContents(manyFolders, "large", "folder_04102", contents).ok());
    CHECK(contents.directories.size() == 1 && contents.directories[0].path == "large/folder_04102");
    CHECK(contents.files.size() == 1 && contents.files[0].path == "large/folder_04102/item.obj");

    const std::vector<Entry> remountedCatalog{{"content/ships/new.glb", 1}};
    CHECK(BuildBrowserContents(remountedCatalog, "content/ships", "", contents).ok());
    CHECK(FindBrowserFile(contents, "content/ships/a.glb") == nullptr);
    CHECK(FindBrowserFile(contents, "content/ships/new.glb") == &contents.files.front());

    // Mount overlays can expose a file and a child path with the same prefix. Keep the visible
    // file selection keyed by its exact VFS path and expose the inferred folder separately.
    const std::vector<Entry> fileFolderPrefix{{"assets", 1}, {"assets/mesh.obj", 2}};
    CHECK(BuildBrowserContents(fileFolderPrefix, "", "", contents).ok());
    CHECK(contents.directories.size() == 1 && contents.directories[0].path == "assets");
    CHECK(contents.files.size() == 1 && contents.files[0].path == "assets");
    CHECK(FindBrowserFile(contents, "assets") == &contents.files[0]);

    CHECK(failures == 0);
    return failures == 0 ? 0 : 1;
}
