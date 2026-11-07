// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"

#include "scuba/scene/scene_document.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace scuba;

namespace {

scene::Document MakeDocument() {
    scene::Document document;
    document.id = "outliner-test";
    document.name = "Outliner test";

    scene::Node root;
    root.id = "root";
    root.name = "Workspace";
    root.kind = scene::NodeKind::Group;

    scene::Node vehicles;
    vehicles.id = "vehicles";
    vehicles.name = "Vehicles";
    vehicles.kind = scene::NodeKind::Group;
    vehicles.parentId = root.id;

    scene::Node submarine;
    submarine.id = "submarine";
    submarine.name = "Abyss";
    submarine.kind = scene::NodeKind::ModelInstance;
    submarine.parentId = vehicles.id;
    submarine.assetPath = "models/submarine.obj";

    scene::Node props;
    props.id = "props";
    props.name = "Props";
    props.kind = scene::NodeKind::Group;
    props.parentId = root.id;

    scene::Node crate;
    crate.id = "crate";
    crate.name = "Crate";
    crate.kind = scene::NodeKind::ModelInstance;
    crate.parentId = props.id;
    crate.assetPath = "models/crate.obj";

    document.nodes = {root, vehicles, submarine, props, crate};
    return document;
}

void TestEmptyAndHierarchySearch() {
    auto document = MakeDocument();
    const auto original = document;
    scene::OutlinerSearchResult result;

    CHECK(scene::SearchOutliner(document, "", result).ok());
    CHECK(result.visibleNodeIds == std::vector<std::string>({"root", "vehicles", "submarine", "props", "crate"}));
    CHECK(result.matchingNodeIds.empty());

    CHECK(scene::SearchOutliner(document, "SUBMARINE.OBJ", result).ok());
    CHECK(result.visibleNodeIds == std::vector<std::string>({"root", "vehicles", "submarine"}));
    CHECK(result.matchingNodeIds == std::vector<std::string>({"submarine"}));

    CHECK(scene::SearchOutliner(document, "VEHICLES", result).ok());
    CHECK(result.visibleNodeIds == std::vector<std::string>({"root", "vehicles"}));
    CHECK(result.matchingNodeIds == std::vector<std::string>({"vehicles"}));

    CHECK(scene::SearchOutliner(document, "crate.obj", result).ok());
    CHECK(result.visibleNodeIds == std::vector<std::string>({"root", "props", "crate"}));
    CHECK(result.matchingNodeIds == std::vector<std::string>({"crate"}));

    CHECK(scene::SearchOutliner(document, "no-such-node", result).ok());
    CHECK(result.visibleNodeIds.empty());
    CHECK(result.matchingNodeIds.empty());
    CHECK(document == original);
}

void TestStableIdsAfterReordering() {
    auto document = MakeDocument();
    std::reverse(document.nodes.begin(), document.nodes.end());
    scene::OutlinerSearchResult result;
    CHECK(scene::SearchOutliner(document, "submarine", result).ok());
    CHECK(result.matchingNodeIds == std::vector<std::string>({"submarine"}));
    CHECK(result.visibleNodeIds == std::vector<std::string>({"submarine", "vehicles", "root"}));
    CHECK(scene::Validate(document).ok());
}

void TestInvalidQueriesAreTransactional() {
    auto document = MakeDocument();
    const auto original = document;
    scene::OutlinerSearchResult result{{"kept-visible"}, {"kept-match"}};
    const auto originalResult = result;
    const std::vector<std::string> invalidQueries = {
        std::string(scene::kMaxOutlinerSearchQueryBytes + 1, 'x'),
        std::string("broken\xc3", 7),
        std::string("bad\x01query", 9),
    };

    for (const auto& query : invalidQueries) {
        CHECK(!scene::SearchOutliner(document, query, result).ok());
        CHECK(result == originalResult);
        CHECK(document == original);
    }

    auto invalidDocument = document;
    invalidDocument.nodes[2].parentId = "missing-parent";
    CHECK(!scene::SearchOutliner(invalidDocument, "abyss", result).ok());
    CHECK(result == originalResult);
    CHECK(document == original);
}

}  // namespace

int main() {
    TestEmptyAndHierarchySearch();
    TestStableIdsAfterReordering();
    TestInvalidQueriesAreTransactional();
    return CheckExit("scene_outliner_search");
}
