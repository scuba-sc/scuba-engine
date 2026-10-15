#include "hostd/net_sim/peer.h"
#include "hostd/net_sim/probe_api.h"
#include "hostd/json.h"
#include "sco/app.h"
#include "sco/log.h"
#include "sco/ipc.h"
#include "sco/net/session.h"
#include "sco/widgets.h"
#include "sco_lua.h"
#include "sco_lua_game.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <charconv>
#include <unistd.h>
#endif

namespace scuba::hostd::net_sim {
namespace {
std::string ipcPrefix;
std::set<uint64_t> remotePeers;
std::atomic<bool> sessionActive{false};
int eventOwner = 0;
void SessionEvent(const char* event, const void* data, void*) {
    if (std::string_view(event) == SCO_NET_EVENT_STATE) {
        sessionActive = static_cast<const sco_net_state_event*>(data)->active != 0;
        if (!sessionActive) remotePeers.clear();
    } else {
        const auto& peer = *static_cast<const sco_net_peer_event*>(data);
        if (peer.what == SCO_NET_PEER_JOINED) remotePeers.insert(peer.peer.peer_id);
        if (peer.what == SCO_NET_PEER_LEFT) remotePeers.erase(peer.peer.peer_id);
    }
}
const sco_plugin_info* IsolationQuery() {
    static const sco_plugin_info info{sizeof(sco_plugin_info), 1, 1, "netsim_isolation", "1.0.0", nullptr};
    return &info;
}
sco_result IsolationLoad(const sco_api*, sco_plugin*) {
    // First built-in: no user plugin has queried sco.ipc yet.
    sco::ipc::Stop();
    sco::ipc::Options options;
    options.prefix = sco::ipc::kPrefix + ipcPrefix + "_";
    return sco::ipc::Start(options) == sco::Result::Ok ? SCO_OK : SCO_FAILED;
}
void IsolationUnload() {}
const Json& Field(const Json& value, const char* name) {
    const auto* field = value.find(name);
    if (!field) throw std::runtime_error(std::string("missing ") + name);
    return *field;
}
void Tick() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    sco::app::Tick(static_cast<uint32_t>(static_cast<uint64_t>(ms)));
}
const NetSimProbe* Probe() {
    const void* table = nullptr;
    if (sco::QueryService("netsim_probe.metrics", 0x00010000u, &table) != sco::Result::Ok)
        throw std::runtime_error("synthetic probe is not loaded");
    return static_cast<const NetSimProbe*>(table);
}
Json Snapshot() {
    Tick();
    Json result = Json::MakeObject();
    const auto* table = sco::net::Table();
    result.set("active", sessionActive.load());
    result.set("self", std::to_string(table->self_peer()));
    result.set("port", sco::net::BoundPort());
    result.set("reason", sco::net::LastReason());
    result.set("ipcPrefix", sco::ipc::kPrefix + ipcPrefix + "_");
    result.set("members", sessionActive ? remotePeers.size() + 1 : 0);
    const auto stats = sco::net::GetServiceStats();
    Json traffic = Json::MakeObject();
    traffic.set("datagramsIn", stats.core.datagramsIn).set("datagramsOut", stats.core.datagramsOut)
        .set("retransmits", stats.core.retransmits).set("sendDropped", stats.sendDropped)
        .set("inboxDropped", stats.inboxDropped).set("quotaRefused", stats.quotaRefused);
    result.set("traffic", std::move(traffic));
    Json plugins = Json::MakeArray();
    for (const auto& plugin : sco::app::Plugins()) {
        Json item = Json::MakeObject();
        item.set("id", plugin.folder).set("state", sco::plugins::StateName(plugin.state)).set("reason", plugin.reason);
        plugins.push_back(std::move(item));
    }
    result.set("plugins", std::move(plugins));
    const void* probeTable = nullptr;
    if (sco::QueryService("netsim_probe.metrics", 0x00010000u, &probeTable) == sco::Result::Ok) {
        const auto counters = static_cast<const NetSimProbe*>(probeTable)->stats();
        Json probe = Json::MakeObject();
        probe.set("reliableSent", counters.reliableSent).set("unreliableSent", counters.unreliableSent)
            .set("reliableReceived", counters.reliableReceived).set("unreliableReceived", counters.unreliableReceived);
        result.set("probe", std::move(probe));
    }
    Json panels = Json::MakeArray();
    for (const auto& panel : sco::widgets::Panels()) {
        Json item = Json::MakeObject();
        item.set("id", panel.id).set("title", panel.title).set("owner", panel.owner);
        Json nodes = Json::MakeArray();
        for (const auto& node : panel.nodes) {
            Json value = Json::MakeObject();
            value.set("id", node.id).set("kind", node.kind).set("parent", node.parent)
                .set("flags", node.flags).set("label", node.label).set("text", node.text)
                .set("value", node.value).set("minimum", node.minimum).set("maximum", node.maximum)
                .set("index", node.index).set("columns", node.columns).set("tooltip", node.tooltip)
                .set("severity", node.severity).set("firstRow", node.first_row).set("visibleRows", node.visible_rows);
            Json items = Json::MakeArray();
            for (const auto& text : node.items) items.push_back(text);
            value.set("items", std::move(items));
            nodes.push_back(std::move(value));
        }
        item.set("nodes", std::move(nodes));
        panels.push_back(std::move(item));
    }
    result.set("panels", std::move(panels));
    return result;
}
class InputRenderer final : public sco::widgets::Renderer {
public:
    sco::widgets::Event input;
    void Render(const sco::widgets::Panel&, std::vector<sco::widgets::Event>& events) override { events.push_back(input); }
};

Json Command(const Json& request) {
    const auto method = Field(request, "method").as_string();
    if (method == "start") {
        static const sco::plugins::Builtin builtins[]{
            {"netsim_isolation", IsolationQuery, IsolationLoad, IsolationUnload},
            {"netsim_probe", NetSimProbeQuery, NetSimProbeLoad, NetSimProbeUnload}
        };
        static const sco::plugins::ScriptRuntime lua{sco_lua_load, sco_lua_unload};
        sco::app::Platform platform;
        platform.hostVersion = "scuba-net-peer";
        platform.dataRoot = Field(request, "dataRoot").as_string();
        platform.pluginRoot = Field(request, "pluginRoot").as_string();
        platform.pluginsEnabled = !platform.pluginRoot.empty();
        platform.scripts = &lua;
        ipcPrefix = Field(request, "ipcPrefix").as_string();
        platform.builtins = builtins;
        platform.nBuiltins = Field(request, "sample").as_bool() ? 2u : 1u;
        if (!sco::app::Start(platform)) throw std::runtime_error("kernel start failed");
        if (sco::Subscribe(&eventOwner, SCO_NET_EVENT_STATE, SessionEvent, nullptr) != sco::Result::Ok ||
            sco::Subscribe(&eventOwner, SCO_NET_EVENT_PEER, SessionEvent, nullptr) != sco::Result::Ok)
            throw std::runtime_error("session event subscription failed");
        const auto port = static_cast<uint16_t>(Field(request, "port").as_number());
        sco::Result rc;
        if (port == 0) {
            sco::net::HostOptions options;
            options.port = 0;
            options.passphrase = Field(request, "passphrase").as_string();
            options.playerName = Field(request, "name").as_string();
            rc = sco::net::Host(options);
        } else {
            sco::net::JoinOptions options;
            options.address = "127.0.0.1";
            options.port = port;
            options.passphrase = Field(request, "passphrase").as_string();
            options.playerName = Field(request, "name").as_string();
            rc = sco::net::Join(options);
        }
        if (rc != sco::Result::Ok) throw std::runtime_error("session start failed");
    } else if (method == "wait") {
        const auto* members = request.find("members");
        const auto* reliable = request.find("reliable");
        const auto* unreliable = request.find("unreliable");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            const Json snapshot = Snapshot();
            const bool memberReady = !members || Field(snapshot, "members").as_number() == members->as_number();
            const auto counters = (reliable || unreliable) ? Probe()->stats() : NetSimProbeStats{};
            if (memberReady && (!reliable || counters.reliableReceived >= reliable->as_number()) &&
                (!unreliable || counters.unreliableReceived >= unreliable->as_number())) return snapshot;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("peer condition timeout: " + snapshot.dump());
            // Delivery stays signalled until Tick drains it, so checking the state first cannot miss a wake.
            sco::net::WaitForDelivery(static_cast<uint32_t>(remaining));
        }
    } else if (method == "send") {
        const auto count = Field(request, "count").as_number();
        if (count < 1 || count > 100 || count != static_cast<uint32_t>(count)) throw std::runtime_error("count must be 1..100");
        const sco_result sent = Probe()->send(static_cast<uint32_t>(count));
        if (sent != SCO_OK) throw std::runtime_error("probe send refused: " + std::to_string(static_cast<int>(sent)));
    } else if (method == "input") {
        InputRenderer renderer;
        renderer.input.node_id = static_cast<uint32_t>(Field(request, "node").as_number());
        renderer.input.kind = static_cast<uint32_t>(Field(request, "kind").as_number());
        if (const auto* value = request.find("value")) renderer.input.value = value->as_number();
        if (const auto* value = request.find("index")) renderer.input.index = static_cast<int32_t>(value->as_number());
        if (const auto* value = request.find("row")) renderer.input.row = static_cast<int32_t>(value->as_number());
        if (const auto* text = request.find("text")) renderer.input.text = text->as_string();
        if (sco::widgets::RenderPanel(Field(request, "panel").as_string().c_str(), renderer) != sco::Result::Ok)
            throw std::runtime_error("panel input refused");
    } else if (method != "snapshot") {
        throw std::runtime_error("unknown peer command");
    }
    return Snapshot();
}
} // namespace

