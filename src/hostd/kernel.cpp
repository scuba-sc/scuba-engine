#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"

#include "sco/app.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco_lua.h"
#include "sco_lua_game.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>

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

bool RemoveStage(fs::path& stage) {
    if (stage.empty()) return true;
    std::error_code ec;
    fs::remove_all(stage, ec);
    if (ec) return false;
    stage.clear();
    return true;
}

#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
enum class KernelPhase { Idle, Starting, Running, Stopping, Retained };
struct KernelProcessRoots {
    std::mutex mutex;
    KernelPhase phase = KernelPhase::Idle;
    fs::path retainedStage;
};

KernelProcessRoots& ProcessRoots() {
    // Intentionally process-lifetime: retained SDK callbacks may outlive every
    // ordinary host object after an uncertain start or shutdown.
    static auto* roots = new KernelProcessRoots;
    return *roots;
}

#ifdef SCUBA_KERNEL_TESTING
bool g_forceCoreRefusal = false;
bool g_forceStartupRetain = false;
bool g_forceStopFailure = false;
bool RefuseDependentStop(void*) { return false; }
void PublishThenThrow() {
    mock_game::Publish();
    throw std::runtime_error("injected exception after provider exposure");
}
#endif
#endif

}  // namespace

#ifdef SCUBA_KERNEL_TESTING
namespace testing {
void ForceCoreRefusal() { g_forceCoreRefusal = true; }
void ForceStartupRetain() { g_forceStartupRetain = true; }
void ForceStopFailure() { g_forceStopFailure = true; }
}  // namespace testing

#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
const std::filesystem::path& Kernel::retainedStageForTesting() const {
    return ProcessRoots().retainedStage;
}
#endif
#endif

#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
void Kernel::RetainProcessState() noexcept {
    auto& roots = ProcessRoots();
    {
        std::lock_guard lock(roots.mutex);
        if (roots.retainedStage.empty()) roots.retainedStage.swap(profileStage_);
        roots.phase = KernelPhase::Retained;
    }
    started_ = false;
    retained_ = true;
}

Kernel::~Kernel() {
    if (started_ || retained_) std::_Exit(EXIT_FAILURE);
}

