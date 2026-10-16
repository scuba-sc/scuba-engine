// SPDX-License-Identifier: GPL-3.0-only
// scuba-pack: build, list and verify .scpak containers.
//   scuba-pack build  <out.scpak> <directory>
//   scuba-pack list   <archive.scpak|.zip>
//   scuba-pack verify <archive.scpak>
#include "scuba/vfs/file_system.h"
#include "scuba/vfs/pack.h"

#include <cstdio>
#include <string>

namespace {

int Usage() {
    std::fprintf(stderr, "usage: scuba-pack build <out.scpak> <dir> | list <archive> | verify <archive.scpak>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace scuba;
    if (argc < 3) return Usage();
    const std::string verb = argv[1];
    if (verb == "build" && argc == 4) {
        const auto st = vfs::WritePackFromDirectory(argv[2], argv[3]);
        if (!st) { std::fprintf(stderr, "scuba-pack: %s\n", st.error.c_str()); return 1; }
        std::printf("wrote %s\n", argv[2]);
        return 0;
    }
    if (verb == "verify" && argc == 3) {
        const auto st = vfs::VerifyPack(argv[2]);
        if (!st) { std::fprintf(stderr, "scuba-pack: %s\n", st.error.c_str()); return 1; }
        std::printf("%s: every entry matches its CRC-32\n", argv[2]);
        return 0;
    }
    if (verb == "list" && argc == 3) {
        const std::string path = argv[2];
        Status st;
        auto fs = path.size() > 4 && path.compare(path.size() - 4, 4, ".zip") == 0 ? vfs::OpenZip(path, st) : vfs::OpenPack(path, st);
        if (!fs) { std::fprintf(stderr, "scuba-pack: %s\n", st.error.c_str()); return 1; }
        std::vector<vfs::Entry> entries;
        fs->List("", entries);
        for (const auto& e : entries) std::printf("%12llu  %s\n", static_cast<unsigned long long>(e.size), e.path.c_str());
        return 0;
    }
    return Usage();
}
