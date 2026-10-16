// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// CPU-side model data: what the importers produce and what the renderer uploads. Flat,
// interleave-free arrays so an upload is a straight copy into GPU buffers.
#include "scuba/core/status.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scuba::vfs { class VirtualFileSystem; }

namespace scuba::asset {

using Vec3 = std::array<float, 3>;

struct MeshData {
    std::vector<float> positions;   // xyz per vertex
    std::vector<float> normals;     // xyz per vertex (generated when the source has none)
    std::vector<float> uvs;         // uv per vertex, or empty
    std::vector<uint32_t> indices;  // triangle list
    std::string material;           // source material name, if any
    Vec3 boundsMin{}, boundsMax{};

    uint32_t vertexCount() const noexcept { return static_cast<uint32_t>(positions.size() / 3); }
};

struct ModelData {
    std::string path;               // the VFS path it was loaded from
    std::vector<MeshData> meshes;
    Vec3 boundsMin{}, boundsMax{};

    size_t triangleCount() const noexcept;
};

enum class ModelFormat : uint8_t { Unknown, Gltf, Glb, Obj, Cry, Fbx };

ModelFormat FormatOf(std::string_view path);   // by extension
const char* FormatName(ModelFormat format);

// Decodes a model file's bytes. vfs resolves a .gltf's external buffers relative to path; it may
// be null for self-contained inputs. Static geometry only: node transforms are not applied
// (glTF primitives are taken in mesh order), skins and animation are ignored.
Status DecodeModel(std::string_view path, std::span<const uint8_t> bytes, const vfs::VirtualFileSystem* vfs,
                   ModelData& out);

// Fills missing normals (area-weighted vertex normals) and the bounds. Importers call it; tools
// that build MeshData by hand can too.
void FinishMesh(MeshData& mesh);

}  // namespace scuba::asset
