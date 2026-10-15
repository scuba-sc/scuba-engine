#pragma once
// Standard base64 (RFC 4648, with padding).
#include <cstddef>
#include <cstdint>
#include <string>

namespace scuba::hostd {

std::string Base64Encode(const uint8_t* data, size_t size);

}  // namespace scuba::hostd
