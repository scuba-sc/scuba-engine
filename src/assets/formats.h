#pragma once
#include "assets/io.h"
#include "hostd/json.h"
#include <array>

namespace scuba::assets {
struct Mesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uv;
    std::vector<uint32_t> indices;
    std::string material;
};
struct Dds {
    uint32_t width = 0, height = 0, mipCount = 0, dxgi = 0;
    size_t dataOffset = 0;
    std::string format;
};
Dds ReadDds(std::span<const uint8_t> bytes);
std::vector<Mesh> ReadCry(std::span<const uint8_t> bytes);
Mesh ReadObj(std::string_view text);
Bytes MakeGlb(const std::vector<Mesh>& meshes, std::span<const uint8_t> dds = {});
Bytes ReadGltf(const std::filesystem::path& path);
hostd::Json GlbDocument(std::span<const uint8_t> bytes);
Bytes Convert(const std::filesystem::path& path);
void ImportPack(const std::filesystem::path& source, const std::filesystem::path& destination,
                std::string_view virtualPath, std::string_view packText);
} // namespace scuba::assets
