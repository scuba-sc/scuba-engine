#include "hostd/modules.h"

namespace scuba::hostd {

void RegisterAllModules(Registry& registry, HostContext& host) {
    RegisterSystemHandlers(registry, host);
    RegisterWidgetsHandlers(registry, host);
}

}  // namespace scuba::hostd
