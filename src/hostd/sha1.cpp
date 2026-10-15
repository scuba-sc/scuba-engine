#include "hostd/sha1.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace scuba::hostd {

namespace {

inline uint32_t Rotl(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

inline uint32_t ReadBe32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
#if defined(__GNUG__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
#endif
}

inline uint32_t F0(uint32_t b, uint32_t c, uint32_t d) { return d ^ (b & (c ^ d)); }
inline uint32_t F1(uint32_t b, uint32_t c, uint32_t d) { return b ^ c ^ d; }
inline uint32_t F2(uint32_t b, uint32_t c, uint32_t d) { return (b & c) | (d & (b ^ c)); }
inline uint32_t F3(uint32_t b, uint32_t c, uint32_t d) { return b ^ c ^ d; }

#define SHA1_STEP(f, k, v, w, x, y, z, i) \
    z += Rotl(v, 5) + f(w, x, y) + k + w_arr[i]; \
    w = Rotl(w, 30)

#define SHA1_ROUND5(f, k, i) \
    SHA1_STEP(f, k, a, b, c, d, e, i); \
    SHA1_STEP(f, k, e, a, b, c, d, (i) + 1); \
    SHA1_STEP(f, k, d, e, a, b, c, (i) + 2); \
    SHA1_STEP(f, k, c, d, e, a, b, (i) + 3); \
    SHA1_STEP(f, k, b, c, d, e, a, (i) + 4)

void ProcessBlock(uint32_t h[5], const uint8_t* block) {
    uint32_t w_arr[80];
    for (size_t i = 0; i < 16; ++i) {
        w_arr[i] = ReadBe32(block + i * 4);
    }
    for (size_t i = 16; i < 80; ++i) {
        w_arr[i] = Rotl(w_arr[i - 3] ^ w_arr[i - 8] ^ w_arr[i - 14] ^ w_arr[i - 16], 1);
    }

    uint32_t a = h[0];
    uint32_t b = h[1];
    uint32_t c = h[2];
    uint32_t d = h[3];
    uint32_t e = h[4];

    // Rounds 0-19
    SHA1_ROUND5(F0, 0x5A827999u, 0);
    SHA1_ROUND5(F0, 0x5A827999u, 5);
    SHA1_ROUND5(F0, 0x5A827999u, 10);
    SHA1_ROUND5(F0, 0x5A827999u, 15);

    // Rounds 20-39
    SHA1_ROUND5(F1, 0x6ED9EBA1u, 20);
    SHA1_ROUND5(F1, 0x6ED9EBA1u, 25);
    SHA1_ROUND5(F1, 0x6ED9EBA1u, 30);
    SHA1_ROUND5(F1, 0x6ED9EBA1u, 35);

    // Rounds 40-59
    SHA1_ROUND5(F2, 0x8F1BBCDCu, 40);
    SHA1_ROUND5(F2, 0x8F1BBCDCu, 45);
    SHA1_ROUND5(F2, 0x8F1BBCDCu, 50);
    SHA1_ROUND5(F2, 0x8F1BBCDCu, 55);

    // Rounds 60-79
    SHA1_ROUND5(F3, 0xCA62C1D6u, 60);
    SHA1_ROUND5(F3, 0xCA62C1D6u, 65);
    SHA1_ROUND5(F3, 0xCA62C1D6u, 70);
    SHA1_ROUND5(F3, 0xCA62C1D6u, 75);

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

#undef SHA1_ROUND5
#undef SHA1_STEP

}  // namespace

std::array<uint8_t, 20> Sha1(std::string_view data) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t rem = data.size();

    while (rem >= 64) {
        ProcessBlock(h, ptr);
        ptr += 64;
        rem -= 64;
    }

    uint8_t pad[64] = {0};
    if (rem > 0) {
        std::memcpy(pad, ptr, rem);
    }
    pad[rem] = 0x80;

    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8u;

    if (rem < 56) {
        for (int i = 0; i < 8; ++i) {
            pad[63 - i] = static_cast<uint8_t>(bitLen >> (i * 8));
        }
        ProcessBlock(h, pad);
    } else {
        ProcessBlock(h, pad);
        uint8_t pad2[64] = {0};
        for (int i = 0; i < 8; ++i) {
            pad2[63 - i] = static_cast<uint8_t>(bitLen >> (i * 8));
        }
        ProcessBlock(h, pad2);
    }

    std::array<uint8_t, 20> out{};
    for (size_t i = 0; i < 5; ++i) {
        out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
    return out;
}

}  // namespace scuba::hostd
