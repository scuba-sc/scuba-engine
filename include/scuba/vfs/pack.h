// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// .scpak: scuba-engine's packed container. Read-optimized and deliberately simple: one index at a
// known place, entries stored contiguously and aligned, a CRC-32 per entry. Little-endian.
//
//   offset 0   header (32 bytes)
//              char[8]  magic "SCPAK\0\r\n"   (\r\n catches text-mode transfers)
//              u32      version = 1
//              u32      entry count
//              u64      index offset
//              u64      index size
//   ...        entry data, each starting on a 16-byte boundary
//   index      per entry, sorted by path:
//              u16 path length | path bytes (normalized) | u64 offset | u64 size | u32 crc32 |
//              u8 compression (0 = stored; anything else is refused at open)
//
// OpenPack (file_system.h) mounts one; WritePack builds one; VerifyPack checks every CRC.
#include "scuba/vfs/file_system.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace scuba::vfs {

inline constexpr char kPackMagic[8] = {'S', 'C', 'P', 'A', 'K', '\0', '\r', '\n'};
inline constexpr uint32_t kPackVersion = 1;
inline constexpr uint32_t kPackMaxEntries = 1u << 20;

// Paths are normalized; duplicates (after normalization) and unnormalizable paths are refused.
Status WritePack(const std::filesystem::path& out, const std::vector<std::pair<std::string, Bytes>>& files);
// Every file under dir, paths relative to it.
Status WritePackFromDirectory(const std::filesystem::path& out, const std::filesystem::path& dir);
Status VerifyPack(const std::filesystem::path& archive);

uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0);

}  // namespace scuba::vfs
