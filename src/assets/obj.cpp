#include "assets/formats.h"
#include <charconv>
#include <cmath>
#include <sstream>

namespace scuba::assets {
namespace {
size_t Index(std::string_view value, size_t count) {
    int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0 ||
        count > static_cast<size_t>(INT64_MAX)) throw Error("invalid OBJ index");
    const int64_t index = parsed > 0 ? parsed - 1 : static_cast<int64_t>(count) + parsed;
    if (index < 0 || static_cast<uint64_t>(index) >= count) throw Error("OBJ index outside source array");
    return static_cast<size_t>(index);
}
template <size_t N> std::array<float, N> Vector(std::istringstream& line) {
    std::array<float, N> value{};
    for (auto& component : value)
        if (!(line >> component) || !std::isfinite(component)) throw Error("invalid OBJ numeric component");
    return value;
}
}
Mesh ReadObj(std::string_view text) {
    std::istringstream input{std::string(text)};
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> uv;
    Mesh mesh;
    bool allNormals = true, allUv = true;
    std::string raw;
    while (std::getline(input, raw)) {
        if (raw.size() > 1024 * 1024) throw Error("OBJ line exceeds limit");
        std::istringstream line(raw);
        std::string kind;
        line >> kind;
        if (kind == "v") positions.push_back(Vector<3>(line));
        else if (kind == "vn") normals.push_back(Vector<3>(line));
        else if (kind == "vt") uv.push_back(Vector<2>(line));
        else if (kind == "f") {
            std::vector<uint32_t> face;
            std::string token;
            while (line >> token && !token.starts_with("#")) {
                const size_t first = token.find('/'), second = first == std::string::npos ? first : token.find('/', first + 1);
                mesh.positions.push_back(positions.at(Index(std::string_view(token).substr(0, first), positions.size())));
                if (first != std::string::npos && second != first + 1 && first + 1 < token.size())
                    mesh.uv.push_back(uv.at(Index(std::string_view(token).substr(first + 1, second == std::string::npos ? second : second - first - 1), uv.size())));
                else allUv = false;
                if (second != std::string::npos && second + 1 < token.size())
                    mesh.normals.push_back(normals.at(Index(std::string_view(token).substr(second + 1), normals.size())));
                else allNormals = false;
                face.push_back(Size32(mesh.positions.size() - 1));
                if (mesh.positions.size() > 4000000) throw Error("OBJ vertex limit exceeded");
            }
            if (face.size() < 3) throw Error("OBJ face needs at least three vertices");
            for (size_t index = 1; index + 1 < face.size(); ++index) {
                mesh.indices.insert(mesh.indices.end(), {face[0], face[index], face[index + 1]});
                if (mesh.indices.size() > 12000000) throw Error("OBJ triangle limit exceeded");
            }
        } else if (!kind.empty() && !kind.starts_with("#") && kind != "o" && kind != "g" && kind != "s" &&
                   kind != "usemtl" && kind != "mtllib") throw Error("unsupported OBJ statement: " + kind);
    }
    if (mesh.indices.empty()) throw Error("OBJ has no faces");
    if (!allNormals) mesh.normals.clear();
    if (!allUv) mesh.uv.clear();
    return mesh;
}
} // namespace scuba::assets
