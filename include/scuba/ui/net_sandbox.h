// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace scuba::ui {
// Owns a private broker, not the editor kernel. All process I/O belongs to its worker.
class NetSandbox {
public:
    struct Peer {
        uint32_t slot = 0;
        std::string pid, status, error;
        bool active = false;
        uint32_t members = 0, port = 0;
        bool hasTraffic = false, hasProbe = false;
        double datagramsIn = 0, datagramsOut = 0, retransmits = 0;
        double sendDropped = 0, inboxDropped = 0, quotaRefused = 0;
        double reliableSent = 0, reliableReceived = 0, unreliableSent = 0, unreliableReceived = 0;
    };
    struct State {
        std::vector<Peer> peers; // At most eight, sorted by broker slot.
        bool busy = false, stale = false;
        std::string error;
    };
    explicit NetSandbox(std::filesystem::path hostdExecutable);
    ~NetSandbox();
    NetSandbox(const NetSandbox&) = delete;
    NetSandbox& operator=(const NetSandbox&) = delete;
    // True means accepted, not completed. One in-flight command, no pending queue.
    bool Start(uint32_t count, std::string pluginRoot, bool sample);
    bool Stop();
    bool Refresh();
    bool Join(uint32_t slot);
    bool Leave(uint32_t slot);
    bool Send(uint32_t slot, uint32_t count);
    State Read() const;
    bool WaitIdle(std::chrono::milliseconds timeout);
    void Draw(bool* open = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace scuba::ui
