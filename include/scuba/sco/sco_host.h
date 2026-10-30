// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The engine's sco-core boundary. ScoHost boots the configured kit's host kernel (sco::app) in
// process, headless, with the synthetic game services of the mock game, and owns its lifecycle:
//
//   Start()  the calling thread becomes sco-core's game thread: every other ScoHost call, and
//            every sco::widgets render, must come from it (the engine's main thread)
//   Tick()   once per engine frame, before the UI is built: plugins run their tasks, commands
//            and events here
//   Stop()   returns false if the kernel cannot safely unload and withdraw services
//
// Nothing crosses this boundary but ids, strings and copies: no plugin pointer, no ImGui type.
// Plugin UI reaches the screen only as sco.widgets panels rendered through an adapter.
#include "scuba/core/status.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace sco::widgets { class Renderer; }

namespace scuba {

struct ScoHostOptions {
    std::filesystem::path pluginRoot;   // empty: no plugin folders are discovered
    std::filesystem::path productRoot;  // a product distribution root: mounts <root>/data/plugins
    std::filesystem::path dataRoot;     // empty: no sco.storage
    bool mockGame = true;               // publish the synthetic game.* services
    bool lua = true;                    // false: lua plugins are refused
};

struct ScoPlugin {
    std::string folder, id, name, version, state, reason;
};

struct ScoPanel {
    std::string id, title, owner;
};

class ScoHost {
public:
    ScoHost() = default;
    ~ScoHost();
    ScoHost(const ScoHost&) = delete;
    ScoHost& operator=(const ScoHost&) = delete;

    Status Start(const ScoHostOptions& options);
    void Tick();
    bool Stop();
    bool started() const noexcept { return started_; }
    // Why the last Start failed, when the kernel could name a reason (a refused product plugin,
    // a missing data/plugins root); empty otherwise.
    const std::string& profileError() const noexcept { return profileError_; }

    std::vector<ScoPlugin> Plugins() const;
    std::vector<ScoPanel> Panels() const;
    // Renders panel id through renderer; events the renderer appends are validated and
    // dispatched to the owning plugin by sco-core after it returns.
    Status RenderPanel(const std::string& id, sco::widgets::Renderer& renderer);

    static const char* SdkVersion();

private:
    bool started_ = false;
    bool stopFailed_ = false;
    std::string profileError_;
    std::chrono::steady_clock::time_point start_{};
};

}  // namespace scuba
