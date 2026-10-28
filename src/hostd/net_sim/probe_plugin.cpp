#include "hostd/net_sim/probe_api.h"
#include "sco_net.h"
#include "sco_widgets.h"
#include <array>
#include <chrono>
#include <string>
#include <thread>

namespace {
const sco_net_v1* netService = nullptr;
const sco_widgets_v1* widgetService = nullptr;
sco_plugin* pluginSelf = nullptr;
NetSimProbeStats counters{};
std::string draft, latest = "No messages received";

// A joiner's session can still be propagating when the broker starts its send, and the per-plugin
// quota drains under load. The kit answers SCO_UNAVAILABLE / SCO_TOO_MANY for those transient
// states, and a refused send queues nothing, so retrying the same message cannot duplicate it.
// Only these two refusals retry; anything else (and a 10 s exhaustion) returns to the caller,
// which is the same contract the Exchange lane of the netsim tests expects.
sco_result SendChannelRetry(const char* channel, const void* data, uint32_t size) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const sco_result result = netService->send_channel(pluginSelf, channel, data, size);
        if (result == SCO_OK) return result;
        if (result != SCO_UNAVAILABLE && result != SCO_TOO_MANY) return result;
        if (std::chrono::steady_clock::now() >= deadline) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}


void Publish();
void ReceiveReliable(uint64_t sender, const void* data, uint32_t size, void*) {
    ++counters.reliableReceived;
    latest = "Peer " + std::to_string(sender) + ": " + std::string(static_cast<const char*>(data), size);
    if (latest.size() > 240) latest.resize(240);
    Publish();
}
void ReceiveUnreliable(uint64_t, const void*, uint32_t, void*) { ++counters.unreliableReceived; }

sco_result Send(uint32_t count) {
    if (count == 0 || count > 100) return SCO_BAD_ARG;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string payload = "synthetic-" + std::to_string(i);
        sco_result result = SendChannelRetry("netsim_probe.reliable",
            payload.data(), static_cast<uint32_t>(payload.size()));
        if (result != SCO_OK) return result;
        ++counters.reliableSent;
        result = SendChannelRetry("netsim_probe.unreliable",
            payload.data(), static_cast<uint32_t>(payload.size()));
        if (result != SCO_OK) return result;
        ++counters.unreliableSent;
    }
    return SCO_OK;
}
NetSimProbeStats Stats() { return counters; }
const NetSimProbe probe{sizeof(NetSimProbe), Send, Stats};

void WidgetEvent(const char*, const sco_widget_event* event, void*) {
    if (event->node_id == 2 && event->text) draft = event->text;
    if (event->node_id == 3 && !draft.empty()) {
        const auto result = netService->send_channel(pluginSelf, "netsim_probe.reliable",
            draft.data(), static_cast<uint32_t>(draft.size()));
        if (result == SCO_OK) {
            ++counters.reliableSent;
            latest = "You: " + draft;
            draft.clear();
        } else {
            latest = "Send refused: " + std::to_string(static_cast<int>(result));
        }
    }
    Publish();
}
void Publish() {
    std::array<sco_widget_node, 3> nodes{};
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].size = sizeof(sco_widget_node);
        nodes[i].id = static_cast<uint32_t>(i + 1);
    }
    nodes[0].kind = SCO_WIDGET_LABEL;
    nodes[0].label = latest.c_str();
    nodes[1].kind = SCO_WIDGET_TEXT_INPUT;
    nodes[1].label = "Message";
    nodes[1].text = draft.c_str();
    nodes[2].kind = SCO_WIDGET_BUTTON;
    nodes[2].label = "Send to peers";
    widgetService->set_panel(pluginSelf, "netsim_probe.chat", "Synthetic chat",
        nodes.data(), static_cast<uint32_t>(nodes.size()), sizeof(sco_widget_node), WidgetEvent, nullptr);
}
} // namespace

const sco_plugin_info* NetSimProbeQuery() {
    static const sco_plugin_info info{sizeof(sco_plugin_info), 1, 1, "netsim_probe", "1.0.0", nullptr};
    return &info;
}
sco_result NetSimProbeLoad(const sco_api* api, sco_plugin* self) {
    const void* table = nullptr;
    if (api->query_service(SCO_NET_NAME, SCO_NET_VERSION_1_0, &table) != SCO_OK) return SCO_FAILED;
    netService = static_cast<const sco_net_v1*>(table);
    if (api->query_service(SCO_WIDGETS_NAME, SCO_WIDGETS_VERSION_1_0, &table) != SCO_OK) return SCO_FAILED;
    widgetService = static_cast<const sco_widgets_v1*>(table);
    pluginSelf = self;
    counters = {};
    if (netService->register_channel(self, "netsim_probe.reliable", SCO_NET_RELIABLE, 256,
                                    ReceiveReliable, nullptr) != SCO_OK ||
        netService->register_channel(self, "netsim_probe.unreliable", 0, 256,
                                    ReceiveUnreliable, nullptr) != SCO_OK) return SCO_FAILED;
    Publish();
    return api->provide_service(self, "netsim_probe.metrics", 0x00010000u, &probe);
}
void NetSimProbeUnload() {}
