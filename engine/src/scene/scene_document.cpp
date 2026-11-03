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
#include <unordered_set>

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

std::string DuplicateNodeName(std::string_view name) {
    constexpr std::string_view suffix = " Copy";
    if (name.size() + suffix.size() > kMaxNameBytes) return std::string(name);
    return std::string(name) + std::string(suffix);
}

bool FiniteBounded(float value, float bound) {
    return std::isfinite(value) && std::fabs(value) <= bound;
}

bool IsWithin(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto c = candidate.begin();
#ifdef _WIN32
    std::filesystem::path candidatePrefix;
    std::filesystem::path rootPrefix;
#endif
    for (auto r = root.begin(); r != root.end(); ++r, ++c) {
        if (c == candidate.end()) return false;
#ifdef _WIN32
        const auto& candidatePart = c->native();
        const auto& rootPart = r->native();
        candidatePrefix /= *c;
        rootPrefix /= *r;
        if (candidatePart != rootPart) {
            if (CompareStringOrdinal(candidatePart.c_str(), static_cast<int>(candidatePart.size()),
                                     rootPart.c_str(), static_cast<int>(rootPart.size()), TRUE) != CSTR_EQUAL)
                return false;
            // Drive/volume/UNC root components do not name complete filesystem objects on their
            // own. Their case-folded spelling is the same root namespace; defer object identity
            // checks until a relative component has formed a resolvable path prefix.
            if (!candidatePrefix.has_relative_path()) continue;
            // Windows can enable case-sensitive lookup on an individual directory. A folded
            // string match is an overlap only if those existing path prefixes identify the same
            // filesystem object. Unknown identity fails closed so an IO error cannot green-light
            // a potentially overlapping project and mount.
            std::error_code ec;
            const bool sameObject = std::filesystem::equivalent(candidatePrefix, rootPrefix, ec);
            if (ec) return true;
            if (!sameObject) return false;
        }
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

Status AtomicWrite(const std::filesystem::path& root, const std::filesystem::path& target,
                   std::string_view contents, bool replace) {
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
        DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
        if (replace) moveFlags |= MOVEFILE_REPLACE_EXISTING;
        if (!MoveFileExW(temporary.c_str(), target.c_str(), moveFlags)) {
            const DWORD error = GetLastError();
            DeleteFileW(temporary.c_str());
            if (!replace && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS))
                return Fail("scene file already exists");
            return Fail("cannot atomically " + std::string(replace ? "replace" : "create") +
                        " scene file (Windows error " + std::to_string(error) + ")");
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
        const int writeResult = replace ? ::rename(temporary.c_str(), target.c_str())
                                        : ::link(temporary.c_str(), target.c_str());
        if (writeResult != 0) {
            const int error = errno;
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            if (!replace && error == EEXIST) return Fail("scene file already exists");
            return Fail("cannot atomically " + std::string(replace ? "replace" : "create") + " scene file: " +
                        std::error_code(error, std::generic_category()).message());
        }
        if (!replace) {
            // The completed temporary inode is now visible under its final name. A failed cleanup
            // only leaves a harmless extra temp entry; the new scene is already committed.
            ::unlink(temporary.c_str());
        }
#endif
        return Status::Ok();
    }
    return Fail("could not allocate a unique temporary scene file name");
}

Status LocalTransformFromAffine(const render::Float4x4& matrix, LocalTransform& transform) {
    constexpr double kOrthogonalityTolerance = 5e-4;
    constexpr double kMatrixTolerance = 8e-4;
    for (const auto& row : matrix.m)
        for (const float value : row)
            if (!std::isfinite(value)) return Fail("reparent transform contains a non-finite value");
    if (std::fabs(matrix.m[0][3]) > 1e-5f || std::fabs(matrix.m[1][3]) > 1e-5f ||
        std::fabs(matrix.m[2][3]) > 1e-5f || std::fabs(matrix.m[3][3] - 1.0f) > 1e-5f)
        return Fail("reparent transform is not affine");

    double rotationRows[3][3]{};
    double scales[3]{};
    for (size_t row = 0; row < 3; ++row) {
        const double x = matrix.m[row][0], y = matrix.m[row][1], z = matrix.m[row][2];
        scales[row] = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(scales[row]) || scales[row] <= 1e-12)
            return Fail("reparent transform has a singular scale");
        rotationRows[row][0] = x / scales[row];
        rotationRows[row][1] = y / scales[row];
        rotationRows[row][2] = z / scales[row];
    }
    for (size_t a = 0; a < 3; ++a)
        for (size_t b = a + 1; b < 3; ++b) {
            const double dot = rotationRows[a][0] * rotationRows[b][0] +
                               rotationRows[a][1] * rotationRows[b][1] +
                               rotationRows[a][2] * rotationRows[b][2];
            if (std::fabs(dot) > kOrthogonalityTolerance)
                return Fail("reparent would require shear that local TRS cannot represent");
        }
    const double determinant =
        rotationRows[0][0] * (rotationRows[1][1] * rotationRows[2][2] - rotationRows[1][2] * rotationRows[2][1]) -
        rotationRows[0][1] * (rotationRows[1][0] * rotationRows[2][2] - rotationRows[1][2] * rotationRows[2][0]) +
        rotationRows[0][2] * (rotationRows[1][0] * rotationRows[2][1] - rotationRows[1][1] * rotationRows[2][0]);
    if (!std::isfinite(determinant) || std::fabs(determinant - 1.0) > 4.0 * kOrthogonalityTolerance)
        return Fail("reparent transform is not a proper rotation and positive scale");

    // AffineFromTrs stores the transpose of the usual column-vector rotation matrix.
    const double m00 = rotationRows[0][0], m01 = rotationRows[1][0], m02 = rotationRows[2][0];
    const double m10 = rotationRows[0][1], m11 = rotationRows[1][1], m12 = rotationRows[2][1];
    const double m20 = rotationRows[0][2], m21 = rotationRows[1][2], m22 = rotationRows[2][2];
    const double trace = m00 + m11 + m22;
    double qx = 0, qy = 0, qz = 0, qw = 1;
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        if (s <= 1e-12) return Fail("cannot decompose reparent rotation");
        qw = 0.25 * s;
        qx = (m21 - m12) / s;
        qy = (m02 - m20) / s;
        qz = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(std::max(0.0, 1.0 + m00 - m11 - m22)) * 2.0;
        if (s <= 1e-12) return Fail("cannot decompose reparent rotation");
        qw = (m21 - m12) / s;
        qx = 0.25 * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(std::max(0.0, 1.0 + m11 - m00 - m22)) * 2.0;
        if (s <= 1e-12) return Fail("cannot decompose reparent rotation");
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25 * s;
        qz = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(std::max(0.0, 1.0 + m22 - m00 - m11)) * 2.0;
        if (s <= 1e-12) return Fail("cannot decompose reparent rotation");
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25 * s;
    }
    const double qNorm = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!std::isfinite(qNorm) || qNorm <= 1e-12) return Fail("cannot normalize reparent rotation");

    LocalTransform candidate;
    candidate.translation = {matrix.m[3][0], matrix.m[3][1], matrix.m[3][2]};
    candidate.rotation = {static_cast<float>(qx / qNorm), static_cast<float>(qy / qNorm),
                          static_cast<float>(qz / qNorm), static_cast<float>(qw / qNorm)};
    candidate.scale = {static_cast<float>(scales[0]), static_cast<float>(scales[1]),
                       static_cast<float>(scales[2])};
    render::Float4x4 recomposed;
    if (!render::AffineFromTrs({candidate.translation.x, candidate.translation.y, candidate.translation.z},
                               {candidate.rotation.x, candidate.rotation.y, candidate.rotation.z, candidate.rotation.w},
                               {candidate.scale.x, candidate.scale.y, candidate.scale.z}, recomposed))
        return Fail("reparent transform cannot be represented as local TRS");
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column) {
            const double expected = matrix.m[row][column];
            const double tolerance = kMatrixTolerance * std::max(1.0, std::fabs(expected));
            if (std::fabs(static_cast<double>(recomposed.m[row][column]) - expected) > tolerance)
                return Fail("reparent transform cannot be represented as local TRS");
        }
    transform = candidate;
    return Status::Ok();
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
    std::unordered_map<std::string, size_t> depthById;
    color.reserve(document.nodes.size());
    depthById.reserve(document.nodes.size());
    for (const auto& start : document.nodes) {
        std::vector<const Node*> path;
        const Node* current = &start;
        while (current && depthById.find(current->id) == depthById.end()) {
            const auto found = color.find(current->id);
            if (found != color.end() && found->second == 1)
                return Fail("scene hierarchy contains a cycle");
            color.emplace(current->id, 1);
            path.push_back(current);
            current = current->parentId ? byId.at(*current->parentId) : nullptr;
        }
        size_t depth = current ? depthById.at(current->id) : 0;
        while (!path.empty()) {
            const Node* node = path.back();
            path.pop_back();
            if (++depth > kMaxHierarchyDepth) return Fail("scene hierarchy exceeds the depth limit");
            depthById.emplace(node->id, depth);
            color[node->id] = 2;
        }
    }
    return Status::Ok();
}