KernelStartResult Kernel::StartDetailed(const KernelOptions& options, LogFn log) {
    if (started_) return KernelStartResult::RefusedBeforeExposure;
    if (retained_) return KernelStartResult::RetainDependencies;

    auto& roots = ProcessRoots();
    {
        std::lock_guard lock(roots.mutex);
        if (roots.phase != KernelPhase::Idle) {
            profileError_ = "another kernel session is active or retained";
            return roots.phase == KernelPhase::Retained
                ? KernelStartResult::RetainDependencies
                : KernelStartResult::RefusedBeforeExposure;
        }
        roots.phase = KernelPhase::Starting;
    }

    const auto resetReservation = [&] {
        std::lock_guard lock(roots.mutex);
        if (roots.phase == KernelPhase::Starting) roots.phase = KernelPhase::Idle;
    };
    const auto failBeforeExposure = [&](KernelStartResult result) {
        if (!RemoveStage(profileStage_)) {
            profileError_ += "; stage cleanup failed; dependencies retained";
            RetainProcessState();
            return KernelStartResult::RetainDependencies;
        }
        mock_game::ResetConfiguration();
        resetReservation();
        return result;
    };

    bool providerConfigured = false;
    try {
        profileError_.clear();
        auto pluginRoot = options.pluginRoot;
        if (options.selectiveProfile && !options.productRoot.empty()) {
            profileError_ = "profile cannot be combined with productRoot";
            return failBeforeExposure(KernelStartResult::FailedCleanly);
        }

        if (options.selectiveProfile) {
            if (!StageProfile(options, profileStage_, profileError_)) {
                if (log) log(profileError_.c_str());
                return failBeforeExposure(KernelStartResult::FailedCleanly);
            }
            pluginRoot = profileStage_;
        }
        if (!options.productRoot.empty()) {
            pluginRoot = options.productRoot / "data" / "plugins";
            std::error_code ec;
            if (!std::filesystem::is_directory(pluginRoot, ec) || ec) {
                profileError_ = "Product profile requires an existing data/plugins directory";
                if (log) log(profileError_.c_str());
                return failBeforeExposure(KernelStartResult::FailedCleanly);
            }
            sco::plugins::Options discover;
            discover.enabled = true;
            discover.has = [](const char*) { return 1; };
            const auto found = sco::plugins::Discover(pluginRoot, discover);
            if (found.empty()) {
                profileError_ = "Product profile contains no plugin manifests";
                if (log) log(profileError_.c_str());
                return failBeforeExposure(KernelStartResult::FailedCleanly);
            }
            for (const auto& plugin : found) {
                if (plugin.state == sco::plugins::State::Refused) {
                    profileError_ = "Product profile refused: " + plugin.folder + ": " + plugin.reason;
                    if (log) log(profileError_.c_str());
                    return failBeforeExposure(KernelStartResult::FailedCleanly);
                }
            }
        }

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
        if (options.mockGame) {
            if (options.selectiveProfile) mock_game::ConfigureSelected(pf, options.profileServices, options.mockEvents);
            else mock_game::Configure(pf);
        }
#ifdef SCUBA_KERNEL_TESTING
        if (g_forceCoreRefusal) {
            pf.gameServices = true;
            g_forceCoreRefusal = false;
        }
        if (g_forceStartupRetain) {
            pf.setCapabilities = &PublishThenThrow;
            g_forceStartupRetain = false;
        }
        if (g_forceStopFailure) {
            pf.beforeDependentStop = &RefuseDependentStop;
            g_forceStopFailure = false;
        }
#endif
        // The supported callers use executable-lifetime log callbacks. Clear a
        // previous session's sink even when this session intentionally has no sink.
        sco::SetLogSink(log);
        providerConfigured = true;
        const sco::app::StartResult result = sco::app::StartDetailed(pf);
        if (result == sco::app::StartResult::RefusedBeforeExposure) {
            // Core classifies this invocation only. It does not prove that an
            // earlier direct core owner cannot share the provider configuration
            // just prepared above, so keep the complete consumer root until exit.
            RetainProcessState();
            profileError_ = "core refused this startup; earlier shared runtime state cannot be excluded, so dependencies remain retained until process exit";
            return KernelStartResult::RefusedBeforeExposure;
        }
        if (result == sco::app::StartResult::RetainDependencies) {
            RetainProcessState();
            profileError_ = "core startup failed after exposure; dependencies retained until process exit";
            return KernelStartResult::RetainDependencies;
        }

        started_ = true;
        {
            std::lock_guard lock(roots.mutex);
            roots.phase = KernelPhase::Running;
        }
        auto failedProfile = [&]() -> bool {
            if (options.selectiveProfile) {
                for (const auto& plugin : sco::app::Plugins()) {
                    if (std::find(options.profilePlugins.begin(), options.profilePlugins.end(), plugin.folder) != options.profilePlugins.end() &&
                        plugin.state != sco::plugins::State::Loaded) {
                        profileError_ = "profile plugin failed to load: " + plugin.folder + ": " + plugin.reason;
                        if (log) log(profileError_.c_str());
                        return true;
                    }
                }
            }
            if (!options.productRoot.empty()) {
                for (const auto& plugin : sco::app::Plugins()) {
                    if (plugin.folder.empty()) continue;
                    if (plugin.state != sco::plugins::State::Loaded) {
                        profileError_ = "Product profile plugin failed to load: " + plugin.folder + ": " + plugin.reason;
                        if (log) log(profileError_.c_str());
                        return true;
                    }
                }
            }
            return false;
        };
        if (failedProfile()) {
            const auto stopped = StopDetailed();
            if (stopped == KernelStopResult::Stopped)
                return KernelStartResult::FailedAfterCleanStop;
            // Even a retryable stop refusal follows provider exposure. Turn it
            // into the same process-terminal state as any other uncertain stop
            // before returning to a caller that may destroy this object.
            RetainProcessState();
            return KernelStartResult::RetainDependencies;
        }
        return KernelStartResult::Started;
    } catch (...) {
        // Exceptions after provider configuration may have exposed callbacks,
        // even when StartDetailed could not return its final classification.
        if (providerConfigured) RetainProcessState();
        else {
            try {
                if (!RemoveStage(profileStage_)) RetainProcessState();
                else resetReservation();
            } catch (...) { RetainProcessState(); }
        }
        try { profileError_ = "kernel startup outcome uncertain; dependencies retained until process exit"; } catch (...) {}
        return retained_ ? KernelStartResult::RetainDependencies : KernelStartResult::FailedCleanly;
    }
}

