#pragma once
// The sco-core host kernel (sco::app) booted in-process, headless and without a game: no image,
// no signatures, synthetic game services. Plugins load from pluginRoot when it is set. All calls belong
// on the thread that called Start (it becomes the sco-core game thread).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scuba::hostd {

struct KernelOptions {
    std::filesystem::path pluginRoot;   // empty: no plugin folders are discovered
    std::filesystem::path productRoot;  // sc-offline distribution root containing data/plugins
    std::filesystem::path dataRoot;     // empty: no sco.storage
    bool lua = true;                    // false: lua plugins are refused "no script runtime"
    bool mockGame = true;
    bool selectiveProfile = false;
    bool mockEvents = false;
    std::vector<std::string> profileServices;
    std::vector<std::string> profilePlugins;
    const char* hostVersion = nullptr;  // static string; null: "scuba-hostd <version>"
};

struct KernelPluginInfo {
    std::string folder;
    std::string id;
    std::string name;
    std::string version;
    std::string state;    // sco::plugins::StateName: "loaded", "refused", ...
    std::string reason;
};

class Kernel {
public:
    // Line sink for kernel log lines (any thread). Set before Start.
    using LogFn = void (*)(const char* line);

    bool Start(const KernelOptions& options, LogFn log);
    void Tick(uint32_t nowMs);
    void Stop();
    bool started() const { return started_; }
    std::vector<KernelPluginInfo> plugins() const;
    const std::string& profileError() const { return profileError_; }

private:
    bool started_ = false;
    std::string profileError_;
    std::filesystem::path profileStage_;
};

}  // namespace scuba::hostd
