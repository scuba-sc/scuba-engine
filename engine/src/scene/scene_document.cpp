// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/scene/scene_document.h"

#include "hostd/json.h"
#include "scuba/vfs/file_system.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace scuba::scene {
namespace {

using hostd::Json;

constexpr size_t kMaxSceneIdBytes = 64;
constexpr size_t kMaxNameBytes = 128;
constexpr size_t kMaxAssetPathBytes = 512;
constexpr float kMaxTranslation = 10000000.0f;
constexpr float kMinScale = 0.000001f;
constexpr float kMaxScale = 1000000.0f;
constexpr float kQuaternionTolerance = 0.001f;

Status Fail(std::string why) { return Status::Fail(std::move(why)); }

bool ValidIdentifier(std::string_view id) {
    if (id.empty() || id.size() > kMaxSceneIdBytes) return false;
    const auto alphaNumeric = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alphaNumeric(static_cast<unsigned char>(id.front()))) return false;
    for (const char raw : id) {
        const auto c = static_cast<unsigned char>(raw);
        if (!alphaNumeric(c) && c != '-' && c != '_') return false;
    }
    if (id == "con" || id == "prn" || id == "aux" || id == "nul") return false;
    if (id.size() == 4 && (id.substr(0, 3) == "com" || id.substr(0, 3) == "lpt") &&
        id[3] >= '1' && id[3] <= '9') return false;
    return true;
}

bool ValidLabel(std::string_view value, size_t maxBytes) {
    if (value.empty() || value.size() > maxBytes) return false;
    for (size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7f) return false;
            ++i;
            continue;
        }
        size_t count = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if ((lead & 0xe0u) == 0xc0u) { count = 2; codepoint = lead & 0x1fu; minimum = 0x80; }
        else if ((lead & 0xf0u) == 0xe0u) { count = 3; codepoint = lead & 0x0fu; minimum = 0x800; }
        else if ((lead & 0xf8u) == 0xf0u) { count = 4; codepoint = lead & 0x07u; minimum = 0x10000; }
        else return false;
        if (i + count > value.size()) return false;
        for (size_t j = 1; j < count; ++j) {
            const auto continuation = static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            (codepoint >= 0x80u && codepoint <= 0x9fu)) return false;
        i += count;
    }
    return true;
}

bool FiniteBounded(float value, float bound) {
    return std::isfinite(value) && std::fabs(value) <= bound;
}

bool IsWithin(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto c = candidate.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++c) {
        if (c == candidate.end()) return false;
#ifdef _WIN32
        const auto& candidatePart = c->native();
        const auto& rootPart = r->native();
        if (CompareStringOrdinal(candidatePart.c_str(), static_cast<int>(candidatePart.size()),
                                 rootPart.c_str(), static_cast<int>(rootPart.size()), TRUE) != CSTR_EQUAL)
            return false;
#else
        if (*c != *r) return false;
#endif
    }
    return true;
}

bool RootsOverlap(const std::filesystem::path& a, const std::filesystem::path& b) {
    return IsWithin(a, b) || IsWithin(b, a);
}

Json Vec3Json(const Vec3& v) {
    Json out = Json::MakeArray();
    out.push_back(Json(v.x)).push_back(Json(v.y)).push_back(Json(v.z));
    return out;
}

Json QuaternionJson(const Quaternion& q) {
    Json out = Json::MakeArray();
    out.push_back(Json(q.x)).push_back(Json(q.y)).push_back(Json(q.z)).push_back(Json(q.w));
    return out;
}

Json TransformJson(const LocalTransform& transform) {
    Json out = Json::MakeObject();
    out.set("translation", Vec3Json(transform.translation));
    out.set("rotation", QuaternionJson(transform.rotation));
    out.set("scale", Vec3Json(transform.scale));
    return out;
}

bool ExactKeys(const Json& value, std::initializer_list<std::string_view> expected) {
    if (!value.is_object() || value.keys().size() != expected.size()) return false;
    for (const auto key : expected)
        if (!value.find(key)) return false;
    return true;
}

bool ReadString(const Json* value, size_t maxBytes, std::string& out) {
    if (!value || !value->is_string() || value->as_string().size() > maxBytes) return false;
    out = value->as_string();
    return true;
}

bool ReadFloat(const Json& value, float& out) {
    if (!value.is_number()) return false;
    const double number = value.as_number();
    if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<float>::max()) return false;
    out = static_cast<float>(number);
    return std::isfinite(out);
}

