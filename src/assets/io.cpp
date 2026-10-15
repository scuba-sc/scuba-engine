#include "assets/io.h"
#include <cmath>
#include <fstream>

namespace scuba::assets {
Bytes ReadFile(const std::filesystem::path& path, size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw Error("cannot open input file");
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > limit) throw Error("input exceeds asset size limit");
    Bytes bytes(static_cast<size_t>(end));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw Error("cannot read input file");
    return bytes;
}
void WriteNewFile(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    if (std::filesystem::exists(path)) throw Error("output already exists; choose a new destination");
    std::ofstream output(path, std::ios::binary);
    if (!output) throw Error("cannot create output file");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output.flush()) throw Error("cannot write output file");
}
std::string Text(std::span<const uint8_t> bytes) {
    return std::string(bytes.begin(), bytes.end());
}
Bytes Binary(std::string_view text) { return Bytes(text.begin(), text.end()); }
float Reader::f32(size_t offset) const {
    const float value = std::bit_cast<float>(u32(offset));
    if (!std::isfinite(value)) throw Error("non-finite vertex component");
    return value;
}
void Put32(Bytes& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>((value >> shift) & 255U));
}
uint32_t Size32(size_t size) {
    if (size > UINT32_MAX) throw Error("asset exceeds 32-bit format limit");
    return static_cast<uint32_t>(size);
}
} // namespace scuba::assets
