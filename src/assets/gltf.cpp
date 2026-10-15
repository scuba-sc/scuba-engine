#include "assets/formats.h"
#include "sco/datacore_pack.h"
#include <algorithm>
#include <cmath>

namespace scuba::assets {
using hostd::Json;
namespace {
Json Array(std::initializer_list<Json> values) {
    Json result = Json::MakeArray();
    for (const auto& value : values) result.push_back(value);
    return result;
}
const Json& Member(const Json& value, std::string_view key) {
    const auto* found = value.find(key);
    if (!found) throw Error("missing glTF property: " + std::string(key));
    return *found;
}
size_t Number(const Json& value) {
    const double number = value.as_number(-1);
    if (!std::isfinite(number) || number < 0 || std::floor(number) != number || number > kMaxAssetBytes)
        throw Error("invalid or excessive glTF integer");
    return static_cast<size_t>(number);
}
size_t Number(const Json& value, std::string_view key, size_t fallback = 0) {
    const auto* found = value.find(key);
    return found ? Number(*found) : fallback;
}
void Align(Bytes& buffer) { while (buffer.size() % 4 != 0) buffer.push_back(0); }
Bytes Encode(Json document, Bytes binary) {
    document.set("buffers", Array({Json::MakeObject().set("byteLength", binary.size())}));
    std::string json = document.dump();
    while (json.size() % 4 != 0) json += ' ';
    Align(binary);
    if (json.size() + binary.size() + 28 > kMaxAssetBytes) throw Error("glTF output exceeds asset limit");
    Bytes output;
    Put32(output, 0x46546C67);
    Put32(output, 2);
    Put32(output, Size32(json.size() + binary.size() + 28));
    Put32(output, Size32(json.size()));
    Put32(output, 0x4E4F534A);
    output.insert(output.end(), json.begin(), json.end());
    Put32(output, Size32(binary.size()));
    Put32(output, 0x004E4942);
    output.insert(output.end(), binary.begin(), binary.end());
    return output;
}
void Validate(const Json& document, std::span<const uint8_t> binary) {
    if (Member(Member(document, "asset"), "version").as_string() != "2.0") throw Error("only glTF 2.0 is supported");
    const auto& buffers = Member(document, "buffers");
    if (!buffers.is_array() || buffers.size() != 1 || buffers.at(0).find("uri")) throw Error("GLB must have one embedded buffer");
    const size_t length = Number(Member(buffers.at(0), "byteLength"));
    if (length > binary.size() || binary.size() - length > 3) throw Error("glTF embedded buffer length mismatch");
    const Reader data(binary.first(length));
    const auto& views = Member(document, "bufferViews");
    if (!views.is_array()) throw Error("glTF bufferViews must be an array");
    for (size_t index = 0; index < views.size(); ++index) {
        const auto& view = views.at(index);
        if (Number(view, "buffer") != 0) throw Error("glTF bufferView refers to missing buffer");
        data.slice(Number(view, "byteOffset"), Number(Member(view, "byteLength")));
        if (const auto* extensions = view.find("extensions"); extensions && extensions->size() != 0)
            throw Error("compressed glTF bufferViews are unsupported");
    }
    const auto& accessors = Member(document, "accessors");
    if (!accessors.is_array()) throw Error("glTF accessors must be an array");
    for (size_t index = 0; index < accessors.size(); ++index) {
        const auto& accessor = accessors.at(index);
        if (accessor.find("sparse")) throw Error("sparse glTF accessors are unsupported");
        const size_t viewIndex = Number(Member(accessor, "bufferView"));
        if (viewIndex >= views.size()) throw Error("glTF accessor refers to missing bufferView");
        const auto& view = views.at(viewIndex);
        const size_t component = Number(Member(accessor, "componentType"));
        const size_t width = component == 5120 || component == 5121 ? 1 : component == 5122 || component == 5123 ? 2 :
            component == 5125 || component == 5126 ? 4 : 0;
        const std::string_view type = Member(accessor, "type").as_string();
        const size_t components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 0;
        if (width == 0 || components == 0) throw Error("unsupported glTF accessor type (only scalar/vectors)");
        const size_t element = width * components, stride = Number(view, "byteStride", element);
        const size_t count = Number(Member(accessor, "count")), offset = Number(accessor, "byteOffset");
        const size_t viewLength = Number(Member(view, "byteLength"));
        if (count == 0 || count > 12000000 || stride < element || stride > 252 || stride % width != 0 ||
            (view.find("byteStride") && stride % 4 != 0) || (Number(view, "byteOffset") + offset) % width != 0 ||
            offset > viewLength || element > viewLength - offset ||
            (count - 1) > (viewLength - offset - element) / stride)
            throw Error("glTF accessor exceeds bufferView or has invalid stride");
        const size_t absoluteOffset = Number(view, "byteOffset") + offset;
        if (component == 5126)
            for (size_t item = 0; item < count; ++item)
                for (size_t axis = 0; axis < components; ++axis)
                    (void)data.f32(absoluteOffset + item * stride + axis * width);
    }
    if (const auto* images = document.find("images")) {
        for (size_t index = 0; index < images->size(); ++index) {
            const auto& image = images->at(index);
            if (image.find("uri") || Number(Member(image, "bufferView")) >= views.size())
                throw Error("glTF image must use an embedded bufferView");
        }
    }
    if (const auto* required = document.find("extensionsRequired")) {
        for (size_t index = 0; index < required->size(); ++index)
            if (required->at(index).as_string() != "MSFT_texture_dds") throw Error("unsupported required glTF extension");
    }
    const auto& meshes = Member(document, "meshes");
    if (!meshes.is_array() || meshes.size() == 0) throw Error("glTF has no meshes");
    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
        const auto& primitives = Member(meshes.at(meshIndex), "primitives");
        if (!primitives.is_array() || primitives.size() == 0) throw Error("glTF mesh has no primitives");
        for (size_t index = 0; index < primitives.size(); ++index) {
            const auto& primitive = primitives.at(index);
            if (Number(primitive, "mode", 4) != 4 || primitive.find("targets")) throw Error("only static glTF triangles are supported");
            const auto& attributes = Member(primitive, "attributes");
            const size_t position = Number(Member(attributes, "POSITION"));
            if (position >= accessors.size() || Member(accessors.at(position), "type").as_string() != "VEC3" ||
                Number(Member(accessors.at(position), "componentType")) != 5126) throw Error("glTF needs float32 VEC3 positions");
            for (size_t attribute = 0; attribute < attributes.size(); ++attribute)
                if (Number(attributes.at(attribute)) >= accessors.size()) throw Error("glTF attribute accessor is missing");
            if (const auto* indices = primitive.find("indices")) {
                const size_t ai = Number(*indices);
                if (ai >= accessors.size()) throw Error("glTF indices accessor is missing");
                const auto& accessor = accessors.at(ai);
                const size_t component = Number(Member(accessor, "componentType"));
                if (Member(accessor, "type").as_string() != "SCALAR" || (component != 5121 && component != 5123 && component != 5125))
                    throw Error("glTF indices must be unsigned scalars");
                const auto& view = views.at(Number(Member(accessor, "bufferView")));
                const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
                const size_t count = Number(Member(accessor, "count"));
                if (count % 3 != 0) throw Error("glTF triangle index count must be divisible by three");
                for (size_t item = 0; item < count; ++item) {
                    const size_t offset = Number(view, "byteOffset") + Number(accessor, "byteOffset") + item * Number(view, "byteStride", width);
                    const uint32_t vertex = width == 1 ? data.u8(offset) : width == 2 ? data.u16(offset) : data.u32(offset);
                    if (vertex >= Number(Member(accessors.at(position), "count"))) throw Error("glTF index outside positions");
                }
            }
        }
    }
    if (document.find("skins") || document.find("animations")) throw Error("skinned or animated glTF import is unsupported");
}
}
Bytes MakeGlb(const std::vector<Mesh>& meshes, std::span<const uint8_t> dds) {
    if (meshes.empty()) throw Error("cannot export an empty mesh list");
    Json document = Json::MakeObject().set("asset", Json::MakeObject().set("version", "2.0").set("generator", "scuba Asset Lab"));
    Json views = Json::MakeArray(), accessors = Json::MakeArray(), meshJson = Json::MakeArray(), nodes = Json::MakeArray(), sceneNodes = Json::MakeArray();
    Bytes binary;
    auto view = [&](size_t offset, size_t length) {
        const size_t index = views.size();
        views.push_back(Json::MakeObject().set("buffer", 0).set("byteOffset", offset).set("byteLength", length));
        return index;
    };
    for (const auto& mesh : meshes) {
        if (mesh.positions.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0) throw Error("invalid triangle mesh");
        Json attributes = Json::MakeObject();
        auto vectors = [&](const auto& values, const char* semantic, const char* type, size_t components) {
            Align(binary);
            const size_t start = binary.size(), accessorIndex = accessors.size();
            std::array<float, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
            for (const auto& value : values) {
                for (size_t axis = 0; axis < components; ++axis) {
                    if (!std::isfinite(value[axis])) throw Error("non-finite glTF vertex");
                    Put32(binary, std::bit_cast<uint32_t>(value[axis]));
                    low[axis] = std::min(low[axis], value[axis]);
                    high[axis] = std::max(high[axis], value[axis]);
                }
            }
            Json accessor = Json::MakeObject().set("bufferView", view(start, binary.size() - start))
                .set("componentType", 5126).set("count", values.size()).set("type", type);
            if (std::string_view(semantic) == "POSITION") accessor.set("min", Array({low[0], low[1], low[2]})).set("max", Array({high[0], high[1], high[2]}));
            accessors.push_back(std::move(accessor));
            attributes.set(semantic, accessorIndex);
        };
        vectors(mesh.positions, "POSITION", "VEC3", 3);
        if (!mesh.normals.empty()) {
            if (mesh.normals.size() != mesh.positions.size()) throw Error("mesh normal count mismatch");
            vectors(mesh.normals, "NORMAL", "VEC3", 3);
        }
        if (!mesh.uv.empty()) {
            if (mesh.uv.size() != mesh.positions.size()) throw Error("mesh UV count mismatch");
            vectors(mesh.uv, "TEXCOORD_0", "VEC2", 2);
        }
        const size_t indexStart = binary.size(), indexAccessor = accessors.size();
        for (const auto index : mesh.indices) {
            if (index >= mesh.positions.size()) throw Error("mesh index outside positions");
            Put32(binary, index);
        }
        accessors.push_back(Json::MakeObject().set("bufferView", view(indexStart, binary.size() - indexStart))
            .set("componentType", 5125).set("count", mesh.indices.size()).set("type", "SCALAR"));
        Json primitive = Json::MakeObject().set("attributes", std::move(attributes)).set("indices", indexAccessor).set("mode", 4);
        if (!dds.empty()) {
            if (mesh.uv.empty()) throw Error("DDS textured mesh requires UV coordinates");
            primitive.set("material", 0);
        }
        sceneNodes.push_back(nodes.size());
        nodes.push_back(Json::MakeObject().set("mesh", meshJson.size()));
        meshJson.push_back(Json::MakeObject().set("primitives", Array({primitive})).set("name", mesh.material.empty() ? "mesh" : mesh.material));
    }
    if (!dds.empty()) {
        (void)ReadDds(dds);
        Align(binary);
        const size_t imageView = view(binary.size(), dds.size());
        binary.insert(binary.end(), dds.begin(), dds.end());
        document.set("images", Array({Json::MakeObject().set("bufferView", imageView).set("mimeType", "image/vnd-ms.dds")}));
        document.set("textures", Array({Json::MakeObject().set("extensions", Json::MakeObject()
            .set("MSFT_texture_dds", Json::MakeObject().set("source", 0)))}));
        document.set("extensionsUsed", Array({"MSFT_texture_dds"})).set("extensionsRequired", Array({"MSFT_texture_dds"}));
        document.set("materials", Array({Json::MakeObject().set("doubleSided", true).set("alphaMode", "BLEND").set("pbrMetallicRoughness",
            Json::MakeObject().set("baseColorTexture", Json::MakeObject().set("index", 0)).set("metallicFactor", 0).set("roughnessFactor", 1))}));
    }
    document.set("bufferViews", std::move(views)).set("accessors", std::move(accessors)).set("meshes", std::move(meshJson))
        .set("nodes", std::move(nodes)).set("scenes", Array({Json::MakeObject().set("nodes", sceneNodes)})).set("scene", 0);
    return Encode(std::move(document), std::move(binary));
}
Json GlbDocument(std::span<const uint8_t> bytes) {
    const Reader file(bytes);
    file.slice(0, 20);
    if (file.u32(0) != 0x46546C67 || file.u32(4) != 2 || file.u32(8) != bytes.size() ||
        file.u32(16) != 0x4E4F534A || file.u32(12) % 4 != 0) throw Error("invalid GLB 2.0 header or JSON chunk");
    const size_t jsonLength = file.u32(12);
    std::string error;
    auto document = Json::parse(Text(file.slice(20, jsonLength)), &error);
    if (!document || !document->is_object()) throw Error("invalid glTF JSON: " + error);
    const size_t chunk = 20 + jsonLength;
    file.slice(chunk, 8);
    const size_t length = file.u32(chunk);
    if (file.u32(chunk + 4) != 0x004E4942 || length % 4 != 0 || length != bytes.size() - chunk - 8)
        throw Error("GLB requires one complete BIN chunk");
    Validate(*document, file.slice(chunk + 8, length));
    return *document;
}
Bytes ReadGltf(const std::filesystem::path& path) {
    auto bytes = ReadFile(path);
    if (path.extension() == ".glb") { (void)GlbDocument(bytes); return bytes; }
    std::string error;
    auto document = Json::parse(Text(bytes), &error);
    if (!document || !document->is_object()) throw Error("invalid glTF JSON: " + error);
    const auto& buffers = Member(*document, "buffers");
    if (!buffers.is_array() || buffers.size() != 1) throw Error("glTF import requires one local .bin buffer");
    const std::string_view uri = Member(buffers.at(0), "uri").as_string();
    const std::filesystem::path relative(uri);
    if (uri.empty() || uri.find_first_of(":%\\?#") != std::string_view::npos || relative.is_absolute())
        throw Error("glTF buffer must be a relative local .bin file (no data or network URI)");
    for (const auto& part : relative)
        if (part == "..") throw Error("glTF buffer path escapes model directory");
    const auto parent = std::filesystem::canonical(path.parent_path().empty() ? "." : path.parent_path());
    const auto input = std::filesystem::canonical(parent / relative);
    for (const auto& part : input.lexically_relative(parent))
        if (part == "..") throw Error("glTF buffer symlink escapes model directory");
    auto binary = ReadFile(input);
    if (Number(Member(buffers.at(0), "byteLength")) != binary.size()) throw Error("glTF buffer length mismatch");
    // Image bufferViews are supported; external images require explicit embedding by the author.
    document->set("buffers", Array({Json::MakeObject().set("byteLength", binary.size())}));
    Validate(*document, binary);
    return Encode(std::move(*document), std::move(binary));
}
Bytes Convert(const std::filesystem::path& path) {
    const auto extension = path.extension();
    if (extension == ".glb" || extension == ".gltf") return ReadGltf(path);
    const auto bytes = ReadFile(path);
    if (extension == ".obj") return MakeGlb({ReadObj(Text(bytes))});
    if (extension == ".cgf" || extension == ".cga") return MakeGlb(ReadCry(bytes));
    if (extension == ".dds") {
        Mesh plane;
        plane.positions = {{{-1,-1,0}}, {{1,-1,0}}, {{1,1,0}}, {{-1,1,0}}};
        plane.uv = {{{0,1}}, {{1,1}}, {{1,0}}, {{0,0}}};
        plane.indices = {0,1,2,0,2,3};
        return MakeGlb({plane}, bytes);
    }
    throw Error("unsupported asset extension; use .cgf, .cga, .dds, .glb, .gltf or .obj");
}
void ImportPack(const std::filesystem::path& source, const std::filesystem::path& destination,
                std::string_view virtualPath, std::string_view packText) {
    if (destination.empty()) throw Error("pack destination must not be empty");
    if (source.extension() != ".glb" && source.extension() != ".gltf" && source.extension() != ".obj")
        throw Error("import requires .glb, .gltf or .obj");
    if (virtualPath.empty() || virtualPath.size() > 240 || virtualPath.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-/ .") != std::string_view::npos ||
        virtualPath.front() == '/' || virtualPath.find("..") != std::string_view::npos || !virtualPath.ends_with(".glb"))
        throw Error("virtual path must be a relative lowercase .glb path without traversal");
    if (std::filesystem::exists(destination)) throw Error("pack destination already exists; nothing overwritten");
    sco::datacore::Pack pack;
    std::string error;
    if (!sco::datacore::ParsePack(packText, pack, error)) throw Error("override pack refused: " + error);
    const auto glb = Convert(source);
    const std::string canonical = sco::datacore::WritePack(pack);
    const auto output = destination / "files" / std::filesystem::path(virtualPath);
    try {
        std::filesystem::create_directories(output.parent_path());
        std::filesystem::create_directories(destination / "datacore");
        WriteNewFile(output, glb);
        WriteNewFile(destination / "datacore" / "overrides.toml", Binary(canonical));
    } catch (...) {
        // Destination did not exist before this operation, so only this import's partial tree is removed.
        std::filesystem::remove_all(destination);
        throw;
    }
}
} // namespace scuba::assets
