#pragma once
#include "hostd/handlers.h"

#include <filesystem>
#include <string>

namespace scuba::hostd {

struct DesignerExportOptions {
    std::filesystem::path sdkRoot;
    std::filesystem::path outputRoot;
    std::filesystem::path python;
};

Json ExportDesignerPlugin(const Json& params, const DesignerExportOptions& options);
void RegisterDesignerHandlers(Registry& registry);

}  // namespace scuba::hostd