Status EulerXYZDegreesToQuaternion(Vec3 degrees, Quaternion& rotation) {
    constexpr double kDegreesToRadians = 3.1415926535897932384626433832795 / 180.0;
    const float input[] = {degrees.x, degrees.y, degrees.z};
    for (const float value : input)
        if (!std::isfinite(value) || std::fabs(value) > kMaxRotationDegrees)
            return Fail("local Euler rotation must be finite and within the editor limit");

    const double x = static_cast<double>(degrees.x) * kDegreesToRadians * 0.5;
    const double y = static_cast<double>(degrees.y) * kDegreesToRadians * 0.5;
    const double z = static_cast<double>(degrees.z) * kDegreesToRadians * 0.5;
    const double cx = std::cos(x), sx = std::sin(x);
    const double cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    Quaternion candidate{
        static_cast<float>(sx * cy * cz - cx * sy * sz),
        static_cast<float>(cx * sy * cz + sx * cy * sz),
        static_cast<float>(cx * cy * sz - sx * sy * cz),
        static_cast<float>(cx * cy * cz + sx * sy * sz),
    };
    const double norm = std::sqrt(static_cast<double>(candidate.x) * candidate.x +
                                  static_cast<double>(candidate.y) * candidate.y +
                                  static_cast<double>(candidate.z) * candidate.z +
                                  static_cast<double>(candidate.w) * candidate.w);
    if (!std::isfinite(norm) || norm <= 1e-12)
        return Fail("local Euler rotation could not be converted to a quaternion");
    candidate.x = static_cast<float>(candidate.x / norm);
    candidate.y = static_cast<float>(candidate.y / norm);
    candidate.z = static_cast<float>(candidate.z / norm);
    candidate.w = static_cast<float>(candidate.w / norm);
    rotation = candidate;
    return Status::Ok();
}

