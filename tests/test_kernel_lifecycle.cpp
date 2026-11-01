// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "hostd/kernel.h"
#include "hostd/mock_game/mock_game.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using scuba::hostd::Kernel;
using scuba::hostd::KernelOptions;
using scuba::hostd::KernelStartResult;
using scuba::hostd::KernelStopResult;

namespace {
KernelOptions Options(const fs::path& sourceRoot) {
    KernelOptions options;
    options.selectiveProfile = true;
    options.lua = false;
    options.pluginRoot = sourceRoot;
    options.profilePlugins = {"empty_data"};
    return options;
}

bool StageExists(const Kernel& kernel) {
    return !kernel.stagedPathForTesting().empty() && fs::is_directory(kernel.stagedPathForTesting());
}

[[noreturn]] void ExitWithoutDestroyingRetainedState() {
    const int result = CheckExit("kernel_lifecycle_retained");
    std::fflush(nullptr);
    std::_Exit(result);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view test = argv[1];
    const fs::path sourceRoot = fs::temp_directory_path() / ("scuba-kernel-lifecycle-source-" + std::string(test));
    std::error_code ec;
    fs::remove_all(sourceRoot, ec);
    fs::create_directories(sourceRoot / "empty_data");
    std::ofstream(sourceRoot / "empty_data" / "plugin.ini")
        << "id = empty_data\nname = Empty data fixture\nversion = 1.0.0\napi = 1.1\nkind = data\n";
    Kernel kernel;
    const auto options = Options(sourceRoot);

    if (test == "clean") {
        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::Started);
        CHECK(kernel.started());
        CHECK(scuba::hostd::mock_game::Published());
        CHECK(StageExists(kernel));

        Kernel competing;
        CHECK(competing.StartDetailed(options, nullptr) == KernelStartResult::RefusedBeforeExposure);
        CHECK(scuba::hostd::mock_game::Published());

        KernelStopResult otherThreadStop = KernelStopResult::NotStarted;
        std::thread wrongThread([&] { otherThreadStop = kernel.StopDetailed(); });
        wrongThread.join();
        CHECK(otherThreadStop == KernelStopResult::Refused);
        CHECK(kernel.started());
        CHECK(scuba::hostd::mock_game::Published());

        CHECK(kernel.StopDetailed() == KernelStopResult::Stopped);
        CHECK(!kernel.started() && !kernel.retained());
        CHECK(!scuba::hostd::mock_game::Published());
        CHECK(kernel.stagedPathForTesting().empty());

        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::Started);
        CHECK(kernel.StopDetailed() == KernelStopResult::Stopped);
        return CheckExit("kernel_lifecycle_clean");
    }

    if (test == "refusal") {
        KernelOptions invalid;
        invalid.productRoot = sourceRoot;
        CHECK(kernel.StartDetailed(invalid, nullptr) == KernelStartResult::FailedCleanly);
        CHECK(!kernel.started() && !kernel.retained());
        CHECK(kernel.stagedPathForTesting().empty());

        scuba::hostd::testing::ForceCoreRefusal();
        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::RefusedBeforeExposure);
        CHECK(!kernel.started() && kernel.retained());
        CHECK(!scuba::hostd::mock_game::Published());
        CHECK(kernel.stagedPathForTesting().empty());
        CHECK(!kernel.retainedStageForTesting().empty());
        CHECK(fs::is_directory(kernel.retainedStageForTesting()));
        ExitWithoutDestroyingRetainedState();
    }

    if (test == "startup-retained") {
        scuba::hostd::testing::ForceStartupRetain();
        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::RetainDependencies);
        CHECK(kernel.retained() && !kernel.started());
        CHECK(scuba::hostd::mock_game::Published());
        CHECK(kernel.stagedPathForTesting().empty());
        CHECK(!kernel.retainedStageForTesting().empty());
        CHECK(fs::is_directory(kernel.retainedStageForTesting()));
        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::RetainDependencies);
        ExitWithoutDestroyingRetainedState();
    }

    if (test == "stop-retained") {
        scuba::hostd::testing::ForceStopFailure();
        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::Started);
        const auto stage = kernel.stagedPathForTesting();
        CHECK(StageExists(kernel));
        CHECK(kernel.StopDetailed() == KernelStopResult::RetainDependencies);
        CHECK(kernel.retained() && !kernel.started());
        CHECK(scuba::hostd::mock_game::Published());
        CHECK(fs::is_directory(stage));
        CHECK(kernel.StartDetailed(options, nullptr) == KernelStartResult::RetainDependencies);
        ExitWithoutDestroyingRetainedState();
    }

    return 2;
}