bool ReadVec3(const Json* value, Vec3& out) {
    if (!value || !value->is_array() || value->size() != 3) return false;
    return ReadFloat(value->at(0), out.x) && ReadFloat(value->at(1), out.y) && ReadFloat(value->at(2), out.z);
}

bool ReadQuaternion(const Json* value, Quaternion& out) {
    if (!value || !value->is_array() || value->size() != 4) return false;
    return ReadFloat(value->at(0), out.x) && ReadFloat(value->at(1), out.y) &&
           ReadFloat(value->at(2), out.z) && ReadFloat(value->at(3), out.w);
}

bool ReadTransform(const Json* value, LocalTransform& out) {
    if (!ExactKeys(value ? *value : Json(), {"translation", "rotation", "scale"})) return false;
    return ReadVec3(value->find("translation"), out.translation) &&
           ReadQuaternion(value->find("rotation"), out.rotation) &&
           ReadVec3(value->find("scale"), out.scale);
}

bool ReadNode(const Json& value, Node& out) {
    if (!ExactKeys(value, {"id", "name", "kind", "parent", "asset", "transform"})) return false;
    if (!ReadString(value.find("id"), kMaxSceneIdBytes, out.id) ||
        !ReadString(value.find("name"), kMaxNameBytes, out.name)) return false;

    const Json* kind = value.find("kind");
    if (!kind || !kind->is_string()) return false;
    if (kind->as_string() == "group") out.kind = NodeKind::Group;
    else if (kind->as_string() == "model") out.kind = NodeKind::ModelInstance;
    else return false;

    const Json* parent = value.find("parent");
    if (!parent) return false;
    if (parent->is_string()) {
        std::string parentId;
        if (!ReadString(parent, kMaxSceneIdBytes, parentId)) return false;
        out.parentId = std::move(parentId);
    } else if (!parent->is_null()) {
        return false;
    }

    const Json* asset = value.find("asset");
    if (!asset) return false;
    if (asset->is_string()) {
        if (!ReadString(asset, kMaxAssetPathBytes, out.assetPath)) return false;
    } else if (!asset->is_null()) {
        return false;
    }
    return ReadTransform(value.find("transform"), out.localTransform);
}

Status AtomicReplace(const std::filesystem::path& root, const std::filesystem::path& target,
                     std::string_view contents) {
    static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
    const auto processId = static_cast<uint64_t>(GetCurrentProcessId());
#else
    const auto processId = static_cast<uint64_t>(getpid());
#endif
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto temporary = root / (target.filename().string() + ".tmp-" +
                                       std::to_string(processId) + "-" + std::to_string(serial));
#ifdef _WIN32
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return Fail("cannot create temporary scene file (Windows error " + std::to_string(error) + ")");
        }
        size_t offset = 0;
        bool writeOk = true;
        while (offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - offset, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) || written == 0) {
                writeOk = false;
                break;
            }
            offset += written;
        }
        if (writeOk && !FlushFileBuffers(file)) writeOk = false;
        const bool closeOk = CloseHandle(file) != 0;
        if (!writeOk || !closeOk) {
            DeleteFileW(temporary.c_str());
            return Fail("cannot write temporary scene file");
        }
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(temporary.c_str());
            return Fail("cannot atomically replace scene file (Windows error " + std::to_string(error) + ")");
        }
#else
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return Fail("cannot create temporary scene file: " +
                        std::error_code(errno, std::generic_category()).message());
        }
        size_t offset = 0;
        bool writeOk = true;
        int savedError = 0;
        while (offset < contents.size()) {
            const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                writeOk = false;
                savedError = written < 0 ? errno : EIO;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (writeOk && ::fsync(fd) != 0) {
            writeOk = false;
            savedError = errno;
        }
        const int closeResult = ::close(fd);
        if (closeResult != 0 && savedError == 0) savedError = errno;
        if (!writeOk || closeResult != 0) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return Fail("cannot write temporary scene file: " +
                        std::error_code(savedError, std::generic_category()).message());
        }
        if (::rename(temporary.c_str(), target.c_str()) != 0) {
            const int error = errno;
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return Fail("cannot atomically replace scene file: " +
                        std::error_code(error, std::generic_category()).message());
        }
#endif
        return Status::Ok();
    }
    return Fail("could not allocate a unique temporary scene file name");
}

}  // namespace

