#pragma once
#include "hostd/handlers.h"
#include "hostd/net_sim/process.h"
#include <map>

namespace scuba::hostd::net_sim {
class Broker {
public:
    explicit Broker(std::filesystem::path executable);
    ~Broker();
    Json Start(uint32_t count, std::string pluginRoot, bool sample);
    Json Snapshot();
    Json Leave(uint32_t slot);
    Json Join(uint32_t slot);
    Json Peer(uint32_t slot, Json request);
    void Stop();
private:
    std::filesystem::path executable_, dataRoot_;
    std::string pluginRoot_, passphrase_, namespace_;
    bool sample_ = true;
    uint16_t port_ = 0;
    std::map<uint32_t, std::unique_ptr<Process>> peers_;
    void Spawn(uint32_t slot);
};
void RegisterNetSim(Registry& registry, Broadcaster& broadcaster, const std::filesystem::path& executable);
} // namespace scuba::hostd::net_sim
