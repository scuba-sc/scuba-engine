#include "hostd/modules.h"
#include "hostd/designer_export.h"

namespace scuba::hostd {

void RegisterAllModules(Registry& registry, HostContext& host) {
    RegisterSystemHandlers(registry, host);
    RegisterWidgetsHandlers(registry, host);
    RegisterMockGameHandlers(registry, host);
    RegisterDesignerHandlers(registry);
    RegisterAssetsHandlers(registry, host);
}

}  // namespace scuba::hostd
