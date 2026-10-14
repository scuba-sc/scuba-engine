#include "hostd/sha1.h"

#include <vector>

namespace scuba::hostd {

namespace {

uint32_t Rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

}  // namespace

std::array<uint8_t, 20> Sha1(std::string_view data) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    std::vector<uint8_t> msg(data.begin(), data.end());
    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8u;
    msg.push_back(0x80u);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFu));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = off + i * 4;
            w[i] = (static_cast<uint32_t>(msg[p]) << 24) | (static_cast<uint32_t>(msg[p + 1]) << 16) |
                   (static_cast<uint32_t>(msg[p + 2]) << 8) | static_cast<uint32_t>(msg[p + 3]);
        }
        for (size_t i = 16; i < 80; ++i) w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (size_t i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            const uint32_t t = Rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
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
