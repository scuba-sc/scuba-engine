#pragma once
// SHA-1 (FIPS 180-4), used only for the RFC 6455 Sec-WebSocket-Accept value. Not for security.
#include <array>
#include <cstdint>
#include <string_view>

namespace scuba::hostd {

std::array<uint8_t, 20> Sha1(std::string_view data);

}  // namespace scuba::hostd
