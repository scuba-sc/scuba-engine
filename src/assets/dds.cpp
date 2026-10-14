#include "assets/formats.h"
#include <algorithm>

namespace scuba::assets {
namespace {
constexpr uint32_t Four(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
        (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}
}
Dds ReadDds(std::span<const uint8_t> bytes) {
    Reader input(bytes);
    input.slice(0, 128);
    if (input.u32(0) != Four('D','D','S',' ') || input.u32(4) != 124 || input.u32(76) != 32)
        throw Error("invalid DDS magic or header size");
    if ((input.u32(80) & 4U) == 0) throw Error("only block-compressed DDS is supported");
    Dds result;
    result.width = input.u32(16);
    result.height = input.u32(12);
    result.mipCount = std::max(input.u32(28), 1U);
    result.dataOffset = 128;
    if (result.width == 0 || result.height == 0 || result.width > 16384 || result.height > 16384)
        throw Error("DDS dimensions must be 1..16384");
    if (input.u32(24) > 1 || (input.u32(112) & 0x20FE00U) != 0)
        throw Error("DDS cube maps and volume textures are not supported");
    switch (input.u32(84)) {
    case Four('D','X','T','1'): result.dxgi = 71; break;
    case Four('D','X','T','3'): result.dxgi = 74; break;
    case Four('D','X','T','5'): result.dxgi = 77; break;
    case Four('A','T','I','1'):
    case Four('B','C','4','U'): result.dxgi = 80; break;
    case Four('B','C','4','S'): result.dxgi = 81; break;
    case Four('A','T','I','2'):
    case Four('B','C','5','U'): result.dxgi = 83; break;
    case Four('B','C','5','S'): result.dxgi = 84; break;
    case Four('D','X','1','0'):
        input.slice(128, 20);
        result.dataOffset = 148;
        result.dxgi = input.u32(128);
        if (input.u32(132) != 3 || (input.u32(136) & 4U) != 0 || input.u32(140) != 1)
            throw Error("DDS DX10 requires a single 2D texture, not an array or cube");
        break;
    default: throw Error("unknown DDS FourCC");
    }
    size_t blockBytes = 16;
    switch (result.dxgi) {
    case 71: case 72: result.format = "BC1"; blockBytes = 8; break;
    case 74: case 75: result.format = "BC2"; break;
    case 77: case 78: result.format = "BC3"; break;
    case 80: case 81: result.format = "BC4"; blockBytes = 8; break;
    case 83: case 84: result.format = "BC5"; break;
    case 95: case 96: result.format = "BC6H"; break;
    case 98: case 99: result.format = "BC7"; break;
    default: throw Error("unknown or typeless DDS DXGI format; only typed BC1..BC7 accepted");
    }
    uint32_t maxMips = 1;
    for (uint32_t edge = std::max(result.width, result.height); edge > 1; edge >>= 1) ++maxMips;
    if (result.mipCount > maxMips) throw Error("DDS mip count exceeds dimensions");
    size_t offset = result.dataOffset;
    uint32_t width = result.width, height = result.height;
    for (uint32_t level = 0; level < result.mipCount; ++level) {
        const size_t length = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
        input.slice(offset, length);
        offset += length;
        width = std::max(width / 2, 1U);
        height = std::max(height / 2, 1U);
    }
    if (offset != bytes.size()) throw Error("DDS payload size disagrees with mip chain");
    return result;
}
} // namespace scuba::assets
