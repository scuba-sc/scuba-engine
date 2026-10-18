#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"

#include "sco/app.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco_lua.h"
#include "sco_lua_game.h"

namespace scuba::hostd {

namespace {

const sco::plugins::ScriptRuntime kLuaRuntime{ sco_lua_load, sco_lua_unload };

}  // namespace

bool Kernel::Start(const KernelOptions& options, LogFn log) {
    if (started_) return false;
    if (log) sco::SetLogSink(log);

    static bool luaExtensionAdded = false;
    if (options.lua && !luaExtensionAdded) {
        sco_lua_add_extension(&sco_lua_game_extension);
        luaExtensionAdded = true;
    }

    sco::app::Platform pf;
    pf.hostVersion = options.hostVersion ? options.hostVersion : "scuba-hostd " SCUBA_VERSION;
    pf.pluginRoot = options.pluginRoot;
    pf.pluginsEnabled = !options.pluginRoot.empty();
    pf.dataRoot = options.dataRoot;
    pf.scripts = options.lua ? &kLuaRuntime : nullptr;
    if (options.mockGame) mock_game::Configure(pf);
    started_ = sco::app::Start(pf);
    return started_;
}

void Kernel::Tick(uint32_t nowMs) {
    if (started_) sco::app::Tick(nowMs);
}

void Kernel::Stop() {
    if (!started_) return;
    sco::app::Stop();
    mock_game::Unpublish();
    started_ = false;
}

std::vector<KernelPluginInfo> Kernel::plugins() const {
    std::vector<KernelPluginInfo> out;
    for (const auto& p : sco::app::Plugins()) {
        KernelPluginInfo info;
        info.folder = p.folder;
        if (p.manifestOk) {
            info.id = p.manifest.id;
            info.name = p.manifest.name;
            info.version = p.manifest.version;
        }
        info.state = sco::plugins::StateName(p.state);
        info.reason = p.reason;
        out.push_back(std::move(info));
    }
    return out;
}

}  // namespace scuba::hostd