Status Validate(const Document& document) {
    if (document.schemaVersion != kSceneSchemaVersion) return Fail("unsupported scene schema version");
    if (!ValidIdentifier(document.id)) return Fail("scene id must be a safe identifier of at most 64 ASCII characters");
    if (!ValidLabel(document.name, kMaxNameBytes))
        return Fail("scene name is empty, too long, or contains a control character");
    if (document.nodes.size() > kMaxSceneNodes) return Fail("scene exceeds the node limit");

    std::unordered_map<std::string, const Node*> byId;
    byId.reserve(document.nodes.size());
    for (const auto& node : document.nodes) {
        if (!ValidIdentifier(node.id)) return Fail("node id must be a safe identifier of at most 64 ASCII characters");
        if (!ValidLabel(node.name, kMaxNameBytes))
            return Fail("node name is empty, too long, or contains a control character");
        if (!byId.emplace(node.id, &node).second) return Fail("duplicate node id: " + node.id);

        const auto& t = node.localTransform;
        if (!FiniteBounded(t.translation.x, kMaxTranslation) ||
            !FiniteBounded(t.translation.y, kMaxTranslation) ||
            !FiniteBounded(t.translation.z, kMaxTranslation))
            return Fail("node translation is not finite or exceeds the scene limit");
        const float qNorm = t.rotation.x * t.rotation.x + t.rotation.y * t.rotation.y +
                            t.rotation.z * t.rotation.z + t.rotation.w * t.rotation.w;
        if (!FiniteBounded(t.rotation.x, 1.0f) || !FiniteBounded(t.rotation.y, 1.0f) ||
            !FiniteBounded(t.rotation.z, 1.0f) || !FiniteBounded(t.rotation.w, 1.0f) ||
            !std::isfinite(qNorm) || std::fabs(qNorm - 1.0f) > kQuaternionTolerance)
            return Fail("node rotation must be a finite normalized quaternion");
        for (const float scale : {t.scale.x, t.scale.y, t.scale.z})
            if (!std::isfinite(scale) || scale < kMinScale || scale > kMaxScale)
                return Fail("node scale must be finite, positive, and within the scene limits");

        if (node.kind == NodeKind::Group) {
            if (!node.assetPath.empty()) return Fail("group nodes cannot reference an asset");
        } else if (node.kind == NodeKind::ModelInstance) {
            const auto normalized = vfs::NormalizePath(node.assetPath);
            if (node.assetPath.empty() || node.assetPath.size() > kMaxAssetPathBytes ||
                !ValidLabel(node.assetPath, kMaxAssetPathBytes) ||
                normalized.empty() || normalized != node.assetPath)
                return Fail("model asset path must be a normalized VFS-relative path");
        } else {
            return Fail("unknown scene node kind");
        }
    }

    for (const auto& node : document.nodes) {
        if (!node.parentId) continue;
        const auto parent = byId.find(*node.parentId);
        if (parent == byId.end()) return Fail("node references a missing parent: " + *node.parentId);
        if (parent->second->kind != NodeKind::Group) return Fail("only group nodes may contain children");
    }

    std::unordered_map<std::string, uint8_t> color;
    color.reserve(document.nodes.size());
    for (const auto& start : document.nodes) {
        std::vector<const Node*> path;
        const Node* current = &start;
        while (current) {
            const auto found = color.find(current->id);
            if (found != color.end()) {
                if (found->second == 1) return Fail("scene hierarchy contains a cycle");
                break;
            }
            color.emplace(current->id, 1);
            path.push_back(current);
            if (path.size() > kMaxHierarchyDepth) return Fail("scene hierarchy exceeds the depth limit");
            current = current->parentId ? byId.at(*current->parentId) : nullptr;
        }
        for (const Node* node : path) color[node->id] = 2;
    }
    return Status::Ok();
}

