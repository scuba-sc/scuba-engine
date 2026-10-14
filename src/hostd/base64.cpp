#include "hostd/base64.h"

namespace scuba::hostd {

std::string Base64Encode(const uint8_t* data, size_t size) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63u];
        out += kAlphabet[(v >> 12) & 63u];
        out += kAlphabet[(v >> 6) & 63u];
        out += kAlphabet[v & 63u];
    }
    if (i + 1 == size) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63u];
        out += kAlphabet[(v >> 12) & 63u];
        out += "==";
    } else if (i + 2 == size) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63u];
        out += kAlphabet[(v >> 12) & 63u];
        out += kAlphabet[(v >> 6) & 63u];
        out += '=';
    }
    return out;
}

}  // namespace scuba::hostd
