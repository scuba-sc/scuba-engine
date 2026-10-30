// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/net_sandbox.h"
#include "hostd/net_sim/broker.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "imgui.h"

namespace scuba::ui {
namespace {
using hostd::Json;
std::string Text(const Json& j, const char* key) {
    const auto* v = j.find(key);
    return v && v->is_string() ? v->as_string().substr(0, 1023) : "";
}
double Number(const Json& j, const char* key) {
    const auto* v = j.find(key);
    return v ? v->as_number() : 0;
}
bool Flag(const Json& j, const char* key) {
    const auto* v = j.find(key);
    return v && v->as_bool();
}
std::vector<NetSandbox::Peer> Peers(const Json& snapshot) {
    std::vector<NetSandbox::Peer> result;
    const auto* peers = snapshot.find("peers");
    if (!peers || !peers->is_array()) throw std::runtime_error("Invalid broker snapshot");
    for (size_t i = 0; i < std::min(peers->size(), size_t{8}); ++i) {
        const auto& j = peers->at(i);
        NetSandbox::Peer p;
        p.slot = static_cast<uint32_t>(Number(j, "slot"));
        p.pid = Text(j, "pid"); p.status = Text(j, "status"); p.error = Text(j, "error");
        p.active = Flag(j, "active");
        p.members = static_cast<uint32_t>(Number(j, "members"));
        p.port = static_cast<uint32_t>(Number(j, "port"));
        if (const auto* t = j.find("traffic")) {
            p.hasTraffic = true;
            p.datagramsIn = Number(*t, "datagramsIn"); p.datagramsOut = Number(*t, "datagramsOut");
            p.retransmits = Number(*t, "retransmits"); p.sendDropped = Number(*t, "sendDropped");
            p.inboxDropped = Number(*t, "inboxDropped"); p.quotaRefused = Number(*t, "quotaRefused");
        }
        if (const auto* t = j.find("probe")) {
            p.hasProbe = true;
            p.reliableSent = Number(*t, "reliableSent"); p.reliableReceived = Number(*t, "reliableReceived");
            p.unreliableSent = Number(*t, "unreliableSent"); p.unreliableReceived = Number(*t, "unreliableReceived");
        }
        result.push_back(std::move(p));
    }
    return result;
}
} // namespace
struct NetSandbox::Impl {
    enum class Action { Refresh, Start, Stop, Join, Leave, Send };
    mutable std::mutex mutex;
    std::condition_variable wake;
    State state;
    bool exit = false;
    Action action = Action::Refresh;
    uint32_t slot = 0, count = 0;
    std::string root;
    bool sample = true;
    std::thread worker;
    int uiCount = 2, uiBurst = 16;
    bool uiSample = true;
    std::array<char, 1024> uiRoot{};
    std::chrono::steady_clock::time_point nextPoll{};