Status RotateLocalAxis(Quaternion startingRotation, size_t axis, float deltaRadians,
                       Quaternion& rotation) {
    if (axis >= 3) return Fail("local rotation axis index is out of range");
    if (!std::isfinite(deltaRadians)) return Fail("local rotation delta must be finite");
    const double input[] = {startingRotation.x, startingRotation.y, startingRotation.z,
                            startingRotation.w};
    double normSquared = 0;
    for (const double component : input) {
        if (!std::isfinite(component)) return Fail("starting quaternion must be finite");
        normSquared += component * component;
    }
    if (!std::isfinite(normSquared) || normSquared <= 1e-24)
        return Fail("starting quaternion must be nonzero");
    const double startInverseNorm = 1.0 / std::sqrt(normSquared);
    const double sx = startingRotation.x * startInverseNorm;
    const double sy = startingRotation.y * startInverseNorm;
    const double sz = startingRotation.z * startInverseNorm;
    const double sw = startingRotation.w * startInverseNorm;
    constexpr double kTau = 6.283185307179586476925286766559;
    const double halfAngle = std::remainder(static_cast<double>(deltaRadians), kTau) * 0.5;
    const double sine = std::sin(halfAngle), cosine = std::cos(halfAngle);
    double dx = 0, dy = 0, dz = 0;
    if (axis == 0) dx = sine;
    else if (axis == 1) dy = sine;
    else dz = sine;
    const double rx = sw * dx + sx * cosine + sy * dz - sz * dy;
    const double ry = sw * dy - sx * dz + sy * cosine + sz * dx;
    const double rz = sw * dz + sx * dy - sy * dx + sz * cosine;
    const double rw = sw * cosine - sx * dx - sy * dy - sz * dz;
    const double resultNorm = std::sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
    if (!std::isfinite(resultNorm) || resultNorm <= 1e-12)
        return Fail("local rotation result could not be normalized");
    const Quaternion candidate{static_cast<float>(rx / resultNorm), static_cast<float>(ry / resultNorm),
                               static_cast<float>(rz / resultNorm), static_cast<float>(rw / resultNorm)};
    if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
        !std::isfinite(candidate.z) || !std::isfinite(candidate.w))
        return Fail("local rotation result is not finite");
    rotation = candidate;
    return Status::Ok();
}

Status QuaternionToEulerXYZDegrees(Quaternion rotation, Vec3& degrees) {
    constexpr double kPi = 3.1415926535897932384626433832795;
    constexpr double kRadiansToDegrees = 180.0 / kPi;
    const float input[] = {rotation.x, rotation.y, rotation.z, rotation.w};
    for (const float value : input)
        if (!std::isfinite(value)) return Fail("quaternion rotation must be finite");
    const double norm = std::sqrt(static_cast<double>(rotation.x) * rotation.x +
                                  static_cast<double>(rotation.y) * rotation.y +
                                  static_cast<double>(rotation.z) * rotation.z +
                                  static_cast<double>(rotation.w) * rotation.w);
    if (!std::isfinite(norm) || norm <= 1e-12)
        return Fail("quaternion rotation must be nonzero");
    const double x = rotation.x / norm, y = rotation.y / norm;
    const double z = rotation.z / norm, w = rotation.w / norm;
    const double sinPitch = std::clamp(2.0 * (w * y - z * x), -1.0, 1.0);
    double roll = 0, pitch = 0, yaw = 0;
    if (std::fabs(sinPitch) >= 1.0 - 1e-7) {
        // At gimbal lock only roll-yaw (pitch +90) or roll+yaw (pitch -90) is observable.
        // Choose roll=0 and carry the combined angle in yaw to make the round trip stable.
        pitch = std::copysign(kPi * 0.5, sinPitch);
        yaw = std::remainder(2.0 * std::atan2(z, w), 2.0 * kPi);
    } else {
        roll = std::atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
        pitch = std::asin(sinPitch);
        yaw = std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
    }
    const Vec3 candidate{
        static_cast<float>(roll * kRadiansToDegrees),
        static_cast<float>(pitch * kRadiansToDegrees),
        static_cast<float>(yaw * kRadiansToDegrees),
    };
    if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) || !std::isfinite(candidate.z))
        return Fail("quaternion rotation could not be converted to Euler angles");
    degrees = candidate;
    return Status::Ok();
}

