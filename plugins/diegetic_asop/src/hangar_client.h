// SPDX-License-Identifier: GPL-3.0-only
// diegetic_asop: game.hangar 1.0 client. Every call is gated, in this order: sco_api covers
// query_service, game.hangar 1.0 is published, has(<capability>) answers 1, and the provider's
// table size covers the function. A gate that fails returns SCO_UNAVAILABLE with the reason and
// never touches the game.
#ifndef DIEGETIC_ASOP_HANGAR_CLIENT_H
#define DIEGETIC_ASOP_HANGAR_CLIENT_H

#include "sc_hangar.h"
#include "sco_api.h"

#include <cstdint>
#include <string>

namespace diegetic_asop {

inline constexpr const char* kCapLift = "game.hangar.lift";
inline constexpr const char* kCapAtcCancel = "game.hangar.atc_cancel";

struct Outcome {
    sco_result result = SCO_OK;
    std::string message;
    bool ok() const noexcept { return result == SCO_OK; }
};

// What the terminal can do on this host right now.
struct HangarStatus {
    bool services = false;   // sco_api is 1.1 or newer (query_service present)
    bool published = false;  // game.hangar 1.0 answered query_service
    bool lift = false;       // game.hangar.lift is on and the table has both lift functions
    bool atcCancel = false;  // game.hangar.atc_cancel is on and the table has atc_cancel_request
};

class HangarClient {
public:
    HangarClient() = default;
    HangarClient(const sco_api* api, sco_plugin* self) noexcept : api_(api), self_(self) {}

    HangarStatus Status() const noexcept;
    // On SCO_OK, outLiftId holds the lift serving the landing area.
    Outcome FindLift(uint64_t landingAreaId, uint64_t& outLiftId) const;
    Outcome RequestLift(uint64_t liftId, sc_hangar_lift_op op) const;
    Outcome CancelAtc(uint64_t atcId) const;
    // The service's last_error text for this plugin.
    Outcome LastError() const;

private:
    const sc_hangar_v1* Table(Outcome& why) const;
    bool Has(const char* capability) const noexcept;
    std::string Reason(const sc_hangar_v1* table) const;
    Outcome Failed(const sc_hangar_v1* table, const char* call, sco_result r) const;

    const sco_api* api_ = nullptr;
    sco_plugin* self_ = nullptr;
};

const char* ResultName(sco_result r) noexcept;

}  // namespace diegetic_asop

#endif
