// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scuba/core/status.h"
#include "scuba/render/camera.h"

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
inline constexpr float kMaxRotationDegrees = 36000.0f;

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

struct ResolvedTransform {
    std::string nodeId;
    render::Float4x4 world;
};

// Scene asset paths use the engine VFS namespace. Project files are separate and are never
// written through the read-only VFS.
Status Validate(const Document& document);
Status Serialize(const Document& document, std::string& json);
Status Deserialize(std::string_view json, Document& document);
// Resolves row-vector local transforms in document order, composing child-local * parent-world.
// Returns failure without changing the output if composition overflows or becomes singular.
Status ResolveWorldTransforms(const Document& document, std::vector<ResolvedTransform>& transforms);
// Intrinsic local XYZ Euler angles in degrees, applied X then Y then Z. Conversion is
// transactional and rejects non-finite input; quaternion-to-Euler returns the canonical range.
Status EulerXYZDegreesToQuaternion(Vec3 degrees, Quaternion& rotation);
Status QuaternionToEulerXYZDegrees(Quaternion rotation, Vec3& degrees);
// Multiplies all local scale axes by one positive factor, preserving their ratios. Refuses invalid
// or out-of-range results without changing the transform.
Status ScaleUniform(LocalTransform& transform, float factor);
// Places a unit-rotation/unit-scale model so its transformed bounds rest on world Z=0 at the
// requested world XY. An optional resolved parent transform is respected; outputs remain unchanged
// on failure. The caller validates that the parent is an allowed group node.
Status ComputeGroundPlacement(std::optional<render::Float4x4> parentWorld, render::Float3 worldPoint,
                              const std::array<float, 3>& modelBoundsMin,
                              const std::array<float, 3>& modelBoundsMax, LocalTransform& local,
                              render::Float4x4& world);
// Transactional editor mutations. Appending requires a unique stable ID and a group parent (if any);
// removing a group removes its descendants. The document is unchanged on failure.
Status AddGroup(Document& document, std::string id, std::string name,
                std::optional<std::string> parentId = std::nullopt,
                LocalTransform transform = {});
Status AddModelInstance(Document& document, std::string id, std::string name, std::string assetPath,
                        std::optional<std::string> parentId = std::nullopt,
                        LocalTransform transform = {});
Status RemoveSubtree(Document& document, std::string_view rootId);
Status RenameNode(Document& document, std::string_view nodeId, std::string name);
// Reparents a node only under another group (or to the root). With preserveWorldTransform=true,
// the operation refuses a result that cannot be represented by a non-sheared local TRS.
Status ReparentNode(Document& document, std::string_view nodeId,
                    std::optional<std::string> parentId, bool preserveWorldTransform = true);

// Opens an existing, explicitly selected project directory. Scene files are direct children named
// <scene-id>.scene.json; IDs are restricted to safe filename characters. Save replaces a scene
// atomically, while Load rejects symlinked scene files and leaves its output unchanged on failure.
class ProjectStore {
public:
    static std::optional<ProjectStore> Open(const std::filesystem::path& projectRoot,
                                            std::span<const std::filesystem::path> readOnlyDirectoryRoots,
                                            Status* status = nullptr);

    Status ListScenes(std::vector<std::string>& sceneIds) const;
    // Creates a new file atomically and refuses to replace an existing scene.
    Status Create(const Document& document) const;
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

// Platform-neutral state for the editor's saved/unsaved indicator. A failed update remains dirty;
// a failed baseline encoding clears the old baseline and remains dirty, matching editor behavior.
class DocumentDirtyState {
public:
    Status SetBaseline(const Document& document, bool exists);
    Status Update(const Document& document);
    bool dirty() const noexcept { return dirty_; }

private:
    std::optional<std::string> baseline_;
    bool dirty_ = false;
};

enum class SceneActionKind : uint8_t { Open, Create, Quit };
enum class SceneActionChoice : uint8_t { Save, Discard, Cancel };

struct SceneAction {
    SceneActionKind kind = SceneActionKind::Open;
    std::string sceneId;

    friend bool operator==(const SceneAction&, const SceneAction&) = default;
};

// A small decision gate used by the editor before switching scenes or quitting. Failed saves
// leave the action pending; discard commits it; cancel clears it.
class SceneActionGate {
public:
    std::optional<SceneAction> Request(SceneAction action, bool dirty);
    std::optional<SceneAction> Resolve(SceneActionChoice choice, bool saveSucceeded = false);
    const std::optional<SceneAction>& pending() const noexcept { return pending_; }

private:
    std::optional<SceneAction> pending_;
};

// Bounded snapshot history for editor scene mutations. Snapshots use the validated on-disk
// representation so a history entry cannot exceed the scene file limit.
class DocumentHistory {
public:
    static constexpr size_t kMaxEntries = 64;
    static constexpr size_t kMaxBytes = 32u * 1024u * 1024u;

    Status RecordBefore(const Document& document);
    Status Undo(const Document& current, Document& restored);
    Status Redo(const Document& current, Document& restored);
    // Removes the most recent captured edit only when its before-snapshot matches the expected
    // document. Used to roll back an explicitly canceled interactive transaction.
    Status CancelLatestChange(const Document& expectedBefore, Document& restored);
    void Clear() noexcept;

    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }
    size_t retainedEntries() const noexcept { return undo_.size() + redo_.size(); }
    size_t retainedBytes() const noexcept { return retainedBytes_; }

private:
    void Trim() noexcept;
    void DropOldest(std::vector<std::string>& stack) noexcept;

    std::vector<std::string> undo_;
    std::vector<std::string> redo_;
    size_t retainedBytes_ = 0;
};

}  // namespace scuba::scene
