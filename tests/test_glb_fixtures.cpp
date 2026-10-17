// Reads the GLB files that test_assets writes into assets-fixtures with a small, separate reader
// written from the glTF 2.0 container and accessor rules, not with scuba_assets' own decoder. It
// checks the geometry, bounds and the embedded DDS image the way a third-party loader would see them.
#include "check.h"
#include "hostd/json.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using scuba::hostd::Json;
namespace {
using Bytes = std::vector<uint8_t>;

Bytes Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
uint32_t U32(const Bytes& b, size_t at) {
    CHECK(at + 4 <= b.size());
    uint32_t v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);  // little-endian hosts only
    return v;
}
struct Glb {
    Json json;
    Bytes bin;
};
bool Parse(const Bytes& b, Glb& out) {
    if (b.size() < 20 || U32(b, 0) != 0x46546C67U || U32(b, 4) != 2 || U32(b, 8) != b.size()) return false;
    const uint32_t jsonLength = U32(b, 12);
    if (U32(b, 16) != 0x4E4F534AU || 20ULL + jsonLength + 8 > b.size()) return false;
    std::string error;
    auto parsed = Json::parse(std::string_view(reinterpret_cast<const char*>(b.data()) + 20, jsonLength), &error);
    if (!parsed) return false;
    out.json = *parsed;
    const size_t binHeader = 20 + jsonLength;
    const uint32_t binLength = U32(b, binHeader);
    if (U32(b, binHeader + 4) != 0x004E4942U || binHeader + 8ULL + binLength > b.size()) return false;
    out.bin.assign(b.begin() + static_cast<std::ptrdiff_t>(binHeader + 8),
                   b.begin() + static_cast<std::ptrdiff_t>(binHeader + 8 + binLength));
    return true;
}
const Json& Get(const Json& object, const char* key) {
    const Json* value = object.find(key);
    CHECK(value != nullptr);
    static const Json empty;
    return value ? *value : empty;
}
size_t Num(const Json& object, const char* key, size_t fallback = 0) {
    const Json* value = object.find(key);
    return value ? static_cast<size_t>(value->as_number()) : fallback;
}
// Start of an accessor's data inside the BIN chunk, after checking it fits its bufferView.
size_t AccessorOffset(const Glb& glb, const Json& accessor, size_t elementBytes) {
    const Json& views = Get(glb.json, "bufferViews");
    const Json& v = views.at(Num(accessor, "bufferView"));
    const size_t start = Num(v, "byteOffset") + Num(accessor, "byteOffset");
    const size_t end = start + Num(accessor, "count") * elementBytes;
    CHECK(end <= Num(v, "byteOffset") + Num(v, "byteLength"));
    CHECK(end <= glb.bin.size());
    return start;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path dir(argv[1]);

    Glb triangle;
    CHECK(Parse(Load(dir / "triangle.glb"), triangle));
    const Json& meshes = Get(triangle.json, "meshes");
    CHECK(meshes.size() == 1);
    const Json& primitive = Get(meshes.at(0), "primitives").at(0);
    const Json& accessors = Get(triangle.json, "accessors");
    const Json& position = accessors.at(Num(Get(primitive, "attributes"), "POSITION"));
    CHECK(Num(position, "componentType") == 5126);  // FLOAT
    CHECK(Get(position, "type").as_string() == "VEC3");
    CHECK(Num(position, "count") == 3);
    const size_t positionAt = AccessorOffset(triangle, position, 12);
    std::array<float, 9> p{};
    std::memcpy(p.data(), triangle.bin.data() + positionAt, sizeof p);
    CHECK((p == std::array<float, 9>{0, 0, 0, 1, 0, 0, 0, 1, 0}));

    // Declared min/max (required for POSITION) must match the data.
    const Json& lo = Get(position, "min");
    const Json& hi = Get(position, "max");
    CHECK(lo.size() == 3 && hi.size() == 3);
    for (size_t axis = 0; axis < 3 && lo.size() == 3 && hi.size() == 3; ++axis) {
        float mn = p[axis], mx = p[axis];
        for (size_t v = 1; v < 3; ++v) { mn = std::min(mn, p[v * 3 + axis]); mx = std::max(mx, p[v * 3 + axis]); }
        CHECK(lo.at(axis).as_number() == static_cast<double>(mn));
        CHECK(hi.at(axis).as_number() == static_cast<double>(mx));
    }

    const Json& indices = accessors.at(Num(primitive, "indices"));
    CHECK(Get(indices, "type").as_string() == "SCALAR");
    CHECK(Num(indices, "count") == 3);
    const size_t component = Num(indices, "componentType");
    CHECK(component == 5125 || component == 5123);  // UNSIGNED_INT / UNSIGNED_SHORT
    const size_t width = component == 5125 ? 4 : 2;
    const size_t indexAt = AccessorOffset(triangle, indices, width);
    for (size_t i = 0; i < 3; ++i) {
        uint32_t value = 0;
        std::memcpy(&value, triangle.bin.data() + indexAt + i * width, width);
        CHECK(value == i);
    }

    Glb textured;
    CHECK(Parse(Load(dir / "texture.glb"), textured));
    const Json& images = Get(textured.json, "images");
    CHECK(images.size() == 1);
    CHECK(images.at(0).find("uri") == nullptr);  // embedded, no network or file reference
    const Json& view = Get(textured.json, "bufferViews").at(Num(images.at(0), "bufferView"));
    const size_t imageAt = Num(view, "byteOffset"), imageLength = Num(view, "byteLength");
    CHECK(imageAt + imageLength <= textured.bin.size());
    // DDS container: "DDS ", 124-byte header, height at 12, width at 16, mip count at 28, then one BC1 4x4 block.
    CHECK(imageLength >= 132);
    if (imageLength >= 132) {
        const Bytes dds(textured.bin.begin() + static_cast<std::ptrdiff_t>(imageAt),
                        textured.bin.begin() + static_cast<std::ptrdiff_t>(imageAt + imageLength));
        CHECK(U32(dds, 0) == 0x20534444U);
        CHECK(U32(dds, 12) == 4 && U32(dds, 16) == 4);
        CHECK(U32(dds, 28) == 1);
        CHECK(imageLength - 128 == 8);
    }
    return CheckExit("test_glb_fixtures");
}
