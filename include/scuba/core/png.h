// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "scuba/core/status.h"
#include <cstdint>
#include <filesystem>
#include <span>
namespace scuba {
Status WriteNewRgbaPng(const std::filesystem::path& path, uint32_t width, uint32_t height,
                       std::span<const uint8_t> pixels);
}
