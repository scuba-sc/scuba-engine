// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/sco/sco_host.h"
#include "scuba/core/log.h"

#include "hostd/kernel.h"
#include "hostd/product_bundle.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/common.h"
#include "sco/widgets.h"
#include "sco/runtime.h"
#include "sco/storage.h"

#include <cstring>
#include <cstdio>

#ifndef SCUBA_SDK_VERSION_STRING
#define SCUBA_SDK_VERSION_STRING "unknown"
#endif

namespace scuba {
namespace {

hostd::Kernel& TheKernel() {
    static hostd::Kernel kernel;   // sco::app is process-wide; so is its one host
    return kernel;
}

void ForwardKernelLog(const char* line) {
    if (!line) return;
    std::string_view text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    const bool bad = text.find("error") != std::string_view::npos || text.find("refused") != std::string_view::npos;
    log::Write(bad ? log::Level::Warn : log::Level::Info, "sco", text);
}

}  // namespace

ScoHost::~ScoHost() { Stop(); }

Status ScoHost::Start(const ScoHostOptions& options) {
    if (started_) return Status::Fail("sco host already started");
    hostd::KernelOptions k;
    k.pluginRoot = options.pluginRoot;
    k.productRoot = options.productRoot;
    k.dataRoot = options.dataRoot;
    k.lua = options.lua;
    k.mockGame = options.mockGame;
    k.hostVersion = "scuba-engine " SCUBA_VERSION;
    start_ = std::chrono::steady_clock::now();
    if (!TheKernel().Start(k, &ForwardKernelLog)) return Status::Fail(TheKernel().profileError().empty()
        ? "sco::app::Start failed (see the sco log channel)" : TheKernel().profileError());
    started_ = true;
    productRoot_ = options.productRoot;
    for (const auto& command : {
             sco::Command{"sco.mock.admit", "Admit mock capabilities", "Enable synthetic capabilities only", nullptr,
                          nullptr, 0, [](const sco::Arg*, uint32_t, void*, char* reply, uint32_t size) {
                              const bool ok = hostd::mock_game::ApplyProfile("all");
                              std::snprintf(reply, size, "%s", ok ? "Synthetic capabilities admitted" : "Mock host unavailable");
                              return ok ? sco::Result::Ok : sco::Result::Unavailable;
                          }, nullptr},
             sco::Command{"sco.mock.revoke", "Revoke mock capabilities", "Disable synthetic capabilities only", nullptr,
                          nullptr, 0, [](const sco::Arg*, uint32_t, void*, char* reply, uint32_t size) {
                              const bool ok = hostd::mock_game::ApplyProfile("none");
                              std::snprintf(reply, size, "%s", ok ? "Synthetic capabilities revoked" : "Mock host unavailable");
                              return ok ? sco::Result::Ok : sco::Result::Unavailable;
                          }, nullptr},
             sco::Command{"sco.mock.fire_respawn_loadout", "RespawnLoadoutCompleted", "Dispatch one synthetic occurrence", nullptr,
                          nullptr, 0, [](const sco::Arg*, uint32_t, void*, char* reply, uint32_t size) {
                              const bool ok = hostd::mock_game::FireRespawnLoadoutCompleted();
                              std::snprintf(reply, size, "%s", ok ? "Synthetic respawn occurrence dispatched" : "Event unavailable or refused");
                              return ok ? sco::Result::Ok : sco::Result::Unavailable;
                          }, nullptr},
             sco::Command{"sco.mock.fire_mission_changed", "Mission changed (synthetic 1)", "Dispatch synthetic mission 1 active occurrence", nullptr,
                          nullptr, 0, [](const sco::Arg*, uint32_t, void*, char* reply, uint32_t size) {
                              const bool ok = hostd::mock_game::FireMissionChanged(1, 1, 0, 0);
                              std::snprintf(reply, size, "%s", ok ? "Synthetic mission occurrence dispatched" : "Event unavailable or refused");
                              return ok ? sco::Result::Ok : sco::Result::Unavailable;
                          }, nullptr}}) {
        if (sco::RegisterCommand(this, nullptr, command) != sco::Result::Ok) {
            Stop();
            return Status::Fail("Native control command registration refused");
        }
    }
    log::Info("sco", "kernel started; " + std::to_string(Plugins().size()) + " plugin folder(s)");
    return Status::Ok();
}

void ScoHost::Tick() {
    if (!started_) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    Tick(static_cast<uint32_t>(ms));
}
void ScoHost::Tick(uint32_t nowMs) {
    if (started_) TheKernel().Tick(nowMs);
}

void ScoHost::Stop() {
    if (!started_) return;
    TheKernel().Stop();
    started_ = false;
}

std::vector<ScoPlugin> ScoHost::Plugins() const {
    std::vector<ScoPlugin> out;
    if (!started_) return out;
    for (auto& p : TheKernel().plugins()) out.push_back({p.folder, p.id, p.name, p.version, p.state, p.reason});
    return out;
}

std::vector<ScoPanel> ScoHost::Panels() const {
    std::vector<ScoPanel> out;
    if (!started_) return out;
    for (auto& p : sco::widgets::Panels()) out.push_back({p.id, p.title, p.owner});
    return out;
}

Status ScoHost::RenderPanel(const std::string& id, sco::widgets::Renderer& renderer) {
    if (!started_) return Status::Fail("sco host not started");
    const sco::Result r = sco::widgets::RenderPanel(id.c_str(), renderer);
    if (r != sco::Result::Ok) return Status::Fail(id + ": " + sco::ResultName(r));
    return Status::Ok();
}

const char* ScoHost::SdkVersion() { return SCUBA_SDK_VERSION_STRING; }
bool ScoHost::SetMockProfile(const std::string& profile) {
    return started_ && hostd::mock_game::ApplyProfile(profile);
}
Status ScoHost::InvokeControl(const std::string& name) {
    if (!started_) return Status::Fail("Product host not started");
    if (!sco::OnGameThread()) return Status::Fail("Native control requires the game thread");
    struct Reply { bool called = false; std::string text; } reply;
    const auto result = sco::Invoke(name.c_str(), nullptr, 0,
        [](sco::Result, const char* text, void* context) {
            auto& value = *static_cast<Reply*>(context);
            value.called = true;
            value.text = text ? text : "";
        }, &reply);
    if (!reply.called) return Status::Fail("Control command did not reply");
    return result == sco::Result::Ok ? Status::Ok() : Status::Fail(reply.text);
}
std::vector<ScoCapability> ScoHost::MockCapabilities() const {
    std::vector<ScoCapability> out;
    if (started_)
        for (const auto& capability : hostd::mock_game::Capabilities())
            out.push_back({capability.name, capability.ready});
    return out;
}
Status ScoHost::ReadAsopSelection(std::string& out) const {
    if (!started_) return Status::Fail("Product host not started");
    const char caller = 0;
    uint32_t size = 0;
    auto result = sco::InvokeRaw(&caller, "asop_selector.snapshot", nullptr, 0, nullptr, &size);
    if (result == sco::Result::NotFound) return Status::Fail("No saved selection; configured startup catalog applies");
    if ((result != sco::Result::Ok && result != sco::Result::TooMany) || size > 2048u * 64u)
        return Status::Fail("Selection snapshot unavailable or oversized");
    std::string next(size, '\0');
    uint32_t copied = size;
    if (size) result = sco::InvokeRaw(&caller, "asop_selector.snapshot", nullptr, 0, next.data(), &copied);
    if (result != sco::Result::Ok || copied != size || next.find('\0') != std::string::npos)
        return Status::Fail("Selection snapshot refused or changed during read");
    out = std::move(next);
    return Status::Ok();
}
std::vector<ScoStorageFile> ScoHost::StorageFiles() const {
    std::vector<ScoStorageFile> out;
    if (!started_ || !sco::storage::Started()) return out;
    for (const auto& plugin : Plugins()) {
        const auto path = sco::storage::DatabasePath(plugin.id.c_str());
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status)) continue;
        const auto bytes = std::filesystem::file_size(path, error);
        if (!error) out.push_back({plugin.id, bytes});
    }
    return out;
}
bool ScoHost::FireMockRespawnLoadout() {
    return started_ && hostd::mock_game::FireRespawnLoadoutCompleted();
}
bool ScoHost::FireMockMissionChanged(uint64_t mission, uint32_t state) {
    return started_ && hostd::mock_game::FireMissionChanged(mission, state, 0, 0);
}
uint64_t ScoHost::AddMockEntity(const std::string& name, bool actor) {
    if (!started_ || name.empty() || name.size() > 95) return 0;
    for (const unsigned char c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    auto& scene = hostd::mock_game::GetScene();
    if (scene.entities.size() >= 1024 || scene.zones.empty()) return 0;
    hostd::mock_game::Entity entity;
    entity.cls = name; entity.actor = actor; entity.zone = scene.zones.begin()->first;
    entity.streamed = true;
    scene.classes.insert(name);
    return hostd::mock_game::AddEntity(std::move(entity));
}
size_t ScoHost::MockEntityCount() const {
    return started_ ? hostd::mock_game::GetScene().entities.size() : 0;
}
Status ScoHost::ExportProduct(const std::filesystem::path& destination) {
    if (!started_ || productRoot_.empty()) return Status::Fail("Load a product profile before export");
    for (const auto& plugin : Plugins())
        if (plugin.state != "loaded" && plugin.state != "disabled" && plugin.state != "off")
            return Status::Fail("Refused or faulted plugin prevents export: " + plugin.id);
    std::string error;
    return hostd::ExportProductBundle(productRoot_, destination, error) ? Status::Ok() : Status::Fail(error);
}

}  // namespace scuba