KernelStopResult Kernel::StopDetailed() {
    if (retained_) return KernelStopResult::RetainDependencies;
    if (!started_) return KernelStopResult::NotStarted;

    auto& roots = ProcessRoots();
    {
        std::lock_guard lock(roots.mutex);
        if (roots.phase != KernelPhase::Running) return KernelStopResult::Refused;
        roots.phase = KernelPhase::Stopping;
    }

    sco::Result result = sco::Result::Failed;
    try { result = sco::app::Stop(); } catch (...) {}
    if (result == sco::Result::WrongThread || result == sco::Result::Unavailable) {
        std::lock_guard lock(roots.mutex);
        roots.phase = KernelPhase::Running;
        return KernelStopResult::Refused;
    }
    if (result != sco::Result::Ok) {
        RetainProcessState();
        profileError_ = "core stop failed; dependencies retained until process exit";
        return KernelStopResult::RetainDependencies;
    }

    for (const auto& plugin : sco::app::Plugins()) {
        if (plugin.module || !plugin.pinned.empty()) {
            RetainProcessState();
            profileError_ = "core reported stop but a plugin module remains pinned";
            return KernelStopResult::RetainDependencies;
        }
    }
    if (!RemoveStage(profileStage_)) {
        RetainProcessState();
        profileError_ = "core stopped but the plugin snapshot could not be removed";
        return KernelStopResult::RetainDependencies;
    }
    sco::SetLogSink(nullptr);
    mock_game::ResetAfterCoreStop();
    started_ = false;
    {
        std::lock_guard lock(roots.mutex);
        roots.phase = KernelPhase::Idle;
    }
    return KernelStopResult::Stopped;
}

#endif

bool Kernel::Start(const KernelOptions& options, LogFn log) {
#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
    const auto result = StartDetailed(options, log);
    if (result == KernelStartResult::RetainDependencies || retained_) std::_Exit(EXIT_FAILURE);
    return result == KernelStartResult::Started;
#else
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
    if (!options.productRoot.empty()) {
        // A product profile is the whole distribution: a folder that discovers but does not load
        // is as fatal as one the preflight refused, so a half-mounted roster never reaches the UI.
        for (const auto& plugin : sco::app::Plugins()) {
            if (plugin.folder.empty()) continue;   // built-ins have no folder: not part of the roster
            if (plugin.state != sco::plugins::State::Loaded) {
                profileError_ = "Product profile plugin failed to load: " + plugin.folder + ": " + plugin.reason;
                if (log) log(profileError_.c_str());
                Stop();
                return false;
            }
        }
    }
    return true;
#endif
}

void Kernel::Tick(uint32_t nowMs) {
    if (started_) sco::app::Tick(nowMs);
}

void Kernel::Stop() {
#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
    const auto result = StopDetailed();
    if (result == KernelStopResult::Refused || result == KernelStopResult::RetainDependencies)
        std::_Exit(EXIT_FAILURE);
#else
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
#endif
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
