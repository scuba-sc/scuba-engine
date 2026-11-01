#include "hostd/net_sim/broker.h"
#include <cmath>
#include <charconv>
#include <random>
#include <stdexcept>

namespace scuba::hostd::net_sim {
namespace {
uint32_t Integer(const Json& params, const char* key, uint32_t minimum, uint32_t maximum) {
    const auto* field = params.find(key);
    if (!field || !field->is_number()) throw HandlerError(rpc_error::kInvalidParams, std::string(key) + " must be an integer");
    const double number = field->as_number();
    if (number < minimum || number > maximum || std::floor(number) != number)
        throw HandlerError(rpc_error::kInvalidParams, std::string(key) + " out of range");
    return static_cast<uint32_t>(number);
}
Json Request(const char* method) {
    Json request = Json::MakeObject();
    request.set("method", method);
    return request;
}
} // namespace

Broker::Broker(std::filesystem::path executable) : executable_(std::filesystem::absolute(std::move(executable))) {}
Broker::~Broker() { (void)Stop(); }

void Broker::Spawn(uint32_t slot) {
    auto peer = std::make_unique<Process>(executable_, std::vector<std::string>{"--net-peer"});
    Json request = Request("start");
    request.set("port", slot == 1 ? 0 : port_).set("name", "Peer " + std::to_string(slot))
        .set("passphrase", passphrase_).set("pluginRoot", pluginRoot_).set("sample", sample_)
        .set("dataRoot", (dataRoot_ / std::to_string(slot)).string())
        .set("ipcPrefix", namespace_ + std::to_string(slot));
    const Json snapshot = peer->Call(std::move(request));
    if (slot == 1) port_ = static_cast<uint16_t>(snapshot.find("port")->as_number());
    peers_.emplace(slot, std::move(peer));
}
Json Broker::Start(uint32_t count, std::string pluginRoot, bool sample) {
    if (count < 2 || count > 8) throw std::invalid_argument("peers must be 2..8");
    if (stopFailure_) throw std::runtime_error("sandbox shutdown was unclean; new sessions are disabled");
    if (!peers_.empty() || !dataRoot_.empty()) throw std::runtime_error("stop the current sandbox first");
    std::random_device random;
    char hex[16]{};
    const auto encoded = std::to_chars(hex, hex + sizeof hex, random(), 16);
    namespace_ = "n" + std::string(hex, encoded.ptr);
    passphrase_ = std::to_string(random()) + std::to_string(random()) + std::to_string(random());
    const auto dataRoot = std::filesystem::temp_directory_path() / ("scuba-net-" + passphrase_);
    if (!std::filesystem::create_directory(dataRoot)) throw std::runtime_error("sandbox data root already exists");
    dataRoot_ = dataRoot;
    pluginRoot_ = std::move(pluginRoot);
    sample_ = sample;
    try {
        for (uint32_t slot = 1; slot <= count; ++slot) Spawn(slot);
        for (auto& [slot, peer] : peers_) {
            (void)slot;
            Json request = Request("wait");
            request.set("members", count);
            peer->Call(std::move(request));
        }
        stopFailure_ = false;
        return Snapshot();
    } catch (...) {
        Stop();
        throw;
    }
}
Json Broker::Snapshot() {
    Json result = Json::MakeObject();
    Json peers = Json::MakeArray();
    for (auto& [slot, peer] : peers_) {
        Json snapshot;
        try {
            snapshot = peer->Call(Request("snapshot"));
            snapshot.set("status", "running");
        } catch (const std::exception& error) {
            snapshot = Json::MakeObject();
            snapshot.set("status", "failed").set("error", error.what());
        }
        snapshot.set("slot", slot).set("pid", std::to_string(peer->id()));
        peers.push_back(std::move(snapshot));
    }
    result.set("peers", std::move(peers));
    return result;
}
bool Broker::Stop() {
    // Joiners send BYE while the host is still alive.
    bool clean = !stopFailure_;
    while (!peers_.empty()) {
        auto peer = std::prev(peers_.end());
        if (!peer->second->Stop()) clean = false;
        peers_.erase(peer);
    }
    port_ = 0;
    if (!dataRoot_.empty()) {
        std::error_code error;
        std::filesystem::remove_all(dataRoot_, error);
        if (error) clean = false;
        else dataRoot_.clear();
    }
    stopFailure_ = !clean;
    return clean;
}
Json Broker::Leave(uint32_t slot) {
    if (stopFailure_) throw std::runtime_error("sandbox shutdown was unclean; peer operations are disabled");
    if (slot == 1) throw std::runtime_error("the session host cannot leave; stop the sandbox");
    const auto found = peers_.find(slot);
    if (found == peers_.end()) throw std::runtime_error("peer slot is not running");
    const bool clean = found->second->Stop();
    peers_.erase(found);
    if (!clean) {
        stopFailure_ = true;
        throw std::runtime_error("peer did not exit cleanly; session is terminal");
    }
    const auto count = peers_.size();
    for (auto& [key, peer] : peers_) {
        (void)key;
        Json request = Request("wait");
        request.set("members", count);
        peer->Call(std::move(request));
    }
    return Snapshot();
}
Json Broker::Join(uint32_t slot) {
    if (stopFailure_) throw std::runtime_error("sandbox shutdown was unclean; peer operations are disabled");
    if (slot < 2 || slot > 8 || peers_.contains(slot) || !peers_.contains(1)) throw std::runtime_error("invalid join slot");
    Spawn(slot);
    try {
        for (auto& [key, peer] : peers_) {
            (void)key;
            Json request = Request("wait");
            request.set("members", peers_.size());
            peer->Call(std::move(request));
        }
    } catch (...) {
        const auto failed = peers_.find(slot);
        if (failed != peers_.end()) {
            if (!failed->second->Stop()) stopFailure_ = true;
            peers_.erase(failed);
        }
        throw;
    }
    return Snapshot();
}
Json Broker::Peer(uint32_t slot, Json request) {
    if (stopFailure_) throw std::runtime_error("sandbox shutdown was unclean; peer operations are disabled");
    const auto found = peers_.find(slot);
    if (found == peers_.end()) throw std::runtime_error("peer slot is not running");
    return found->second->Call(std::move(request));
}

std::shared_ptr<Broker> RegisterNetSim(Registry& registry, Broadcaster& broadcaster,
                                      const std::filesystem::path& executable) {
    auto broker = std::make_shared<Broker>(executable);
    const auto publish = [broker, &broadcaster] {
        Json result = broker->Snapshot();
        broadcaster.broadcast("netsim.snapshot", result);
        return result;
    };
    registry.register_handler("netsim.start", [broker, &broadcaster](const Json& params, const CallContext&) {
        const uint32_t count = Integer(params, "count", 2, 8);
        const auto* root = params.find("pluginRoot");
        const auto* sample = params.find("sample");
        if ((root && !root->is_string()) || (sample && !sample->is_bool()))
            throw HandlerError(rpc_error::kInvalidParams, "pluginRoot must be a string and sample a boolean");
        auto result = broker->Start(count, root ? root->as_string() : "", !sample || sample->as_bool());
        broadcaster.broadcast("netsim.snapshot", result);
        return result;
    });
    registry.register_handler("netsim.status", [publish](const Json&, const CallContext&) { return publish(); });
    registry.register_handler("netsim.stop", [broker, publish](const Json&, const CallContext&) {
        if (!broker->Stop()) throw std::runtime_error("peer did not exit cleanly; child processes have exited and the session is terminal");
        return publish();
    });
    registry.register_handler("netsim.leave", [broker, publish](const Json& params, const CallContext&) {
        broker->Leave(Integer(params, "slot", 2, 8)); return publish();
    });
    registry.register_handler("netsim.join", [broker, publish](const Json& params, const CallContext&) {
        broker->Join(Integer(params, "slot", 2, 8)); return publish();
    });
    registry.register_handler("netsim.send", [broker, publish](const Json& params, const CallContext&) {
        const auto slot = Integer(params, "slot", 1, 8);
        Json request = Request("send");
        request.set("count", Integer(params, "count", 1, 100));
        broker->Peer(slot, std::move(request)); return publish();
    });
    registry.register_handler("netsim.input", [broker, publish](const Json& params, const CallContext&) {
        const auto slot = Integer(params, "slot", 1, 8);
        const auto* panel = params.find("panel");
        if (!panel || !panel->is_string() || panel->as_string().size() > 63)
            throw HandlerError(rpc_error::kInvalidParams, "panel must be a panel id");
        Integer(params, "node", 1, UINT32_MAX);
        Integer(params, "kind", 1, 0x100u);
        if (const auto* text = params.find("text"); text && (!text->is_string() || text->as_string().size() > 255))
            throw HandlerError(rpc_error::kInvalidParams, "text must be at most 255 bytes");
        for (const char* key : {"index", "row", "value"}) {
            if (const auto* value = params.find(key); value && (!value->is_number() ||
                (std::string_view(key) != "value" && (std::floor(value->as_number()) != value->as_number() ||
                 value->as_number() < -1 || value->as_number() > INT32_MAX))))
                throw HandlerError(rpc_error::kInvalidParams, "invalid widget value");
        }
        Json request = params;
        request.set("method", "input");
        broker->Peer(slot, std::move(request)); return publish();
    });
    return broker;
}
} // namespace scuba::hostd::net_sim
