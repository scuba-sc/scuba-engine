// SPDX-License-Identifier: GPL-3.0-only
// diegetic_shop_inv: capability-gated clients for game.economy 1.0, game.loadout 1.0,
// game.prefabs 1.0 and a presence probe for sco.datacore 1.x.
//
// Every call re-queries the service (tables are session-lifetime and owned by the host) and
// passes four gates before touching a table: sco_api covers query_service; the service is
// published at a compatible version; has(<capability>) answers 1; the table's size covers the
// member and the pointer is non-null. A closed gate returns SCO_UNAVAILABLE and a reason.
#ifndef DIEGETIC_SHOP_INV_SERVICES_H
#define DIEGETIC_SHOP_INV_SERVICES_H

#include "catalogue.h"

#include "sc_economy.h"
#include "sc_loadout.h"
#include "sc_prefabs.h"
#include "sco_api.h"

#include <cstdint>
#include <string>

namespace shop {

inline constexpr char kCapEconomyRead[] = "game.economy.read";
inline constexpr char kCapEconomyApply[] = "game.economy.apply";
inline constexpr char kCapLoadoutApply[] = "game.loadout.apply";
inline constexpr char kCapPrefabsSpawn[] = "game.entities.prefabs";

const char* ResultName(sco_result r) noexcept;

class Services {
public:
    void Bind(const sco_api* api, sco_plugin* self) noexcept {
        api_ = api;
        self_ = self;
    }

    // One clause per service: "economy: ready; loadout: off; prefabs: not published; datacore: ...".
    std::string Describe() const;

    sco_result Balance(int64_t& out, std::string& why) const;
    // Reads the balance, refuses when it is below price, then applies -price under key.
    sco_result Pay(uint64_t key, int64_t price, sc_economy_receipt& out, std::string& why) const;
    sco_result Equip(const Item& item, sc_loadout_report& report, std::string& why) const;
    sco_result Spawn(const Item& item, uint64_t zone, const double pos[3], uint64_t& entity, std::string& why) const;
    // "open", "loaded", or why sco.datacore is not usable.
    std::string DataCoreState() const;

private:
    sco_result Query(const char* name, uint32_t version, const void** out, std::string& why) const;
    bool Has(const char* capability) const noexcept;

    const sco_api* api_ = nullptr;
    sco_plugin* self_ = nullptr;
};

}  // namespace shop

#endif