int PeerMain() {
#ifdef __linux__
    // The parent may own plugin descriptors without CLOEXEC. Drop them before starting threads.
    std::vector<int> inherited;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const auto name = entry.path().filename().string();
        int descriptor = -1;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), descriptor);
        if (parsed.ec == std::errc{} && descriptor > STDERR_FILENO) inherited.push_back(descriptor);
    }
    for (const int descriptor : inherited) close(descriptor);
#endif
    sco::SetLogSink([](const char* line) { std::cerr << line << '\n'; });
    sco_lua_add_extension(&sco_lua_game_extension);
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> requests;
    bool ended = false;
    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard lock(mutex);
            requests.push_back(std::move(line));
            changed.notify_one();
        }
        std::lock_guard lock(mutex);
        ended = true;
        changed.notify_one();
    });
    for (;;) {
        std::unique_lock lock(mutex);
        if (requests.empty() && !ended) {
            if (!sessionActive) {
                changed.wait(lock, [&] { return ended || !requests.empty(); });
            } else {
                lock.unlock();
                sco::net::WaitForDelivery(16);
                Tick();
                continue;
            }
        }
        if (requests.empty()) {
            if (ended) break;
            continue;
        }
        auto line = std::move(requests.front());
        requests.pop_front();
        lock.unlock();
        Json result;
        try {
            auto request = Json::parse(line);
            if (!request) throw std::runtime_error("invalid peer request");
            result = Command(*request);
        } catch (const std::exception& error) {
            result = Json::MakeObject();
            result.set("error", error.what());
        }
        std::cout << result.dump() << '\n' << std::flush;
    }
    reader.join();
    sco::app::Stop();
    return 0;
}
} // namespace scuba::hostd::net_sim