Status ScaleUniform(LocalTransform& transform, float factor) {
    if (!std::isfinite(factor) || factor <= 0)
        return Fail("uniform scale factor must be finite and positive");
    const float original[] = {transform.scale.x, transform.scale.y, transform.scale.z};
    float result[3]{};
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(original[axis]) || original[axis] < kMinScale || original[axis] > kMaxScale)
            return Fail("uniform scale requires valid positive local scale");
        const double scaled = static_cast<double>(original[axis]) * factor;
        if (!std::isfinite(scaled) || scaled < kMinScale || scaled > kMaxScale)
            return Fail("uniform scale would exceed the scene scale limits");
        result[axis] = static_cast<float>(scaled);
    }
    transform.scale = {result[0], result[1], result[2]};
    return Status::Ok();
}

Status SetLocalScaleAxis(LocalTransform& transform, size_t axis, float value) {
    if (axis >= 3) return Fail("local scale axis index is out of range");
    if (!std::isfinite(value) || value < kMinScale || value > kMaxScale)
        return Fail("local scale must be finite, positive, and within the scene limits");
    const float original[] = {transform.scale.x, transform.scale.y, transform.scale.z};
    for (const float component : original)
        if (!std::isfinite(component) || component < kMinScale || component > kMaxScale)
            return Fail("axis scale requires valid positive local scale");
    float result[] = {original[0], original[1], original[2]};
    result[axis] = value;
    transform.scale = {result[0], result[1], result[2]};
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

Status ComputeGroundPlacement(std::optional<render::Float4x4> parentWorld,
                              render::Float3 worldPoint, const std::array<float, 3>& modelBoundsMin,
                              const std::array<float, 3>& modelBoundsMax, LocalTransform& local,
                              render::Float4x4& world) {
    if (!std::isfinite(worldPoint.x) || !std::isfinite(worldPoint.y))
        return Fail("placement point must be finite");
    const render::Float4x4 parent = parentWorld.value_or(render::Float4x4{});
    render::Float4x4 parentInverse;
    if (!render::InvertAffine(parent, parentInverse)) return Fail("placement parent transform is singular or unstable");

    auto applyPoint = [](render::Float3 point, const render::Float4x4& matrix) {
        return render::Float3{
            point.x * matrix.m[0][0] + point.y * matrix.m[1][0] + point.z * matrix.m[2][0] + matrix.m[3][0],
            point.x * matrix.m[0][1] + point.y * matrix.m[1][1] + point.z * matrix.m[2][1] + matrix.m[3][1],
            point.x * matrix.m[0][2] + point.y * matrix.m[1][2] + point.z * matrix.m[2][2] + matrix.m[3][2]};
    };
    LocalTransform candidate;
    const auto initialLocal = applyPoint({worldPoint.x, worldPoint.y, 0}, parentInverse);
    candidate.translation = {initialLocal.x, initialLocal.y, initialLocal.z};
    render::Float4x4 localMatrix;
    auto makeLocalMatrix = [&](render::Float4x4& output) {
        return render::AffineFromTrs({candidate.translation.x, candidate.translation.y, candidate.translation.z},
                                     {candidate.rotation.x, candidate.rotation.y,
                                      candidate.rotation.z, candidate.rotation.w},
                                     {candidate.scale.x, candidate.scale.y, candidate.scale.z}, output);
    };
    if (!makeLocalMatrix(localMatrix)) return Fail("placement transform cannot be represented");
    render::Float4x4 candidateWorld = render::Multiply(localMatrix, parent);
    std::array<float, 3> boundsMin{}, boundsMax{};
    if (!render::TransformBounds(modelBoundsMin, modelBoundsMax, candidateWorld, boundsMin, boundsMax))
        return Fail("model bounds cannot be transformed for placement");
    render::Float3 adjustedWorldOrigin = applyPoint({0, 0, 0}, candidateWorld);
    adjustedWorldOrigin.z -= boundsMin[2];
    const auto adjustedLocal = applyPoint(adjustedWorldOrigin, parentInverse);
    candidate.translation = {adjustedLocal.x, adjustedLocal.y, adjustedLocal.z};
    if (!makeLocalMatrix(localMatrix)) return Fail("ground-adjusted placement transform cannot be represented");
    candidateWorld = render::Multiply(localMatrix, parent);
    if (!render::TransformBounds(modelBoundsMin, modelBoundsMax, candidateWorld, boundsMin, boundsMax))
        return Fail("ground-adjusted model bounds cannot be transformed");
    local = candidate;
    world = candidateWorld;
    return Status::Ok();
}

Status AddGroup(Document& document, std::string id, std::string name,
                std::optional<std::string> parentId, LocalTransform transform) {
    if (auto status = Validate(document); !status) return status;
    Document candidate = document;
    Node node;
    node.id = std::move(id);
    node.name = std::move(name);
    node.kind = NodeKind::Group;
    node.parentId = std::move(parentId);
    node.localTransform = transform;
    candidate.nodes.push_back(std::move(node));
    if (auto status = Validate(candidate); !status) return status;
    document = std::move(candidate);
    return Status::Ok();
}

Status AddModelInstance(Document& document, std::string id, std::string name, std::string assetPath,
                        std::optional<std::string> parentId, LocalTransform transform) {
    if (auto status = Validate(document); !status) return status;
    Document candidate = document;
    Node node;
    node.id = std::move(id);
    node.name = std::move(name);
    node.kind = NodeKind::ModelInstance;
    node.parentId = std::move(parentId);
    node.assetPath = std::move(assetPath);
    node.localTransform = transform;
    candidate.nodes.push_back(std::move(node));
    if (auto status = Validate(candidate); !status) return status;
    document = std::move(candidate);
    return Status::Ok();
}

Status RemoveSubtree(Document& document, std::string_view rootId) {
    if (auto status = Validate(document); !status) return status;
    if (std::none_of(document.nodes.begin(), document.nodes.end(),
                     [&](const Node& node) { return node.id == rootId; }))
        return Fail("scene node does not exist");
    std::unordered_set<std::string> removed{std::string(rootId)};
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& node : document.nodes)
            if (node.parentId && removed.contains(*node.parentId) && removed.insert(node.id).second)
                expanded = true;
    }
    Document candidate = document;
    candidate.nodes.erase(std::remove_if(candidate.nodes.begin(), candidate.nodes.end(),
                                         [&](const Node& node) { return removed.contains(node.id); }),
                          candidate.nodes.end());
    if (auto status = Validate(candidate); !status) return status;
    document = std::move(candidate);
    return Status::Ok();
}

