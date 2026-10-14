#include "check.h"
#include "hostd/net_sim/broker.h"
#include <csignal>
#include <iostream>
#include <set>

using namespace scuba::hostd;
using namespace scuba::hostd::net_sim;

namespace {
Json Request(const char* method) {
    Json value = Json::MakeObject();
    value.set("method", method);
    return value;
}
const Json& Field(const Json& object, const char* name) {
    const auto* value = object.find(name);
    CHECK(value != nullptr);
    return *value;
}
void CheckMembers(const Json& snapshot, size_t count) {
    const auto& peers = Field(snapshot, "peers");
    CHECK(peers.size() == count);
    std::set<std::string> processes, ipcPrefixes;
    for (size_t i = 0; i < count; ++i) {
        const auto& peer = peers.at(i);
        CHECK(Field(peer, "status").as_string() == "running");
        CHECK(Field(peer, "active").as_bool());
        CHECK(Field(peer, "members").as_number() == static_cast<double>(count));
        processes.insert(Field(peer, "pid").as_string());
        ipcPrefixes.insert(Field(peer, "ipcPrefix").as_string());
        CHECK(Field(peer, "panels").size() == 1);
        const auto& plugins = Field(peer, "plugins");
        CHECK(plugins.size() == 2);
        for (size_t p = 0; p < plugins.size(); ++p) CHECK(Field(plugins.at(p), "state").as_string() == "loaded");
    }
    CHECK(processes.size() == count);
    CHECK(ipcPrefixes.size() == count);
}
void Exchange(Broker& broker, uint32_t count) {
    for (uint32_t slot = 1; slot <= count; ++slot) {
        auto send = Request("send");
        send.set("count", 32);
        broker.Peer(slot, send);
    }
    for (uint32_t slot = 1; slot <= count; ++slot) {
        auto wait = Request("wait");
        wait.set("reliable", 32u * (count - 1)).set("unreliable", 32u * (count - 1));
        const auto snapshot = broker.Peer(slot, wait);
        const auto& probe = Field(snapshot, "probe");
        CHECK(Field(probe, "reliableSent").as_number() == 32);
        CHECK(Field(probe, "unreliableSent").as_number() == 32);
        CHECK(Field(probe, "reliableReceived").as_number() == 32u * (count - 1));
        CHECK(Field(probe, "unreliableReceived").as_number() == 32u * (count - 1));
        const auto& traffic = Field(snapshot, "traffic");
        CHECK(Field(traffic, "sendDropped").as_number() == 0);
        CHECK(Field(traffic, "inboxDropped").as_number() == 0);
        std::cout << "slot=" << slot << " reliable_received=" << Field(probe, "reliableReceived").as_number()
                  << " unreliable_received=" << Field(probe, "unreliableReceived").as_number()
                  << " unreliable_loss=0\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 3);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    Broker broker(argv[1]);
    try {
        if (std::string(argv[2]) == "delivery") {
            // Given two real child kernels with a public-API-only synthetic plugin.
            CheckMembers(broker.Start(2, "", true), 2);
            // When both peers send reliable and unreliable traffic.
            // Then every receiver sees exactly the expected message count.
            Exchange(broker, 2);
            auto input = Request("input");
            input.set("panel", "netsim_probe.chat").set("node", 2).set("kind", 7).set("text", "widget event");
            broker.Peer(1, input);
            input = Request("input");
            input.set("panel", "netsim_probe.chat").set("node", 3).set("kind", 2);
            broker.Peer(1, input);
            auto wait = Request("wait");
            wait.set("reliable", 33);
            const auto receiver = broker.Peer(2, wait);
            const auto& label = Field(Field(receiver, "panels").at(0), "nodes").at(0);
            CHECK(Field(label, "label").as_string() == "Peer 1: widget event");
        } else if (std::string(argv[2]) == "membership") {
            // Given four real peers.
            CheckMembers(broker.Start(4, "", true), 4);
            // When a joiner leaves and rejoins.
            CheckMembers(broker.Leave(3), 3);
            const auto replacement = broker.Join(3);
            // Then all kernels agree on membership and traffic reaches the replacement.
            CheckMembers(replacement, 4);
            Exchange(broker, 4);
        } else {
            for (const uint32_t invalid : {1u, 9u}) {
                bool refused = false;
                try { broker.Start(invalid, "", true); } catch (const std::invalid_argument&) { refused = true; }
                CHECK(refused);
            }
            CheckMembers(broker.Start(8, "", true), 8);
            Exchange(broker, 8);
        }
        broker.Stop();
        CHECK(Field(broker.Snapshot(), "peers").size() == 0);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
