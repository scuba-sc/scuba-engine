// SPDX-License-Identifier: GPL-3.0-only
// Model importers. OBJ and CryEngine CGF/CGA reuse scuba-engine's bounded Asset Lab readers;
// glTF 2.0 (.gltf/.glb) is decoded here from the accessor model in the Khronos specification.
#include "scuba/asset/model.h"
#include "scuba/vfs/virtual_file_system.h"

#include "assets/formats.h"
#include "assets/io.h"
#include "hostd/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace scuba::asset {
namespace {

using hostd::Json;

constexpr size_t kMaxVertices = 50'000'000;

std::string Lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

MeshData FromAssetMesh(const assets::Mesh& in) {
    MeshData m;
    m.positions.reserve(in.positions.size() * 3);
    for (const auto& p : in.positions) m.positions.insert(m.positions.end(), p.begin(), p.end());
    if (in.normals.size() == in.positions.size())
        for (const auto& n : in.normals) m.normals.insert(m.normals.end(), n.begin(), n.end());
    if (in.uv.size() == in.positions.size())
        for (const auto& t : in.uv) m.uvs.insert(m.uvs.end(), t.begin(), t.end());
    m.indices = in.indices;
    m.material = in.material;
    return m;
}

// ---- glTF ------------------------------------------------------------------------------------

size_t Count(const Json* j, size_t fallback = 0) {
    if (!j || !j->is_number()) return fallback;
    const double v = j->as_number();
    if (v < 0 || v > 9.0e15 || v != std::floor(v)) return std::numeric_limits<size_t>::max();
    return static_cast<size_t>(v);
}

bool Base64Decode(std::string_view in, vfs::Bytes& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

class Gltf {
public:
    Gltf(const Json& doc, std::vector<vfs::Bytes> buffers) : doc_(doc), buffers_(std::move(buffers)) {}

    Status Decode(ModelData& out) {
        const Json* meshes = doc_.find("meshes");
        if (!meshes || !meshes->is_array() || meshes->size() == 0) return Status::Fail("glTF has no meshes");
        for (size_t mi = 0; mi < meshes->size(); ++mi) {
            const Json* prims = meshes->at(mi).find("primitives");
            if (!prims || !prims->is_array()) continue;
            for (size_t pi = 0; pi < prims->size(); ++pi) {
                const Json& prim = prims->at(pi);
                if (Count(prim.find("mode"), 4) != 4) continue;   // triangles only
                MeshData mesh;
                if (auto st = Primitive(prim, mesh); !st)
                    return Status::Fail("mesh " + std::to_string(mi) + " primitive " + std::to_string(pi) + ": " + st.error);
                out.meshes.push_back(std::move(mesh));
            }
        }
        if (out.meshes.empty()) return Status::Fail("glTF has no triangle primitives");
        return Status::Ok();
    }

private:
    struct View {
        const uint8_t* base = nullptr;
        size_t stride = 0, count = 0, comps = 0;
        uint32_t componentType = 0;
        bool normalized = false;
    };

    static size_t Components(const std::string& type) {
        if (type == "SCALAR") return 1;
        if (type == "VEC2") return 2;
        if (type == "VEC3") return 3;
        if (type == "VEC4") return 4;
        return 0;
    }
    static size_t ComponentSize(uint32_t t) {
        switch (t) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: return 0;
        }
    }

    Status Accessor(size_t index, View& v) const {
        const Json* accessors = doc_.find("accessors");
        if (!accessors || index >= accessors->size()) return Status::Fail("accessor " + std::to_string(index) + " missing");
        const Json& a = accessors->at(index);
        if (a.find("sparse")) return Status::Fail("sparse accessors are not supported");
        const size_t viewIndex = Count(a.find("bufferView"), std::numeric_limits<size_t>::max());
        const Json* views = doc_.find("bufferViews");
        if (!views || viewIndex >= views->size()) return Status::Fail("accessor without a buffer view");
        const Json& bv = views->at(viewIndex);
        const size_t bufferIndex = Count(bv.find("buffer"), std::numeric_limits<size_t>::max());
        if (bufferIndex >= buffers_.size()) return Status::Fail("buffer view points at a missing buffer");
        const vfs::Bytes& buffer = buffers_[bufferIndex];

        v.componentType = static_cast<uint32_t>(Count(a.find("componentType")));
        const Json* type = a.find("type");
        v.comps = type && type->is_string() ? Components(type->as_string()) : 0;
        const size_t cs = ComponentSize(v.componentType);
        if (!v.comps || !cs) return Status::Fail("unsupported accessor layout");
        v.count = Count(a.find("count"));
        v.normalized = a.find("normalized") && a.find("normalized")->as_bool();
        if (v.count > kMaxVertices * 3) return Status::Fail("accessor too large");
        const size_t elem = v.comps * cs;
        const size_t viewOffset = Count(bv.find("byteOffset")), viewLength = Count(bv.find("byteLength"));
        const size_t accOffset = Count(a.find("byteOffset"));
        v.stride = Count(bv.find("byteStride"), elem);
        if (v.stride < elem || v.stride > 252) return Status::Fail("bad byteStride");
        if (viewOffset > buffer.size() || viewLength > buffer.size() - viewOffset) return Status::Fail("buffer view out of range");
        if (v.count == 0) return Status::Fail("empty accessor");
        const size_t last = accOffset + v.stride * (v.count - 1) + elem;
        if (accOffset > viewLength || last > viewLength || last < accOffset) return Status::Fail("accessor out of range");
        v.base = buffer.data() + viewOffset + accOffset;
        return Status::Ok();
    }

    static float Component(const View& v, size_t i, size_t c) {
        const uint8_t* p = v.base + i * v.stride + c * ComponentSize(v.componentType);
        switch (v.componentType) {
        case 5126: { float f; std::memcpy(&f, p, 4); return f; }
        case 5121: return v.normalized ? p[0] / 255.0f : p[0];
        case 5123: { uint16_t u; std::memcpy(&u, p, 2); return v.normalized ? u / 65535.0f : u; }
        case 5120: { int8_t s; std::memcpy(&s, p, 1); return v.normalized ? std::max(s / 127.0f, -1.0f) : s; }
        case 5122: { int16_t s; std::memcpy(&s, p, 2); return v.normalized ? std::max(s / 32767.0f, -1.0f) : s; }
        default: { uint32_t u; std::memcpy(&u, p, 4); return static_cast<float>(u); }
        }
    }

    Status Floats(size_t accessor, size_t comps, std::vector<float>& out, size_t expectCount) const {
        View v;
        if (auto st = Accessor(accessor, v); !st) return st;
        if (v.comps != comps) return Status::Fail("unexpected component count");
        if (expectCount && v.count != expectCount) return Status::Fail("attribute count differs from POSITION");
        out.resize(v.count * comps);
        for (size_t i = 0; i < v.count; ++i)
            for (size_t c = 0; c < comps; ++c) out[i * comps + c] = Component(v, i, c);
        return Status::Ok();
    }

    Status Primitive(const Json& prim, MeshData& mesh) const {
        const Json* attrs = prim.find("attributes");
        if (!attrs || !attrs->is_object()) return Status::Fail("no attributes");
        const Json* pos = attrs->find("POSITION");
        if (!pos) return Status::Fail("no POSITION");
        if (auto st = Floats(Count(pos), 3, mesh.positions, 0); !st) return st;
        const size_t n = mesh.positions.size() / 3;
        if (n > kMaxVertices) return Status::Fail("too many vertices");
        if (const Json* nrm = attrs->find("NORMAL"))
            if (auto st = Floats(Count(nrm), 3, mesh.normals, n); !st) return st;
        if (const Json* uv = attrs->find("TEXCOORD_0"))
            if (auto st = Floats(Count(uv), 2, mesh.uvs, n); !st) return st;

        if (const Json* idx = prim.find("indices")) {
            View v;
            if (auto st = Accessor(Count(idx), v); !st) return st;
            if (v.comps != 1 || (v.componentType != 5121 && v.componentType != 5123 && v.componentType != 5125))
                return Status::Fail("bad index accessor");
            mesh.indices.resize(v.count);
            for (size_t i = 0; i < v.count; ++i) {
                const uint8_t* p = v.base + i * v.stride;
                uint32_t value = 0;
                if (v.componentType == 5121) value = p[0];
                else if (v.componentType == 5123) { uint16_t u; std::memcpy(&u, p, 2); value = u; }
                else std::memcpy(&value, p, 4);
                if (value >= n) return Status::Fail("index out of range");
                mesh.indices[i] = value;
            }
        } else {
            mesh.indices.resize(n);
            for (size_t i = 0; i < n; ++i) mesh.indices[i] = static_cast<uint32_t>(i);
        }
        if (mesh.indices.size() % 3) return Status::Fail("index count is not a multiple of 3");

        const size_t material = Count(prim.find("material"), std::numeric_limits<size_t>::max());
        if (const Json* mats = doc_.find("materials"); mats && material < mats->size())
            if (const Json* name = mats->at(material).find("name"); name && name->is_string()) mesh.material = name->as_string();
        return Status::Ok();
    }

    const Json& doc_;
    std::vector<vfs::Bytes> buffers_;
};

std::string Directory(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

Status LoadBuffers(const Json& doc, std::string_view path, const vfs::VirtualFileSystem* vfs,
                   const vfs::Bytes* glbBin, std::vector<vfs::Bytes>& buffers) {
    const Json* list = doc.find("buffers");
    if (!list || !list->is_array()) return Status::Ok();
    for (size_t i = 0; i < list->size(); ++i) {
        const Json& b = list->at(i);
        vfs::Bytes bytes;
        const Json* uri = b.find("uri");
        if (!uri) {
            if (i != 0 || !glbBin) return Status::Fail("buffer " + std::to_string(i) + " has no uri");
            bytes = *glbBin;
        } else if (!uri->is_string()) {
            return Status::Fail("buffer uri is not a string");
        } else if (uri->as_string().rfind("data:", 0) == 0) {
            const auto& s = uri->as_string();
            const auto comma = s.find(',');
            if (comma == std::string::npos || s.find(";base64") > comma) return Status::Fail("only base64 data URIs are supported");
            if (!Base64Decode(std::string_view(s).substr(comma + 1), bytes)) return Status::Fail("bad base64 in buffer " + std::to_string(i));
        } else {
            if (!vfs) return Status::Fail("external buffer " + uri->as_string() + " needs a VFS");
            const std::string dir = Directory(path);
            const std::string rel = dir.empty() ? uri->as_string() : dir + "/" + uri->as_string();
            if (auto st = vfs->ReadAll(rel, bytes); !st) return Status::Fail("buffer " + uri->as_string() + ": " + st.error);
        }
        const size_t declared = Count(b.find("byteLength"));
        if (declared > bytes.size()) return Status::Fail("buffer " + std::to_string(i) + " shorter than its byteLength");
        buffers.push_back(std::move(bytes));
    }
    return Status::Ok();
}

uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

Status DecodeGltf(std::string_view path, std::span<const uint8_t> bytes, const vfs::VirtualFileSystem* vfs,
                  bool binary, ModelData& out) {
    std::string_view jsonText;
    vfs::Bytes bin;
    bool hasBin = false;
    if (binary) {
        if (bytes.size() < 20 || Le32(bytes.data()) != 0x46546C67u || Le32(bytes.data() + 4) != 2)
            return Status::Fail("not a glTF 2.0 binary");
        if (Le32(bytes.data() + 8) > bytes.size()) return Status::Fail("truncated GLB");
        size_t at = 12;
        while (at + 8 <= bytes.size()) {
            const uint32_t len = Le32(bytes.data() + at), type = Le32(bytes.data() + at + 4);
            if (len > bytes.size() - at - 8) return Status::Fail("GLB chunk past the end");
            const auto* chunk = bytes.data() + at + 8;
            if (type == 0x4E4F534Au && jsonText.empty()) jsonText = std::string_view(reinterpret_cast<const char*>(chunk), len);
            else if (type == 0x004E4942u && !hasBin) { bin.assign(chunk, chunk + len); hasBin = true; }
            at += 8 + ((len + 3u) & ~3u);
        }
        if (jsonText.empty()) return Status::Fail("GLB has no JSON chunk");
    } else {
        jsonText = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::string error;
    auto doc = Json::parse(jsonText, &error);
    if (!doc || !doc->is_object()) return Status::Fail("invalid glTF JSON: " + error);
    std::vector<vfs::Bytes> buffers;
    if (auto st = LoadBuffers(*doc, path, vfs, hasBin ? &bin : nullptr, buffers); !st) return st;
    return Gltf(*doc, std::move(buffers)).Decode(out);
}

}  // namespace

size_t ModelData::triangleCount() const noexcept {
    size_t n = 0;
    for (const auto& m : meshes) n += m.indices.size() / 3;
    return n;
}

ModelFormat FormatOf(std::string_view path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return ModelFormat::Unknown;
    const std::string ext = Lower(path.substr(dot + 1));
    if (ext == "gltf") return ModelFormat::Gltf;
    if (ext == "glb") return ModelFormat::Glb;
    if (ext == "obj") return ModelFormat::Obj;
    if (ext == "cgf" || ext == "cga") return ModelFormat::Cry;
    if (ext == "fbx") return ModelFormat::Fbx;
    return ModelFormat::Unknown;
}

const char* FormatName(ModelFormat format) {
    switch (format) {
    case ModelFormat::Gltf: return "glTF";
    case ModelFormat::Glb: return "GLB";
    case ModelFormat::Obj: return "OBJ";
    case ModelFormat::Cry: return "CGF/CGA";
    case ModelFormat::Fbx: return "FBX";
    case ModelFormat::Unknown: break;
    }
    return "unknown";
}

void FinishMesh(MeshData& mesh) {
    const size_t n = mesh.positions.size() / 3;
    if (mesh.normals.size() != n * 3) {
        mesh.normals.assign(n * 3, 0.0f);
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const uint32_t i[3] = {mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
            if (i[0] >= n || i[1] >= n || i[2] >= n) continue;
            const float* a = &mesh.positions[i[0] * 3];
            const float* b = &mesh.positions[i[1] * 3];
            const float* c = &mesh.positions[i[2] * 3];
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (uint32_t k : i)
                for (int d = 0; d < 3; ++d) mesh.normals[k * 3 + d] += cross[d];   // area-weighted
        }
        for (size_t v = 0; v < n; ++v) {
            float* nv = &mesh.normals[v * 3];
            const float len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            if (len > 1e-20f) { nv[0] /= len; nv[1] /= len; nv[2] /= len; }
            else { nv[0] = 0; nv[1] = 0; nv[2] = 1; }
        }
    }
    if (!mesh.uvs.empty() && mesh.uvs.size() != n * 2) mesh.uvs.clear();
    mesh.boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    mesh.boundsMax = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (size_t v = 0; v < n; ++v)
        for (int d = 0; d < 3; ++d) {
            mesh.boundsMin[d] = std::min(mesh.boundsMin[d], mesh.positions[v * 3 + d]);
            mesh.boundsMax[d] = std::max(mesh.boundsMax[d], mesh.positions[v * 3 + d]);
        }
    if (n == 0) mesh.boundsMin = mesh.boundsMax = {0, 0, 0};
}

Status DecodeModel(std::string_view path, std::span<const uint8_t> bytes, const vfs::VirtualFileSystem* vfs,
                   ModelData& out) {
    out = ModelData{};
    out.path = std::string(path);
    const ModelFormat format = FormatOf(path);
    Status st;
    try {
        switch (format) {
        case ModelFormat::Glb: st = DecodeGltf(path, bytes, vfs, true, out); break;
        case ModelFormat::Gltf: st = DecodeGltf(path, bytes, vfs, false, out); break;
        case ModelFormat::Obj:
            out.meshes.push_back(FromAssetMesh(assets::ReadObj(assets::Text(bytes))));
            break;
        case ModelFormat::Cry:
            for (const auto& m : assets::ReadCry(bytes)) out.meshes.push_back(FromAssetMesh(m));
            if (out.meshes.empty()) st = Status::Fail("no mesh chunks");
            break;
        case ModelFormat::Fbx:
            st = Status::Fail("FBX import is not built yet (planned: ufbx, MIT)");
            break;
        case ModelFormat::Unknown:
            st = Status::Fail("unknown model format: " + std::string(path));
            break;
        }
    } catch (const std::exception& e) {
        st = Status::Fail(e.what());
    }
    if (!st) {
        out.meshes.clear();
        return Status::Fail(std::string(FormatName(format)) + " " + std::string(path) + ": " + st.error);
    }
    // glTF is Y-up by specification and OBJ is Y-up by convention; the engine and the sco-core
    // SDK are Z-up. (x, y, z) -> (x, -z, y). CryEngine geometry is already Z-up.
    if (format == ModelFormat::Gltf || format == ModelFormat::Glb || format == ModelFormat::Obj) {
        auto toZUp = [](std::vector<float>& v) {
            for (size_t i = 0; i + 2 < v.size(); i += 3) {
                const float y = v[i + 1], z = v[i + 2];
                v[i + 1] = -z;
                v[i + 2] = y;
            }
        };
        for (auto& m : out.meshes) {
            toZUp(m.positions);
            toZUp(m.normals);
        }
    }
    out.boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    out.boundsMax = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (auto& m : out.meshes) {
        FinishMesh(m);
        for (int d = 0; d < 3; ++d) {
            out.boundsMin[d] = std::min(out.boundsMin[d], m.boundsMin[d]);
            out.boundsMax[d] = std::max(out.boundsMax[d], m.boundsMax[d]);
        }
    }
    return Status::Ok();
}

}  // namespace scuba::asset
