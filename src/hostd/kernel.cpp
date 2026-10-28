#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"

#include "sco/app.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco_lua.h"
#include "sco_lua_game.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <set>

namespace scuba::hostd {

namespace {

const sco::plugins::ScriptRuntime kLuaRuntime{ sco_lua_load, sco_lua_unload };
namespace fs = std::filesystem;

// The published app API discovers a whole directory. Give it an isolated, bounded
// snapshot so its normal dependency resolution, ownership and unload rules apply.
bool StageProfile(const KernelOptions& options, fs::path& stage, std::string& error) {
    if (options.profilePlugins.empty()) return true;
    std::set<std::string> ids;
    for (const auto& id : options.profilePlugins) {
        if (id.empty() || id.size() > sco::plugins::kMaxIdLen ||
            !std::all_of(id.begin(), id.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            }) || !ids.insert(id).second) {
            error = "invalid or duplicate profile plugin id: " + id; return false;
        }
    }
    if (ids.size() > sco::plugins::kMaxPlugins) { error = "too many profile plugins"; return false; }
    std::error_code ec;
    const auto temp = fs::temp_directory_path(ec);
    if (ec) { error = "cannot find temporary directory"; return false; }
    std::random_device random;
    for (int attempt = 0; attempt < 32 && stage.empty(); ++attempt) {
        auto candidate = temp / ("scuba-profile-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (fs::create_directory(candidate, ec)) stage = candidate;
        else if (ec && ec != std::errc::file_exists) break;
        ec.clear();
    }
    if (stage.empty()) { error = "cannot create profile staging directory"; return false; }
    fs::permissions(stage, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) { error = "cannot secure profile staging directory"; return false; }
    constexpr uintmax_t maxBytes = 256u * 1024u * 1024u;
    constexpr size_t maxEntries = 4096;
    size_t entries = 0;
    uintmax_t bytes = 0;
    for (const auto& id : ids) {
        const auto source = options.pluginRoot / id;
        if (fs::symlink_status(source, ec).type() != fs::file_type::directory || ec) {
            error = "profile plugin folder unavailable: " + id; return false;
        }
        fs::create_directory(stage / id, ec);
        if (ec) { error = "cannot stage plugin: " + id; return false; }
        fs::recursive_directory_iterator it(source, ec), end;
        if (ec) { error = "cannot read plugin: " + id; return false; }
        for (; it != end; it.increment(ec)) {
            if (ec) { error = "cannot read plugin: " + id; return false; }
            if (++entries > maxEntries || it.depth() > 32) { error = "profile plugin snapshot exceeds entry or depth limit"; return false; }
            const auto type = it->symlink_status(ec).type();
            if (ec || (type != fs::file_type::directory && type != fs::file_type::regular)) {
                error = "profile plugin contains a symlink or unsupported file: " + id; return false;
            }
            const auto target = stage / id / it->path().lexically_relative(source);
            if (type == fs::file_type::directory) fs::create_directory(target, ec);
            else {
                const auto size = it->file_size(ec);
                if (ec || size > maxBytes - bytes) { error = "profile plugin snapshot exceeds byte limit or cannot be read"; return false; }
                std::ifstream input(it->path(), std::ios::binary);
                std::ofstream output(target, std::ios::binary);
                if (!input || !output) { error = "cannot copy profile plugin: " + id; return false; }
                std::array<char, 65536> buffer{};
                while (input) {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = static_cast<uintmax_t>(input.gcount());
                    if (count > maxBytes - bytes) { error = "profile plugin snapshot exceeds byte limit"; return false; }
                    bytes += count;
                    output.write(buffer.data(), static_cast<std::streamsize>(count));
                    if (!output) { error = "cannot copy profile plugin: " + id; return false; }
                }
                if (!input.eof()) { error = "cannot read profile plugin: " + id; return false; }
                output.close();
                if (!output) { error = "cannot finish copying profile plugin: " + id; return false; }
            }
            if (ec) { error = "cannot copy profile plugin: " + id; return false; }
        }
        if (ec) { error = "cannot read plugin: " + id; return false; }
    }
    sco::plugins::Options discover;
    discover.enabled = true;
    discover.has = [](const char*) { return 1; };
    const auto found = sco::plugins::Discover(stage, discover);
    if (found.size() != ids.size()) { error = "profile plugin manifest missing"; return false; }
    for (const auto& plugin : found) {
        if (plugin.state != sco::plugins::State::Ready) {
            error = "profile plugin refused: " + plugin.folder + ": " + plugin.reason; return false;
        }
    }
    return true;
}

void RemoveStage(fs::path& stage) {
    if (stage.empty()) return;
    std::error_code ec;
    fs::remove_all(stage, ec);
    stage.clear();
}

}  // namespace

bool Kernel::Start(const KernelOptions& options, LogFn log) {
    if (started_) return false;
    profileError_.clear();
    auto pluginRoot = options.pluginRoot;
    if (options.selectiveProfile && !options.productRoot.empty()) {
        profileError_ = "profile cannot be combined with productRoot";
        return false;
    }
    if (options.selectiveProfile) {
        if (!StageProfile(options, profileStage_, profileError_)) {
            RemoveStage(profileStage_);
            if (log) log(profileError_.c_str());
            return false;
        }
        pluginRoot = profileStage_;
    }
    if (!options.productRoot.empty()) {
        pluginRoot = options.productRoot / "data" / "plugins";
        std::error_code ec;
        if (!std::filesystem::is_directory(pluginRoot, ec) || ec) {
            profileError_ = "Product profile requires an existing data/plugins directory";
            if (log) log(profileError_.c_str());
            return false;
        }
        sco::plugins::Options discover;
        discover.enabled = true;
        // Preflight validates manifests/entries only; live capability admission
        // is evaluated again by the kernel after mock providers are published.
        discover.has = [](const char*) { return 1; };
        const auto found = sco::plugins::Discover(pluginRoot, discover);
        if (found.empty()) {
            profileError_ = "Product profile contains no plugin manifests";
            if (log) log(profileError_.c_str());
            return false;
        }
        for (const auto& plugin : found) {
            if (plugin.state == sco::plugins::State::Refused) {
                profileError_ = "Product profile refused: " + plugin.folder + ": " + plugin.reason;
                if (log) log(profileError_.c_str());
                return false;
            }
        }
    }
    if (log) sco::SetLogSink(log);

    static bool luaExtensionAdded = false;
    if (options.lua && !luaExtensionAdded) {
        sco_lua_add_extension(&sco_lua_game_extension);
        luaExtensionAdded = true;
    }

    sco::app::Platform pf;
    pf.hostVersion = options.hostVersion ? options.hostVersion : "scuba-hostd " SCUBA_VERSION;
    pf.pluginRoot = pluginRoot;
    pf.pluginsEnabled = !pluginRoot.empty();
    pf.dataRoot = options.dataRoot;
    pf.scripts = options.lua ? &kLuaRuntime : nullptr;
    const bool mockWasPublished = mock_game::Published();
    if (options.mockGame) {
        if (options.selectiveProfile) mock_game::ConfigureSelected(pf, options.profileServices, options.mockEvents);
        else mock_game::Configure(pf);
    }
    started_ = sco::app::Start(pf);
    if (!started_) {
        if (!mockWasPublished && mock_game::Published()) mock_game::Unpublish();
        RemoveStage(profileStage_);
        return false;
    }
    if (options.selectiveProfile) {
        for (const auto& plugin : sco::app::Plugins()) {
            if (std::find(options.profilePlugins.begin(), options.profilePlugins.end(), plugin.folder) != options.profilePlugins.end() &&
                plugin.state != sco::plugins::State::Loaded) {
                profileError_ = "profile plugin failed to load: " + plugin.folder + ": " + plugin.reason;
                if (log) log(profileError_.c_str());
                Stop();
                return false;
            }
        }
    }
    return true;
}

void Kernel::Tick(uint32_t nowMs) {
    if (started_) sco::app::Tick(nowMs);
}

void Kernel::Stop() {
    if (!started_) return;
    sco::app::Stop();
    mock_game::Unpublish();
    started_ = false;
    // A faulted or pinned native may still refer to its files. Leave the snapshot
    // in place in that case; the SDK deliberately retains the module as well.
    bool retained = false;
    for (const auto& plugin : sco::app::Plugins())
        if (plugin.module || !plugin.pinned.empty()) retained = true;
    if (!retained) RemoveStage(profileStage_);
    else profileStage_.clear();
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
