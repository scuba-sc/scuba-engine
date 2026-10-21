// SPDX-License-Identifier: GPL-3.0-only
// diegetic_asop: game.asop 1.0 client (sc_asop.h, added in sco-core 2.3.0). The same gates as the hangar
// client, in the same order: sco_api covers query_service, game.asop 1.0 is published,
// has(<capability>) answers 1, and the provider's table size covers the function. A gate that
// fails returns SCO_UNAVAILABLE with the reason and never touches the game. Both capabilities are
// off by default in the game pack. Nothing here writes to the game.
#ifndef DIEGETIC_ASOP_ASOP_CLIENT_H
#define DIEGETIC_ASOP_ASOP_CLIENT_H

#include "hangar_client.h"
#include "sc_asop.h"
#include "sco_api.h"

#include <cstdint>
#include <string>

namespace diegetic_asop {

inline constexpr const char* kCapClaimTimeout = "game.asop.claim_timeout";
inline constexpr const char* kCapShardPersisted = "game.asop.shard_persisted";

// What the ASOP section can show on this host right now. Probing calls nothing in the table.
struct AsopStatus {
    bool active = false;                 // game.asop 1.x answered with a table
    sco_result query = SCO_UNAVAILABLE;  // query_service's answer (UNAVAILABLE on a 1.0 host)
    uint32_t tableSize = 0;              // the table's own size field when active
    bool claimTimeout = false;           // game.asop.claim_timeout is on and the table has the call
    bool shardPersisted = false;         // game.asop.shard_persisted is on and the table has the call
};

class AsopClient {
public:
    AsopClient() = default;
    AsopClient(const sco_api* api, sco_plugin* self) noexcept : api_(api), self_(self) {}

    AsopStatus Status() const noexcept;
    // On SCO_OK, outSeconds holds the insurance claim request timeout the game holds now.
    Outcome ClaimTimeout(int32_t& outSeconds) const;
    // On SCO_OK, outPersisted holds the game's IsShardPersisted flag.
    Outcome ShardPersisted(bool& outPersisted) const;

private:
    const sc_asop_v1* Table(Outcome& why) const;
    bool Has(const char* capability) const noexcept;
    Outcome Failed(const sc_asop_v1* table, const char* call, sco_result r) const;

    const sco_api* api_ = nullptr;
    sco_plugin* self_ = nullptr;
};

}  // namespace diegetic_asop

#endif
