// SPDX-License-Identifier: GPL-3.0-only
#include "vfs/os_file.h"

#include <algorithm>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scuba::vfs::detail {

namespace {
std::string Utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
}  // namespace

#ifdef _WIN32

std::shared_ptr<OsFile> OsFile::Open(const std::filesystem::path& path, Status* why) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = Status::Fail("cannot open " + Utf8(path) + " (Win32 error " + std::to_string(GetLastError()) + ")");
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        CloseHandle(h);
        if (why) *why = Status::Fail("cannot size " + Utf8(path));
        return nullptr;
    }
    std::shared_ptr<OsFile> f(new OsFile());
    f->handle_ = h;
    f->size_ = static_cast<uint64_t>(size.QuadPart);
    return f;
}

OsFile::~OsFile() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

size_t OsFile::ReadAt(uint64_t offset, void* dst, size_t n) const {
    size_t done = 0;
    auto* out = static_cast<uint8_t*>(dst);
    while (done < n && offset + done < size_) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - done, 1u << 30));
        OVERLAPPED at{};
        const uint64_t pos = offset + done;
        at.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFu);
        at.OffsetHigh = static_cast<DWORD>(pos >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), out + done, chunk, &got, &at) || got == 0) break;
        done += got;
    }
    return done;
}

#else

std::shared_ptr<OsFile> OsFile::Open(const std::filesystem::path& path, Status* why) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (why) *why = Status::Fail("cannot open " + Utf8(path));
        return nullptr;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        if (why) *why = Status::Fail("cannot size " + Utf8(path));
        return nullptr;
    }
    std::shared_ptr<OsFile> f(new OsFile());
    f->fd_ = fd;
    f->size_ = static_cast<uint64_t>(st.st_size);
    return f;
}

OsFile::~OsFile() {
    if (fd_ >= 0) ::close(fd_);
}

size_t OsFile::ReadAt(uint64_t offset, void* dst, size_t n) const {
    size_t done = 0;
    auto* out = static_cast<uint8_t*>(dst);
    while (done < n && offset + done < size_) {
        const ssize_t got = ::pread(fd_, out + done, n - done, static_cast<off_t>(offset + done));
        if (got <= 0) break;
        done += static_cast<size_t>(got);
    }
    return done;
}

#endif

}  // namespace scuba::vfs::detail