Status Serialize(const Document& document, std::string& json) {
    if (auto status = Validate(document); !status) return status;
    Json root = Json::MakeObject();
    root.set("format", "scuba.scene");
    root.set("schema_version", Json(document.schemaVersion));
    root.set("scene_id", document.id);
    root.set("name", document.name);
    Json nodes = Json::MakeArray();
    for (const auto& node : document.nodes) {
        Json value = Json::MakeObject();
        value.set("id", node.id);
        value.set("name", node.name);
        value.set("kind", node.kind == NodeKind::Group ? "group" : "model");
        value.set("parent", node.parentId ? Json(*node.parentId) : Json());
        value.set("asset", node.assetPath.empty() ? Json() : Json(node.assetPath));
        value.set("transform", TransformJson(node.localTransform));
        nodes.push_back(std::move(value));
    }
    root.set("nodes", std::move(nodes));
    std::string encoded = root.dump();
    if (encoded.size() > kMaxSceneFileBytes) return Fail("serialized scene exceeds the file size limit");
    json = std::move(encoded);
    return Status::Ok();
}

Status Deserialize(std::string_view json, Document& document) {
    if (json.size() > kMaxSceneFileBytes) return Fail("scene file exceeds the size limit");
    std::string parseError;
    const auto parsed = Json::parse(json, &parseError, true);
    if (!parsed) return Fail("invalid scene JSON: " + parseError);
    const Json& root = *parsed;
    if (!ExactKeys(root, {"format", "schema_version", "scene_id", "name", "nodes"}))
        return Fail("scene document has missing or unknown top-level fields");

    const Json* format = root.find("format");
    if (!format || !format->is_string() || format->as_string() != "scuba.scene")
        return Fail("unrecognized scene document format");
    const Json* version = root.find("schema_version");
    if (!version || !version->is_number()) return Fail("scene schema version is missing or invalid");
    const double rawVersion = version->as_number();
    if (!std::isfinite(rawVersion) || rawVersion != static_cast<double>(kSceneSchemaVersion))
        return Fail("unsupported scene schema version");

    Document decoded;
    decoded.schemaVersion = kSceneSchemaVersion;
    if (!ReadString(root.find("scene_id"), kMaxSceneIdBytes, decoded.id) ||
        !ReadString(root.find("name"), kMaxNameBytes, decoded.name))
        return Fail("scene id or name is missing or invalid");
    const Json* nodes = root.find("nodes");
    if (!nodes || !nodes->is_array() || nodes->size() > kMaxSceneNodes)
        return Fail("scene node list is invalid or exceeds the node limit");
    decoded.nodes.reserve(nodes->size());
    for (size_t i = 0; i < nodes->size(); ++i) {
        Node node;
        if (!ReadNode(nodes->at(i), node)) return Fail("scene node " + std::to_string(i) + " has invalid fields");
        decoded.nodes.push_back(std::move(node));
    }
    if (auto status = Validate(decoded); !status) return status;
    document = std::move(decoded);
    return Status::Ok();
}

Status ResolveWorldTransforms(const Document& document, std::vector<ResolvedTransform>& transforms) {
    if (auto status = Validate(document); !status) return status;
    std::unordered_map<std::string, size_t> indexById;
    indexById.reserve(document.nodes.size());
    std::vector<ResolvedTransform> resolved;
    resolved.reserve(document.nodes.size());
    for (size_t i = 0; i < document.nodes.size(); ++i) {
        indexById.emplace(document.nodes[i].id, i);
        resolved.push_back({document.nodes[i].id, {}});
    }

    std::vector<uint8_t> state(document.nodes.size(), 0);
    for (size_t start = 0; start < document.nodes.size(); ++start) {
        std::vector<size_t> chain;
        std::optional<size_t> cursor = start;
        while (cursor && state[*cursor] != 2) {
            if (state[*cursor] == 1) return Fail("scene hierarchy contains a cycle");
            state[*cursor] = 1;
            chain.push_back(*cursor);
            const auto& parentId = document.nodes[*cursor].parentId;
            cursor = parentId ? std::optional<size_t>(indexById.at(*parentId)) : std::nullopt;
        }
        while (!chain.empty()) {
            const size_t nodeIndex = chain.back();
            chain.pop_back();
            const auto& node = document.nodes[nodeIndex];
            const auto& local = node.localTransform;
            render::Float4x4 localMatrix;
            if (!render::AffineFromTrs({local.translation.x, local.translation.y, local.translation.z},
                                       {local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w},
                                       {local.scale.x, local.scale.y, local.scale.z}, localMatrix))
                return Fail("node local transform cannot be represented as a finite matrix: " + node.id);
            render::Float4x4 world = localMatrix;
            if (node.parentId) world = render::Multiply(localMatrix, resolved[indexById.at(*node.parentId)].world);
            render::Float4x4 inverse;
            if (!render::InvertAffine(world, inverse))
                return Fail("node world transform is singular or numerically unstable: " + node.id);
            resolved[nodeIndex].world = world;
            state[nodeIndex] = 2;
        }
    }
    transforms = std::move(resolved);
    return Status::Ok();
}

