// SPDX-License-Identifier: GPL-3.0-only
#include "asop_probe.h"

#include <cstddef>
#include <cstring>

namespace diegetic_asop {

AsopStatus ProbeAsop(const sco_api* api) noexcept {
    AsopStatus s;
    if (!api || api->size < offsetof(sco_api, query_service) + sizeof(api->query_service) || !api->query_service)
        return s;
    const void* table = nullptr;
    s.query = api->query_service(kAsopServiceName, kAsopServiceVersion, &table);
    if (s.query != SCO_OK || !table) return s;
    std::memcpy(&s.tableSize, table, sizeof s.tableSize);
    s.active = s.tableSize >= sizeof(uint32_t);
    return s;
}

}  // namespace diegetic_asop
