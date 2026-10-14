#pragma once
// The mock game services, one file each. Each provides a ServiceDef that mock_game.cpp publishes
// with sco::host::ProvideGameService.
#include "sco_api.h"
#include "sc_reputation.h"
#include "sc_world.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace scuba::hostd::mock_game {

struct ServiceDef {
    const char* name;
    uint32_t version;
    const void* table;
    void (*reset)();                     // back to the default state, before plugins load
    void (*release)(const void* owner);  // a plugin unloaded or crashed
    void (*tick)();                      // once per host tick, after streaming; may be null
};

const ServiceDef& ActorsService();
const ServiceDef& CreativeService();
const ServiceDef& CvarsService();
const ServiceDef& CvarsWriteService();
const ServiceDef& EconomyService();
const ServiceDef& EntitiesService();
const ServiceDef& GameSpatialService();
const ServiceDef& HangarService();
const ServiceDef& LoadoutService();
const ServiceDef& MiningService();
const ServiceDef& MissionsService();
const ServiceDef& OwnedService();
const ServiceDef& PrefabsService();
const ServiceDef& PrivateSessionService();
const ServiceDef& QuantumService();
const ServiceDef& RelocationService();
const ServiceDef& ReputationService();
const ServiceDef& VehiclesService();
const ServiceDef& WorldService();
const ServiceDef& WorldControlService();
const ServiceDef& SpawnService();
const ServiceDef& TeleportSpatialService();

// game.world's ray cast (world.cpp), shared with game.world.control's filtered cast. skip: entity
// ids the ray passes through. SCO_OK with *hit filled, or SCO_NOT_FOUND with `why`.
sco_result CastRay(uint64_t zone, const double from[3], const double dir[3], double maxDist,
                   const std::set<uint64_t>& skip, sc_world_hit* hit, std::string& why);

// Service state the hostd handlers read or edit.
int64_t& EconomyBalance();
std::map<std::string, uint32_t>& CvarValues();
const std::array<std::string, 8>& EquippedItems();
sc_reputation_checks& ReputationChecks();
bool AddSessionPeer();
void BumpSpatialEpoch();

}  // namespace scuba::hostd::mock_game