std::optional<ProjectStore> ProjectStore::Open(
    const std::filesystem::path& projectRoot,
    std::span<const std::filesystem::path> readOnlyDirectoryRoots,
    Status* status) {
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(projectRoot, ec);
    if (ec) {
        if (status) *status = Fail("project directory cannot be resolved: " + ec.message());
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(canonical, ec) || ec) {
        if (status) *status = Fail("project root must be an existing directory");
        return std::nullopt;
    }
    std::vector<std::filesystem::path> canonicalReadOnlyRoots;
    canonicalReadOnlyRoots.reserve(readOnlyDirectoryRoots.size());
    for (const auto& readOnlyRoot : readOnlyDirectoryRoots) {
        const auto readOnlyCanonical = std::filesystem::canonical(readOnlyRoot, ec);
        if (ec || !std::filesystem::is_directory(readOnlyCanonical, ec) || ec) {
            if (status) *status = Fail("read-only asset root must be an existing directory");
            return std::nullopt;
        }
        if (RootsOverlap(canonical, readOnlyCanonical)) {
            if (status) *status = Fail("project directory overlaps a read-only asset directory");
            return std::nullopt;
        }
        canonicalReadOnlyRoots.push_back(readOnlyCanonical);
    }
    if (status) *status = Status::Ok();
    return ProjectStore(canonical, std::move(canonicalReadOnlyRoots));
}

std::filesystem::path ProjectStore::ScenePath(std::string_view sceneId) const {
    return root_ / (std::string(sceneId) + ".scene.json");
}

Status ProjectStore::CheckRootSeparation() const {
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::canonical(root_, ec);
    if (ec || canonicalRoot != root_ || !std::filesystem::is_directory(canonicalRoot, ec) || ec)
        return Fail("project directory changed or is no longer available");
    for (const auto& readOnlyRoot : readOnlyRoots_) {
        const auto canonicalReadOnly = std::filesystem::canonical(readOnlyRoot, ec);
        if (ec || canonicalReadOnly != readOnlyRoot || !std::filesystem::is_directory(canonicalReadOnly, ec) || ec)
            return Fail("read-only asset directory changed or is no longer available");
        if (RootsOverlap(canonicalRoot, canonicalReadOnly))
            return Fail("project directory overlaps a read-only asset directory");
    }
    return Status::Ok();
}

Status ProjectStore::Save(const Document& document) const {
    if (auto status = CheckRootSeparation(); !status) return status;
    if (auto status = Validate(document); !status) return status;
    std::string json;
    if (auto status = Serialize(document, json); !status) return status;
    return AtomicReplace(root_, ScenePath(document.id), json);
}

Status ProjectStore::Load(std::string_view sceneId, Document& document) const {
    if (auto status = CheckRootSeparation(); !status) return status;
    if (!ValidIdentifier(sceneId)) return Fail("scene id must be a safe identifier of at most 64 ASCII characters");
    const auto path = ScenePath(sceneId);
    std::error_code ec;
    const auto linkStatus = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(linkStatus) || std::filesystem::is_symlink(linkStatus))
        return Fail("scene file is missing or is not a regular project file");
    const auto canonical = std::filesystem::canonical(path, ec);
    if (ec || canonical.parent_path() != root_ || canonical.filename() != path.filename())
        return Fail("scene file resolves outside the project directory");
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxSceneFileBytes) return Fail("scene file is missing or exceeds the size limit");

    std::ifstream input(path, std::ios::binary);
    if (!input) return Fail("cannot open scene file");
    std::string contents(static_cast<size_t>(size), '\0');
    if (size != 0) input.read(contents.data(), static_cast<std::streamsize>(size));
    if ((!input && !input.eof()) || static_cast<uint64_t>(input.gcount()) != size)
        return Fail("scene file changed or could not be read completely");
    if (input.peek() != std::char_traits<char>::eof()) return Fail("scene file changed while it was being read");

    Document decoded;
    if (auto status = Deserialize(contents, decoded); !status) return status;
    if (decoded.id != sceneId) return Fail("scene file id does not match its project filename");
    document = std::move(decoded);
    return Status::Ok();
}

}  // namespace scuba::scene