Status DuplicateSubtree(Document& document, std::string_view rootId,
                        const std::function<std::string()>& generateId,
                        std::string& duplicatedRootId, Vec3 localOffset) {
    if (auto status = Validate(document); !status) return status;
    if (!generateId) return Fail("scene node ID generator is unavailable");
    if (!std::isfinite(localOffset.x) || !std::isfinite(localOffset.y) || !std::isfinite(localOffset.z))
        return Fail("duplicate offset must be finite");

    const auto root = std::find_if(document.nodes.begin(), document.nodes.end(),
                                   [&](const Node& node) { return node.id == rootId; });
    if (root == document.nodes.end()) return Fail("scene node does not exist");

    std::unordered_map<std::string, std::vector<const Node*>> childrenByParent;
    childrenByParent.reserve(document.nodes.size());
    for (const auto& node : document.nodes)
        if (node.parentId) childrenByParent[*node.parentId].push_back(&node);

    // Parent-first traversal gives deterministic ID assignment even when the stored document is
    // not arranged parent-first. Child ordering follows the source document.
    std::vector<const Node*> subtree;
    subtree.reserve(document.nodes.size());
    subtree.push_back(&*root);
    for (size_t cursor = 0; cursor < subtree.size(); ++cursor) {
        const auto found = childrenByParent.find(subtree[cursor]->id);
        if (found == childrenByParent.end()) continue;
        subtree.insert(subtree.end(), found->second.begin(), found->second.end());
    }
    if (subtree.size() > kMaxSceneNodes - document.nodes.size())
        return Fail("duplicated subtree would exceed the scene node limit");

    std::unordered_set<std::string> usedIds;
    usedIds.reserve(document.nodes.size() + subtree.size());
    for (const auto& node : document.nodes) usedIds.insert(node.id);

    std::unordered_map<std::string, std::string> idRemap;
    idRemap.reserve(subtree.size());
    for (const Node* source : subtree) {
        std::string generated = generateId();
        if (!ValidIdentifier(generated) || !usedIds.insert(generated).second)
            return Fail("scene node ID generator returned an invalid or reused ID");
        idRemap.emplace(source->id, std::move(generated));
    }

    std::vector<Node> copies;
    copies.reserve(subtree.size());
    for (const Node* source : subtree) {
        Node copy = *source;
        copy.id = idRemap.at(source->id);
        if (source == &*root) {
            copy.name = DuplicateNodeName(source->name);
            copy.localTransform.translation.x += localOffset.x;
            copy.localTransform.translation.y += localOffset.y;
            copy.localTransform.translation.z += localOffset.z;
        } else if (source->parentId) {
            copy.parentId = idRemap.at(*source->parentId);
        }
        copies.push_back(std::move(copy));
    }

    Document candidate = document;
    candidate.nodes.insert(candidate.nodes.end(), copies.begin(), copies.end());
    if (auto status = Validate(candidate); !status) return status;
    const std::string newRootId = copies.front().id;
    document = std::move(candidate);
    duplicatedRootId = newRootId;
    return Status::Ok();
}

