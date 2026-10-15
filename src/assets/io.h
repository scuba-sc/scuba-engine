#pragma once
#include <bit>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace scuba::assets {
using Bytes = std::vector<uint8_t>;
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
constexpr size_t kMaxAssetBytes = 256U * 1024U * 1024U;
Bytes ReadFile(const std::filesystem::path& path, size_t limit = kMaxAssetBytes);
void WriteNewFile(const std::filesystem::path& path, std::span<const uint8_t> bytes);
std::string Text(std::span<const uint8_t> bytes);
Bytes Binary(std::string_view text);

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}
    std::span<const uint8_t> slice(size_t offset, size_t length) const {
        if (offset > data_.size() || length > data_.size() - offset) throw Error("truncated or out-of-bounds asset");
        return data_.subspan(offset, length);
    }
    uint8_t u8(size_t offset) const { return slice(offset, 1)[0]; }
    uint16_t u16(size_t offset) const {
        const auto s = slice(offset, 2);
        return static_cast<uint16_t>(static_cast<uint16_t>(s[0]) | (static_cast<uint16_t>(s[1]) << 8));
    }
    uint32_t u32(size_t offset) const {
        const auto s = slice(offset, 4);
        return uint32_t{s[0]} | (uint32_t{s[1]} << 8) | (uint32_t{s[2]} << 16) | (uint32_t{s[3]} << 24);
    }
    float f32(size_t offset) const;
    size_t size() const { return data_.size(); }
private:
    std::span<const uint8_t> data_;
};
void Put32(Bytes& bytes, uint32_t value);
uint32_t Size32(size_t size);
} // namespace scuba::assets
