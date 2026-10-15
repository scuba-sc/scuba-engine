#include "assets/formats.h"
#include <algorithm>
#include <map>

namespace scuba::assets {
namespace {
struct Chunk { uint16_t type, version; uint32_t id; size_t offset, size; };
std::string Name(std::span<const uint8_t> bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0});
    if (end == bytes.end()) throw Error("unterminated material name");
    return std::string(bytes.begin(), end);
}
}
std::vector<Mesh> ReadCry(std::span<const uint8_t> bytes) {
    const Reader file(bytes);
    file.slice(0, 16);
    if (file.u32(0) != 0x68437243U) throw Error("unsupported Cry header; expected CrCh chunk file");
    if (file.u32(4) != 0x746) throw Error("unsupported Cry file version; only little-endian 0x746");
    const size_t count = file.u32(8), table = file.u32(12);
    if (count == 0 || count > 65536 || table < 16) throw Error("invalid Cry chunk table");
    file.slice(table, count * 16);
    std::map<uint32_t, Chunk> chunks;
    std::vector<std::pair<size_t, size_t>> ranges{{0, 16}, {table, table + count * 16}};
    for (size_t index = 0; index < count; ++index) {
        const size_t entry = table + index * 16;
        Chunk chunk{file.u16(entry), file.u16(entry + 2), file.u32(entry + 4), file.u32(entry + 12), file.u32(entry + 8)};
        file.slice(chunk.offset, chunk.size);
        if (chunk.size == 0) throw Error("empty Cry chunk");
        if (!chunks.emplace(chunk.id, chunk).second) throw Error("duplicate Cry chunk identifier");
        ranges.emplace_back(chunk.offset, chunk.offset + chunk.size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t index = 1; index < ranges.size(); ++index)
        if (ranges[index].first < ranges[index - 1].second) throw Error("overlapping Cry chunk ranges");
    // The format's stream slots are positions, normals, UVs, colors, colors2, indices, then auxiliary streams.
    auto stream = [&](uint32_t id, uint32_t kind, uint32_t elements, uint32_t stride) {
        const auto found = chunks.find(id);
        if (found == chunks.end() || found->second.type != 0x1016) throw Error("missing Cry data stream chunk");
        const auto& chunk = found->second;
        if (chunk.version != 0x800) throw Error("unsupported Cry data stream version; expected 0x800");
        const Reader data(file.slice(chunk.offset, chunk.size));
        data.slice(0, 24);
        if (data.u32(4) != kind || data.u32(8) != elements || data.u32(12) != stride)
            throw Error("Cry data stream kind, count or stride mismatch");
        if (data.u32(0) != 0) throw Error("compressed Cry streams are not supported");
        return data.slice(24, static_cast<size_t>(elements) * stride);
    };
    std::vector<Mesh> meshes;
    std::string material;
    for (const auto& [id, chunk] : chunks) {
        (void)id;
        if (chunk.type == 0x1014) {
            if (chunk.version != 0x800) throw Error("unsupported Cry material-name version; expected 0x800");
            const Reader data(file.slice(chunk.offset, chunk.size));
            material = Name(data.slice(8, 128));
            const size_t children = data.u32(140);
            data.slice(144, children * 4);
        }
    }
    for (const auto& [id, chunk] : chunks) {
        (void)id;
        if (chunk.type != 0x1000) continue;
        if (chunk.version != 0x800) throw Error("unsupported Cry mesh version; expected 0x800");
        const Reader meshData(file.slice(chunk.offset, chunk.size));
        meshData.slice(0, 260);
        if (meshData.u32(0) != 0 || meshData.u32(4) != 0) throw Error("unsupported Cry mesh flags");
        const uint32_t vertices = meshData.u32(8), indices = meshData.u32(12);
        if (vertices == 0 || vertices > 4000000 || indices == 0 || indices > 12000000 || indices % 3 != 0)
            throw Error("invalid or excessive Cry mesh counts");
        if (meshData.u32(16) != 0) throw Error("Cry mesh subsets require material splitting; unsupported");
        Mesh mesh;
        mesh.material = material;
        const Reader positions(stream(meshData.u32(28), 0, vertices, 12));
        for (size_t index = 0; index < vertices; ++index)
            mesh.positions.push_back({positions.f32(index * 12), positions.f32(index * 12 + 4), positions.f32(index * 12 + 8)});
        const auto normalId = meshData.u32(32), uvId = meshData.u32(36);
        if (normalId != 0 && normalId != UINT32_MAX) {
            const Reader normals(stream(normalId, 1, vertices, 12));
            for (size_t index = 0; index < vertices; ++index)
                mesh.normals.push_back({normals.f32(index * 12), normals.f32(index * 12 + 4), normals.f32(index * 12 + 8)});
        }
        if (uvId != 0 && uvId != UINT32_MAX) {
            const Reader uv(stream(uvId, 2, vertices, 8));
            for (size_t index = 0; index < vertices; ++index)
                mesh.uv.push_back({uv.f32(index * 8), uv.f32(index * 8 + 4)});
        }
        const auto indexChunk = chunks.find(meshData.u32(48));
        if (indexChunk == chunks.end()) throw Error("missing Cry indices");
        const Reader indexData(file.slice(indexChunk->second.offset, indexChunk->second.size));
        const uint32_t stride = indexData.u32(12);
        if (stride != 2 && stride != 4) throw Error("Cry indices must be uint16 or uint32");
        const Reader indexStream(stream(meshData.u32(48), 5, indices, stride));
        for (size_t index = 0; index < indices; ++index) {
            const uint32_t value = stride == 2 ? indexStream.u16(index * 2) : indexStream.u32(index * 4);
            if (value >= vertices) throw Error("Cry vertex index outside mesh");
            mesh.indices.push_back(value);
        }
        meshes.push_back(std::move(mesh));
    }
    if (meshes.empty()) throw Error("Cry file has no supported mesh chunks");
    return meshes;
}
} // namespace scuba::assets
