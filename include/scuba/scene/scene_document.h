// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scuba/core/status.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scuba::scene {

inline constexpr uint32_t kSceneSchemaVersion = 1;
inline constexpr size_t kMaxSceneNodes = 8192;
inline constexpr size_t kMaxSceneFileBytes = 8u * 1024u * 1024u;
inline constexpr size_t kMaxHierarchyDepth = 256;

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;

    friend bool operator==(const Vec3&, const Vec3&) = default;
};

struct Quaternion {
    float x = 0;
    float y = 0;
    float z = 0;
    float w = 1;

    friend bool operator==(const Quaternion&, const Quaternion&) = default;
};

struct LocalTransform {
    Vec3 translation;
    Quaternion rotation;
    Vec3 scale{1, 1, 1};

    friend bool operator==(const LocalTransform&, const LocalTransform&) = default;
};

enum class NodeKind : uint8_t { Group, ModelInstance };

struct Node {
    std::string id;  // Persistent lowercase ID, never an array index.
    std::string name;
    NodeKind kind = NodeKind::Group;
    std::optional<std::string> parentId;
    std::string assetPath;  // Normalized VFS-relative path for ModelInstance; empty for Group.
    LocalTransform localTransform;

    friend bool operator==(const Node&, const Node&) = default;
};

struct Document {
    uint32_t schemaVersion = kSceneSchemaVersion;
    std::string id;  // Persistent lowercase filename-safe ID.
    std::string name = "Untitled";
    std::vector<Node> nodes;

    friend bool operator==(const Document&, const Document&) = default;
};

// Scene asset paths use the engine VFS namespace. Project files are separate and are never
// written through the read-only VFS.
Status Validate(const Document& document);
Status Serialize(const Document& document, std::string& json);
Status Deserialize(std::string_view json, Document& document);

// Opens an existing, explicitly selected project directory. Scene files are direct children named
// <scene-id>.scene.json; IDs are restricted to safe filename characters. Save replaces a scene
// atomically, while Load rejects symlinked scene files and leaves its output unchanged on failure.
class ProjectStore {
public:
    static std::optional<ProjectStore> Open(const std::filesystem::path& projectRoot,
                                            std::span<const std::filesystem::path> readOnlyDirectoryRoots,
                                            Status* status = nullptr);

    Status Save(const Document& document) const;
    Status Load(std::string_view sceneId, Document& document) const;

private:
    ProjectStore(std::filesystem::path canonicalRoot, std::vector<std::filesystem::path> readOnlyRoots)
        : root_(std::move(canonicalRoot)), readOnlyRoots_(std::move(readOnlyRoots)) {}
    std::filesystem::path ScenePath(std::string_view sceneId) const;
    Status CheckRootSeparation() const;

    std::filesystem::path root_;
    std::vector<std::filesystem::path> readOnlyRoots_;
};

}  // namespace scuba::scene