Status RenameNode(Document& document, std::string_view nodeId, std::string name) {
    if (auto status = Validate(document); !status) return status;
    Document candidate = document;
    const auto found = std::find_if(candidate.nodes.begin(), candidate.nodes.end(),
                                    [&](const Node& node) { return node.id == nodeId; });
    if (found == candidate.nodes.end()) return Fail("scene node does not exist");
    if (found->name == name) return Status::Ok();
    found->name = std::move(name);
    if (auto status = Validate(candidate); !status) return status;
    document = std::move(candidate);
    return Status::Ok();
}

Status ReparentNode(Document& document, std::string_view nodeId,
                    std::optional<std::string> parentId, bool preserveWorldTransform) {
    if (auto status = Validate(document); !status) return status;
    const auto child = std::find_if(document.nodes.begin(), document.nodes.end(),
                                    [&](const Node& node) { return node.id == nodeId; });
    if (child == document.nodes.end()) return Fail("scene node does not exist");
    if (parentId == child->parentId) return Status::Ok();

    const Node* newParent = nullptr;
    if (parentId) {
        const auto found = std::find_if(document.nodes.begin(), document.nodes.end(),
                                        [&](const Node& node) { return node.id == *parentId; });
        if (found == document.nodes.end()) return Fail("new scene parent does not exist");
        if (found->kind != NodeKind::Group) return Fail("only group nodes may contain children");
        newParent = &*found;

        const Node* cursor = newParent;
        while (cursor) {
            if (cursor->id == nodeId) return Fail("reparent would create a scene hierarchy cycle");
            if (!cursor->parentId) break;
            const auto ancestor = std::find_if(document.nodes.begin(), document.nodes.end(),
                                               [&](const Node& node) { return node.id == *cursor->parentId; });
            cursor = ancestor == document.nodes.end() ? nullptr : &*ancestor;
        }
    }

    LocalTransform local = child->localTransform;
    std::vector<ResolvedTransform> oldWorlds;
    const render::Float4x4* oldWorld = nullptr;
    if (preserveWorldTransform) {
        if (auto status = ResolveWorldTransforms(document, oldWorlds); !status) return status;
        const auto oldChild = std::find_if(oldWorlds.begin(), oldWorlds.end(),
                                           [&](const ResolvedTransform& item) { return item.nodeId == nodeId; });
        if (oldChild == oldWorlds.end()) return Fail("scene node world transform is unavailable");
        oldWorld = &oldChild->world;
        render::Float4x4 parentWorld;
        if (newParent) {
            const auto resolvedParent = std::find_if(oldWorlds.begin(), oldWorlds.end(),
                [&](const ResolvedTransform& item) { return item.nodeId == newParent->id; });
            if (resolvedParent == oldWorlds.end()) return Fail("new parent world transform is unavailable");
            parentWorld = resolvedParent->world;
        }
        render::Float4x4 parentInverse;
        if (!render::InvertAffine(parentWorld, parentInverse))
            return Fail("new parent transform is singular or numerically unstable");
        const render::Float4x4 relative = render::Multiply(*oldWorld, parentInverse);
        if (auto status = LocalTransformFromAffine(relative, local); !status) return status;
    }

    Document candidate = document;
    const auto moved = std::find_if(candidate.nodes.begin(), candidate.nodes.end(),
                                    [&](const Node& node) { return node.id == nodeId; });
    moved->parentId = std::move(parentId);
    moved->localTransform = local;
    if (auto status = Validate(candidate); !status) return status;
    std::vector<ResolvedTransform> newWorlds;
    if (auto status = ResolveWorldTransforms(candidate, newWorlds); !status) return status;
    if (preserveWorldTransform) {
        std::unordered_map<std::string, const Node*> nodesById;
        nodesById.reserve(candidate.nodes.size());
        for (const auto& node : candidate.nodes) nodesById.emplace(node.id, &node);
        std::unordered_map<std::string, const render::Float4x4*> oldWorldById, newWorldById;
        oldWorldById.reserve(oldWorlds.size());
        newWorldById.reserve(newWorlds.size());
        for (const auto& item : oldWorlds) oldWorldById.emplace(item.nodeId, &item.world);
        for (const auto& item : newWorlds) newWorldById.emplace(item.nodeId, &item.world);
        for (const auto& subtreeNode : candidate.nodes) {
            const Node* cursor = &subtreeNode;
            bool inMovedSubtree = false;
            while (cursor) {
                if (cursor->id == nodeId) { inMovedSubtree = true; break; }
                if (!cursor->parentId) break;
                const auto parent = nodesById.find(*cursor->parentId);
                cursor = parent == nodesById.end() ? nullptr : parent->second;
            }
            if (!inMovedSubtree) continue;
            const auto oldTransform = oldWorldById.find(subtreeNode.id);
            const auto newTransform = newWorldById.find(subtreeNode.id);
            if (oldTransform == oldWorldById.end() || newTransform == newWorldById.end())
                return Fail("reparented subtree world transform is unavailable");
            for (size_t row = 0; row < 4; ++row)
                for (size_t column = 0; column < 4; ++column) {
                    const double expected = oldTransform->second->m[row][column];
                    const double tolerance = 1e-3 * std::max(1.0, std::fabs(expected));
                    if (std::fabs(static_cast<double>(newTransform->second->m[row][column]) - expected) > tolerance)
                        return Fail("reparent cannot preserve subtree world transforms within tolerance");
                }
        }
    }
    document = std::move(candidate);
    return Status::Ok();
}

