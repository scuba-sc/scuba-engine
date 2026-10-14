#include "check.h"
#include "asset_fixtures.h"
#include "assets/datacore.h"
#include "hostd/modules.h"
#include <fstream>
#include <functional>
#include <iostream>

using namespace scuba::assets;
namespace {
void Refuses(const std::function<void()>& action) {
    bool refused = false;
    try { action(); } catch (const Error&) { refused = true; }
    CHECK(refused);
}
void Save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(output));
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const auto cry = fixture::Cry(), dds = fixture::Dds(), dcb = fixture::Dcb();
    const auto meshes = ReadCry(cry);
    CHECK(meshes.size() == 1);
    CHECK(meshes.at(0).positions.size() == 3);
    CHECK(meshes.at(0).positions.at(1)[0] == 1.F);
    CHECK(meshes.at(0).indices == std::vector<uint32_t>({0,1,2}));
    const auto glb = MakeGlb(meshes);
    const auto document = GlbDocument(glb);
    CHECK(document.find("meshes")->size() == 1);
    Save(directory / "triangle.cgf", cry); Save(directory / "triangle.cga", cry);
    Save(directory / "triangle.glb", glb); Save(directory / "red.dds", dds);
    Save(directory / "tiny.dcb", dcb);
    auto jsonImport = document;
    auto buffers = scuba::hostd::Json::MakeArray();
    const Reader glbReader(glb);
    const size_t binaryChunk = 20 + glbReader.u32(12);
    const auto binary = glbReader.slice(binaryChunk + 8, glbReader.u32(binaryChunk));
    buffers.push_back(scuba::hostd::Json::MakeObject().set("uri", "triangle.bin").set("byteLength", binary.size()));
    jsonImport.set("buffers", buffers);
    Save(directory / "triangle.gltf", Binary(jsonImport.dump()));
    Save(directory / "triangle.bin", Bytes(binary.begin(), binary.end()));
    CHECK(GlbDocument(ReadGltf(directory / "triangle.gltf")).find("meshes")->size() == 1);
    auto badBuffers = scuba::hostd::Json::MakeArray();
    badBuffers.push_back(scuba::hostd::Json::MakeObject().set("uri", "../outside.bin").set("byteLength", binary.size()));
    jsonImport.set("buffers", badBuffers);
    Save(directory / "escape.gltf", Binary(jsonImport.dump()));
    Refuses([&] { (void)ReadGltf(directory / "escape.gltf"); });
    auto invalidFloat = glb;
    fixture::Set32(invalidFloat, binaryChunk + 8, 0x7F800000);
    Refuses([&] { (void)GlbDocument(invalidFloat); });
    for (const uint32_t dxgi : {71U,72U,74U,75U,77U,78U,80U,81U,83U,84U,95U,96U,98U,99U}) {
        const auto texture = fixture::Dds(dxgi);
        CHECK(ReadDds(texture).dxgi == dxgi);
        Save(directory / ("bc-" + std::to_string(dxgi) + ".dds"), texture);
    }
    CHECK(ReadDds(dds).format == "BC1");
    const auto textureGlb = Convert(directory / "red.dds");
    CHECK(GlbDocument(textureGlb).find("images")->size() == 1);
    Save(directory / "texture.glb", textureGlb);
    for (size_t length = 0; length < cry.size(); ++length)
        Refuses([&] { (void)ReadCry(std::span<const uint8_t>(cry).first(length)); });
    for (size_t length = 0; length < dds.size(); ++length)
        Refuses([&] { (void)ReadDds(std::span<const uint8_t>(dds).first(length)); });
    for (size_t length = 0; length < glb.size(); ++length)
        Refuses([&] { (void)GlbDocument(std::span<const uint8_t>(glb).first(length)); });
    auto corrupt = cry;
    fixture::Set32(corrupt, 4, 0x999);
    Refuses([&] { (void)ReadCry(corrupt); });
    corrupt = cry; fixture::Set32(corrupt, 28, UINT32_MAX);
    Refuses([&] { (void)ReadCry(corrupt); });
    corrupt = cry; fixture::Set32(corrupt, 24, UINT32_MAX);
    Refuses([&] { (void)ReadCry(corrupt); });
    corrupt = cry; fixture::Set16(corrupt, 18, 0x801);
    Refuses([&] { (void)ReadCry(corrupt); });
    corrupt = cry; fixture::Set32(corrupt, 44, 64);
    Refuses([&] { (void)ReadCry(corrupt); });
    corrupt = cry; fixture::Set16(corrupt, corrupt.size() - 2, 10);
    Refuses([&] { (void)ReadCry(corrupt); });
    corrupt = fixture::Dds(99); fixture::Set32(corrupt, 140, 2);
    Refuses([&] { (void)ReadDds(corrupt); });
    for (size_t byte = 0; byte < cry.size(); ++byte) {
        corrupt = cry;
        corrupt[byte] ^= 0xFFU;
        try { (void)ReadCry(corrupt); } catch (const Error&) {}
    }
    corrupt = dds; fixture::Set32(corrupt, 28, UINT32_MAX);
    Refuses([&] { (void)ReadDds(corrupt); });
    corrupt = fixture::Dds(99); fixture::Set32(corrupt, 128, 1000);
    Refuses([&] { (void)ReadDds(corrupt); });
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf -3/1 -2/2 -1/3\n";
    const auto imported = ReadObj(obj);
    CHECK(imported.positions.size() == 3 && imported.uv.size() == 3);
    Refuses([&] { (void)ReadObj("v 0 0 0\nf 0 1 2\n"); });
    Refuses([&] { (void)ReadObj("v 0 0 0\nf 1 2 3\n"); });
    Save(directory / "triangle.obj", Binary(obj));
    CHECK(GlbDocument(Convert(directory / "triangle.obj")).find("meshes")->size() == 1);
    DataCore data;
    const auto summary = data.Open(directory / "tiny.dcb");
    CHECK(summary.find("records")->as_number() == 2);
    CHECK(data.Structs().size() == 1);
    CHECK(data.Records(0, 1, "").find("items")->size() == 1);
    CHECK(data.Records(0, 100, "TriangleB").find("total")->as_number() == 1);
    const auto fields = data.Fields("TriangleA");
    CHECK(fields.size() == 2);
    CHECK(std::stod(fields.at(0).find("value")->as_string()) == 10.0);
    CHECK(fields.at(1).find("reference") != nullptr);
    if (const auto* reference = fields.at(1).find("reference"))
        CHECK(reference->find("name")->as_string() == "TriangleB");
    data.SetPack("format = 1\n[[set]]\nrecord = \"TriangleA\"\nfield = \"speed\"\nvalue = 42.5\n");
    Save(directory / "export.toml", Binary(data.Export()));
    Refuses([&] { data.SetPack("format = 1\n[[set]]\nrecord = \"Missing\"\nfield = \"speed\"\nvalue = 1\n"); });
    Refuses([&] { data.SetPack("format = 1\n[[set]]\nrecord = \"TriangleA\"\nfield = \"speed\"\nvalue = \"bad\"\n"); });
    for (size_t length = 0; length < dcb.size(); ++length) {
        sco::datacore::Schema schema;
        CHECK(!schema.Parse(std::span<const uint8_t>(dcb).first(length)));
    }
    const auto packPath = directory / "imported-pack";
    if (std::filesystem::exists(packPath)) std::filesystem::remove_all(packPath);
    ImportPack(directory / "triangle.obj", packPath, "objects/synthetic.glb", data.Export());
    CHECK(std::filesystem::exists(packPath / "files/objects/synthetic.glb"));
    CHECK(std::filesystem::exists(packPath / "datacore/overrides.toml"));
    Refuses([&] { ImportPack(directory / "triangle.obj", packPath, "objects/synthetic.glb", data.Export()); });
    Refuses([&] { ImportPack(directory / "triangle.obj", directory / "bad-pack", "../escape.glb", data.Export()); });
    Refuses([&] { ImportPack(directory / "triangle.obj", {}, "objects/synthetic.glb", data.Export()); });
    const auto tomlSummary = data.Open(directory / "export.toml");
    CHECK(tomlSummary.find("kind")->as_string() == "toml");
    CHECK(data.Records(0, 100, "").find("total")->as_number() == 1);
    scuba::hostd::Registry registry;
    scuba::hostd::HostContext host;
    scuba::hostd::RegisterAssetsHandlers(registry, host);
    const auto request = scuba::hostd::Json::MakeObject().set("id", 1).set("method", "assets.open")
        .set("params", scuba::hostd::Json::MakeObject().set("path", (directory / "tiny.dcb").string()));
    CHECK(registry.dispatch(request, {}).find("result") != nullptr);
    const auto bad = scuba::hostd::Json::MakeObject().set("id", 2).set("method", "assets.records")
        .set("params", scuba::hostd::Json::MakeObject().set("limit", -1));
    CHECK(registry.dispatch(bad, {}).find("error")->find("code")->as_number() == -32602);
    std::cout << "Synthetic fixtures written for GLTFLoader and sco-dcb verification\n";
    return CheckExit("test_assets");
}