    explicit Impl(std::filesystem::path executable) : worker([this, executable = std::move(executable)] {
        hostd::net_sim::Broker broker(executable);
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return exit || state.busy; });
            if (exit) break;
            const auto a = action;
            const auto s = slot, n = count;
            auto r = root;
            const auto useSample = sample;
            lock.unlock();
            std::vector<Peer> peers;
            std::string error;
            try {
                Json result;
                switch (a) {
                case Action::Start: result = broker.Start(n, std::move(r), useSample); break;
                case Action::Stop: broker.Stop(); result = broker.Snapshot(); break;
                case Action::Join: result = broker.Join(s); break;
                case Action::Leave: result = broker.Leave(s); break;
                case Action::Send: {
                    auto request = Json::MakeObject();
                    request.set("method", "send").set("count", n);
                    broker.Peer(s, std::move(request));
                    result = broker.Snapshot(); break;
                }
                case Action::Refresh: result = broker.Snapshot(); break;
                }
                peers = Peers(result);
            } catch (const std::exception& e) { error = std::string(e.what()).substr(0, 1023); }
            lock.lock();
            state.stale = !error.empty();
            if (error.empty()) state.peers = std::move(peers);
            state.error = std::move(error);
            state.busy = false;
            wake.notify_all();
        }
        // Broker destruction and process cleanup stay on the same worker.
    }) {}
    ~Impl() {
        { std::lock_guard lock(mutex); exit = true; }
        wake.notify_all();
        worker.join();
    }
    bool Submit(Action a, uint32_t s = 0, uint32_t n = 0, std::string r = "", bool useSample = true) {
        std::lock_guard lock(mutex);
        if (state.busy || exit) return false;
        const auto found = std::find_if(state.peers.begin(), state.peers.end(),
                                       [s](const Peer& p) { return p.slot == s; });
        if (a == Action::Start && !state.peers.empty()) return false;
        if (a == Action::Join && (state.peers.empty() || found != state.peers.end())) return false;
        if ((a == Action::Leave || a == Action::Send) && found == state.peers.end()) return false;
        if (a == Action::Send && (!found->active || !found->hasProbe || found->status != "running")) return false;
        action = a; slot = s; count = n; root = std::move(r); sample = useSample;
        state.busy = true;
        wake.notify_one();
        return true;
    }
};
NetSandbox::NetSandbox(std::filesystem::path executable) : impl_(std::make_unique<Impl>(std::move(executable))) {}
NetSandbox::~NetSandbox() = default;
NetSandbox::State NetSandbox::Read() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
bool NetSandbox::WaitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->wake.wait_for(lock, timeout, [this] { return !impl_->state.busy; });
}
bool NetSandbox::Start(uint32_t count, std::string root, bool sample) {
    if (count < 2 || count > 8 || root.size() >= 1024 || root.find('\0') != std::string::npos) return false;
    return impl_->Submit(Impl::Action::Start, 0, count, std::move(root), sample);
}
bool NetSandbox::Stop() { return impl_->Submit(Impl::Action::Stop); }
bool NetSandbox::Refresh() { return impl_->Submit(Impl::Action::Refresh); }
bool NetSandbox::Join(uint32_t slot) { return slot >= 2 && slot <= 8 && impl_->Submit(Impl::Action::Join, slot); }
bool NetSandbox::Leave(uint32_t slot) { return slot >= 2 && slot <= 8 && impl_->Submit(Impl::Action::Leave, slot); }
bool NetSandbox::Send(uint32_t slot, uint32_t count) {
    return slot >= 1 && slot <= 8 && count >= 1 && count <= 100 && impl_->Submit(Impl::Action::Send, slot, count);
}
void NetSandbox::Draw(bool* open) {
    if (open && !*open) return;
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("NetSandbox", open)) { ImGui::End(); return; }
    const auto state = Read();
    const bool running = !state.peers.empty();
    ImGui::TextWrapped("Separate kernels / real loopback UDP. Hostd executable required.");
    ImGui::BeginDisabled(state.busy || running);
    ImGui::SliderInt("Peers", &impl_->uiCount, 2, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::InputText("Plugin folder", impl_->uiRoot.data(), impl_->uiRoot.size());
    ImGui::Checkbox("Synthetic chat plugin", &impl_->uiSample);
    if (ImGui::Button("Start sandbox"))
        Start(static_cast<uint32_t>(impl_->uiCount), impl_->uiRoot.data(), impl_->uiSample);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(state.busy);
    if (ImGui::Button("Stop all")) Stop();
    if (ImGui::Button("Refresh status")) Refresh();
    ImGui::EndDisabled();
    ImGui::Text("%zu isolated hosts%s", state.peers.size(), state.busy ? " / working..." : "");
    if (state.stale) ImGui::TextWrapped("Status stale: last operation failed. Refresh or stop all to recover.");
    if (!state.error.empty()) ImGui::TextWrapped("Error: %s", state.error.c_str());
    ImGui::SliderInt("Burst per channel", &impl_->uiBurst, 1, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (running) {
        ImGui::BeginDisabled(state.busy);
        for (uint32_t slot = 2; slot <= 8; ++slot) {
            if (std::any_of(state.peers.begin(), state.peers.end(), [slot](const Peer& p) { return p.slot == slot; })) continue;
            ImGui::PushID(static_cast<int>(slot));
            if (ImGui::Button(("Join peer " + std::to_string(slot)).c_str())) Join(slot);
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }
    ImGui::Separator();
    ImGui::TextWrapped("Peer widgets are not rendered here. Hiding keeps peers alive; destruction stops them.");
    ImGui::BeginChild("Peer list", ImVec2(0, 0));
    if (!running) ImGui::TextWrapped("No peers running. Start 2-8 isolated hosts; slot 1 is the session host.");
    for (const auto& p : state.peers) {
        ImGui::PushID(static_cast<int>(p.slot));
        ImGui::Text("Peer %u / %s", p.slot, p.slot == 1 ? "SESSION HOST" : "JOINER");
        ImGui::TextWrapped("%s / PID %s / %u members / port %u", p.status.c_str(), p.pid.c_str(), p.members, p.port);
        if (!p.error.empty()) ImGui::TextWrapped("%s", p.error.c_str());
        ImGui::BeginDisabled(state.busy || !p.active || !p.hasProbe || p.status != "running");
        if (ImGui::Button("Send burst")) Send(p.slot, static_cast<uint32_t>(impl_->uiBurst));
        ImGui::EndDisabled();
        if (p.slot != 1) {
            ImGui::BeginDisabled(state.busy);
            if (ImGui::Button("Leave")) Leave(p.slot);
            ImGui::EndDisabled();
        }
        if (p.hasTraffic) {
            ImGui::TextWrapped("UDP in/out: %.0f / %.0f; retransmits: %.0f", p.datagramsIn, p.datagramsOut, p.retransmits);
            ImGui::TextWrapped("Send/inbox drops: %.0f / %.0f; quota refused: %.0f", p.sendDropped, p.inboxDropped, p.quotaRefused);
        } else ImGui::TextUnformatted("Traffic unavailable");
        if (p.hasProbe) {
            ImGui::TextWrapped("Reliable TX/RX: %.0f / %.0f", p.reliableSent, p.reliableReceived);
            ImGui::TextWrapped("Unreliable TX/RX: %.0f / %.0f", p.unreliableSent, p.unreliableReceived);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
    const auto now = std::chrono::steady_clock::now();
    if (!state.busy && now >= impl_->nextPoll) {
        if (Refresh()) impl_->nextPoll = now + std::chrono::milliseconds(400);
    }
}
} // namespace scuba::ui