Status ProjectStore::ValidateSourceSeparation(
    const std::filesystem::path& projectRoot,
    std::span<const std::filesystem::path> mountedSources) {
    std::error_code ec;
    const auto canonicalProject = std::filesystem::canonical(projectRoot, ec);
    if (ec) return Fail("project directory cannot be resolved: " + ec.message());
    if (!std::filesystem::is_directory(canonicalProject, ec) || ec)
        return Fail("project root must be an existing directory");

    for (const auto& source : mountedSources) {
        const auto canonicalSource = std::filesystem::canonical(source, ec);
        if (ec) return Fail("mounted asset source cannot be resolved: " + ec.message());
        if (RootsOverlap(canonicalProject, canonicalSource))
            return Fail("project directory overlaps a mounted asset source");
    }
    return Status::Ok();
}

std::optional<ProjectStore> ProjectStore::Open(
    const std::filesystem::path& projectRoot,
    std::span<const std::filesystem::path> readOnlyDirectoryRoots,
    Status* status) {
    if (auto separation = ValidateSourceSeparation(projectRoot, readOnlyDirectoryRoots); !separation) {
        if (status) *status = std::move(separation);
        return std::nullopt;
    }
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

Status ProjectStore::ListScenes(
    std::vector<std::string>& sceneIds,
    std::span<const std::filesystem::path> mountedSources) const {
    if (auto status = ValidateSourceSeparation(root_, mountedSources); !status) return status;
    if (auto status = CheckRootSeparation(); !status) return status;
    constexpr size_t kMaxListedScenes = 4096;
    std::vector<std::string> listed;
    std::error_code ec;
    std::filesystem::directory_iterator it(root_, ec), end;
    if (ec) return Fail("cannot list project scenes: " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec) return Fail("project directory changed while listing scenes: " + ec.message());
        const std::string filename = it->path().filename().string();
        constexpr std::string_view suffix = ".scene.json";
        if (!filename.ends_with(suffix)) continue;
        const std::string id = filename.substr(0, filename.size() - suffix.size());
        if (!ValidIdentifier(id)) continue;
        const auto fileStatus = it->symlink_status(ec);
        if (ec) return Fail("cannot inspect project scene entry: " + ec.message());
        if (!std::filesystem::is_regular_file(fileStatus) || std::filesystem::is_symlink(fileStatus)) continue;
        listed.push_back(id);
        if (listed.size() > kMaxListedScenes) return Fail("project has too many scene files to list");
    }
    if (ec) return Fail("project directory changed while listing scenes: " + ec.message());
    std::sort(listed.begin(), listed.end());
    sceneIds = std::move(listed);
    return Status::Ok();
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

Status ProjectStore::Save(
    const Document& document,
    std::span<const std::filesystem::path> mountedSources) const {
    if (auto status = ValidateSourceSeparation(root_, mountedSources); !status) return status;
    if (auto status = CheckRootSeparation(); !status) return status;
    if (auto status = Validate(document); !status) return status;
    std::string json;
    if (auto status = Serialize(document, json); !status) return status;
    return AtomicWrite(root_, ScenePath(document.id), json, true);
}

Status ProjectStore::Create(
    const Document& document,
    std::span<const std::filesystem::path> mountedSources) const {
    if (auto status = ValidateSourceSeparation(root_, mountedSources); !status) return status;
    if (auto status = CheckRootSeparation(); !status) return status;
    if (auto status = Validate(document); !status) return status;
    std::string json;
    if (auto status = Serialize(document, json); !status) return status;
    return AtomicWrite(root_, ScenePath(document.id), json, false);
}

Status ProjectStore::Load(
    std::string_view sceneId, Document& document,
    std::span<const std::filesystem::path> mountedSources) const {
    if (auto status = ValidateSourceSeparation(root_, mountedSources); !status) return status;
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

Status DocumentDirtyState::SetBaseline(const Document& document, bool exists) {
    if (!exists) {
        baseline_.reset();
        dirty_ = true;
        return Status::Ok();
    }
    baseline_.reset();
    dirty_ = true;
    std::string saved;
    if (auto status = Serialize(document, saved); !status) return status;
    baseline_ = std::move(saved);
    dirty_ = false;
    return Status::Ok();
}

Status DocumentDirtyState::Update(const Document& document) {
    std::string current;
    if (auto status = Serialize(document, current); !status) {
        dirty_ = true;
        return status;
    }
    dirty_ = !baseline_ || current != *baseline_;
    return Status::Ok();
}

std::optional<SceneAction> SceneActionGate::Request(SceneAction action, bool dirty) {
    pending_.reset();
    if (!dirty) return action;
    pending_ = std::move(action);
    return std::nullopt;
}

std::optional<SceneAction> SceneActionGate::Resolve(SceneActionChoice choice, bool saveSucceeded) {
    if (!pending_) return std::nullopt;
    if (choice == SceneActionChoice::Cancel) {
        pending_.reset();
        return std::nullopt;
    }
    if (choice == SceneActionChoice::Save && !saveSucceeded) return std::nullopt;
    auto action = std::move(pending_);
    pending_.reset();
    return action;
}

Status DocumentHistory::RecordBefore(const Document& document) {
    std::string snapshot;
    if (auto status = Serialize(document, snapshot); !status) return status;
    for (const auto& entry : redo_) retainedBytes_ -= entry.size();
    redo_.clear();
    retainedBytes_ += snapshot.size();
    undo_.push_back(std::move(snapshot));
    Trim();
    return Status::Ok();
}

Status DocumentHistory::Undo(const Document& current, Document& restored) {
    if (undo_.empty()) return Fail("there is no scene change to undo");
    std::string currentSnapshot;
    if (auto status = Serialize(current, currentSnapshot); !status) return status;
    Document candidate;
    if (auto status = Deserialize(undo_.back(), candidate); !status) return status;

    retainedBytes_ -= undo_.back().size();
    undo_.pop_back();
    retainedBytes_ += currentSnapshot.size();
    redo_.push_back(std::move(currentSnapshot));
    Trim();
    restored = std::move(candidate);
    return Status::Ok();
}

Status DocumentHistory::Redo(const Document& current, Document& restored) {
    if (redo_.empty()) return Fail("there is no scene change to redo");
    std::string currentSnapshot;
    if (auto status = Serialize(current, currentSnapshot); !status) return status;
    Document candidate;
    if (auto status = Deserialize(redo_.back(), candidate); !status) return status;

    retainedBytes_ -= redo_.back().size();
    redo_.pop_back();
    retainedBytes_ += currentSnapshot.size();
    undo_.push_back(std::move(currentSnapshot));
    Trim();
    restored = std::move(candidate);
    return Status::Ok();
}

Status DocumentHistory::CancelLatestChange(const Document& expectedBefore, Document& restored) {
    if (!redo_.empty()) return Fail("cannot cancel an interactive edit while redo history is active");
    if (undo_.empty()) return Fail("there is no captured scene change to cancel");
    Document candidate;
    if (auto status = Deserialize(undo_.back(), candidate); !status) return status;
    if (candidate != expectedBefore)
        return Fail("the latest scene change no longer matches the interactive edit being canceled");

    retainedBytes_ -= undo_.back().size();
    undo_.pop_back();
    restored = std::move(candidate);
    return Status::Ok();
}

void DocumentHistory::Clear() noexcept {
    undo_.clear();
    redo_.clear();
    retainedBytes_ = 0;
}

void DocumentHistory::DropOldest(std::vector<std::string>& stack) noexcept {
    if (stack.empty()) return;
    retainedBytes_ -= stack.front().size();
    stack.erase(stack.begin());
}

void DocumentHistory::Trim() noexcept {
    while (retainedEntries() > kMaxEntries || retainedBytes_ > kMaxBytes) {
        // Keep the newest undo snapshots and the next redo target. redo_.back() is always next.
        if (!undo_.empty()) DropOldest(undo_);
        else if (!redo_.empty()) DropOldest(redo_);
        else break;
    }
}

}  // namespace scuba::scene
