// SPDX-License-Identifier: GPL-3.0-only
// diegetic_asop: the game.asop section's switch. sco-core 2.2.0 publishes no game.asop service and
// no sc_asop.h, so the section only activates when query_service answers a game.asop 1.x table.
// Even then this build calls nothing in it: it reads the table's leading uint32_t size (the
// convention every service table follows) and reports it. The calls get wired when the public
// header ships.
#ifndef DIEGETIC_ASOP_ASOP_PROBE_H
#define DIEGETIC_ASOP_ASOP_PROBE_H

#include "sco_api.h"

#include <cstdint>

namespace diegetic_asop {

inline constexpr const char* kAsopServiceName = "game.asop";
inline constexpr uint32_t kAsopServiceVersion = 0x00010000u;  // 1.0

struct AsopStatus {
    bool active = false;                    // game.asop 1.x answered with a table
    sco_result query = SCO_UNAVAILABLE;     // query_service's answer (UNAVAILABLE on a 1.0 host)
    uint32_t tableSize = 0;                 // the table's own size field when active
};

AsopStatus ProbeAsop(const sco_api* api) noexcept;

}  // namespace diegetic_asop

#endif
