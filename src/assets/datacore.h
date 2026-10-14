#pragma once
#include "assets/io.h"
#include "hostd/json.h"
#include "sco/datacore_pack.h"

namespace scuba::assets {
class DataCore {
public:
    hostd::Json Open(const std::filesystem::path& path);
    hostd::Json Records(size_t offset, size_t limit, std::string_view filter) const;
    hostd::Json Structs() const;
    hostd::Json Fields(std::string_view record) const;
    void SetPack(std::string_view text);
    std::string Export() const;
private:
    Bytes bytes_;
    sco::datacore::Schema schema_;
    sco::datacore::Pack pack_;
    bool loaded_ = false;
};
} // namespace scuba::assets
