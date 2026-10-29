// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/sco/sco_host.h"
#include "scuba/core/log.h"

#include "hostd/kernel.h"
#include "sco/widgets.h"

#include <cstring>

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
    profileError_.clear();
    hostd::KernelOptions k;
    k.pluginRoot = options.pluginRoot;
    k.productRoot = options.productRoot;
    k.dataRoot = options.dataRoot;
    k.lua = options.lua;
    k.mockGame = options.mockGame;
    k.hostVersion = "scuba-engine " SCUBA_VERSION;
    start_ = std::chrono::steady_clock::now();
    if (!TheKernel().Start(k, &ForwardKernelLog)) {
        profileError_ = TheKernel().profileError();
        return Status::Fail(profileError_.empty() ? "sco::app::Start failed (see the sco log channel)" : profileError_);
    }
    started_ = true;
    log::Info("sco", "kernel started; " + std::to_string(Plugins().size()) + " plugin folder(s)");
    return Status::Ok();
}

void ScoHost::Tick() {
    if (!started_) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    TheKernel().Tick(static_cast<uint32_t>(ms));
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

}  // namespace scuba
