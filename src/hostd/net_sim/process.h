#pragma once
#include "hostd/json.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace scuba::hostd::net_sim {
// One child, one ordered JSON-line request stream. No shell interprets arguments.
class Process {
public:
    Process(const std::filesystem::path& executable, const std::vector<std::string>& args);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Json Call(Json request);
    uint64_t id() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace scuba::hostd::net_sim
