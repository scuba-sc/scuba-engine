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
    // Close stdin, wait for process exit, and force termination after the
    // grace period if needed. Returns true only when the child exits normally
    // with status zero before the deadline; either result returns only after
    // the child is confirmed gone.
    bool Stop();
    uint64_t id() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace scuba::hostd::net_sim
