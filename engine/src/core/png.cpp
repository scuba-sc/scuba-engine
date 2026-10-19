// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/core/png.h"
#include "miniz.h"
#include <cstdio>
#include <cstring>
#include <vector>
namespace scuba {
namespace {
void Big(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
}
void Chunk(std::vector<uint8_t>& out, const char* type, std::span<const uint8_t> bytes) {
    Big(out, static_cast<uint32_t>(bytes.size()));
    const auto first = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), bytes.begin(), bytes.end());
    Big(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, out.data() + first,
                                           static_cast<mz_ulong>(bytes.size() + 4))));
}
}
Status WriteNewRgbaPng(const std::filesystem::path& path, uint32_t width, uint32_t height,
                       std::span<const uint8_t> pixels) {
    if (!width || !height || width > 4096 || height > 4096 ||
        pixels.size() != static_cast<size_t>(width) * height * 4)
        return Status::Fail("Invalid bounded RGBA image");
    const size_t row = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> scanlines((row + 1) * height, 0);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(scanlines.data() + static_cast<size_t>(y) * (row + 1) + 1,
                    pixels.data() + static_cast<size_t>(y) * row, row);
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(scanlines.size()));
    std::vector<uint8_t> compressed(size);
    if (mz_compress2(compressed.data(), &size, scanlines.data(), static_cast<mz_ulong>(scanlines.size()), 6) != MZ_OK)
        return Status::Fail("PNG compression failed");
    compressed.resize(size);
    std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> header;
    Big(header, width); Big(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    Chunk(png, "IHDR", header); Chunk(png, "IDAT", compressed); Chunk(png, "IEND", {});
    FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) file = nullptr;
#else
    file = std::fopen(path.c_str(), "wbx");
#endif
    if (!file) return Status::Fail("Capture output exists or cannot be exclusively created");
    const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::error_code error; std::filesystem::remove(path, error);
        return Status::Fail("Cannot write PNG output");
    }
    return Status::Ok();
}
}
