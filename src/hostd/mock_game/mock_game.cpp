#include "hostd/mock_game/mock_game.h"

#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sco/app.h"
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/runtime.h"

#include <iterator>

namespace scuba::hostd::mock_game {

namespace {

using ServiceFn = const ServiceDef& (*)();

constexpr ServiceFn kServices[] = {
    ActorsService,     AsopService,         ContractsService,  CreativeService,    CvarsService,
    CvarsWriteService, EconomyService,      EntitiesService,   GameSpatialService, HangarService,
    LoadoutService,    MiningService,       MissionsService,   OwnedService,       PrefabsService,
    PrivateSessionService, QuantumService,  RelocationService, ReputationService,  VehiclesService,
    WorldService,      WorldControlService, SpawnService,      TeleportSpatialService,
#ifdef SCUBA_HAVE_ACTOR_STATS
    ActorStatsService, ActorStatsWriteService,
#endif
};

struct CapDef {
    const char* name;
    const char* service;
    bool gamePack;
    bool defaultOn = true;   // in the "all" profile; false for a write switch that is off until turned on
};

// gamePack: false where the kit's header says the game pack holds the capability off.
constexpr CapDef kCaps[] = {
    { "game.actors.local_player", "game.actors", true },
    { "game.actors.spawn_npc", "game.actors", true },
    { "game.actors.despawn", "game.actors", true },
    { "game.actors.health", "game.actors", true },
    { "game.actors.state", "game.actors", false },
    { "game.asop.claim_timeout", "game.asop", false },
    { "game.asop.shard_persisted", "game.asop", false },
    { "game.contracts.stream_radius", "game.contracts", false },
    { "game.creative.god_mode", "game.creative", true },
    { "game.creative.fly", "game.creative", true },
    { "game.creative.ammo", "game.creative", true },
    { "game.creative.ship_ammo", "game.creative", true },
    { "game.cvars.read", "game.cvars", true },
    { "game.cvars.write", "game.cvars_write", true },
    { "game.economy.read", "game.economy", true },
    { "game.economy.apply", "game.economy", true },
    { "game.entities.transform", "game.entities", true },
    { "game.entities.spawn", "game.entities", true },
    { "game.entities.class_of", "game.entities", true },
    { "game.entities.query_radius", "game.entities", false },
    { "game.entities.watch", "game.entities", false },
    { "game.entities.prefabs", "game.prefabs", true },
    { "game.spatial", "game.spatial", true },
    { "game.hangar.lift", "game.hangar", false },
    { "game.hangar.atc_cancel", "game.hangar", false },
    { "game.loadout.apply", "game.loadout", true },
    { "game.loadout.weapons", "game.loadout", false },
    { "game.mining", "game.mining", false },
    { "game.missions.read", "game.missions", true },
    { "game.missions.accept", "game.missions", true },
    { "game.missions.progress", "game.missions", true },
    { "game.missions.scripts", "game.missions", true },
    { "game.owned", "game.owned", true },
    { "game.private_session", "game.private_session", true },
    { "game.quantum.control", "game.quantum", true },
    { "game.relocation", "game.relocation", false },
    { "game.reputation.checks", "game.reputation", false },
    { "game.vehicles.seats", "game.vehicles", true },
    { "game.vehicles.seat", "game.vehicles", true },
    { "game.vehicles.flight_ready", "game.vehicles", true },
    { "game.world.raycast", "game.world", true },
    { "game.world.camera", "game.world", true },
    { "game.world.free_camera_flag", "game.world", false },
    { "game.world.raycast_filtered", "game.world.control", false },
    { "game.world.free_camera", "game.world.control", false },
    { "spawn.ship", "spawn.entities", true },
    { "teleport", "teleport.spatial", true },
    { "game.events.player_spawned", "game.* events", true },
    { "game.events.player_died", "game.* events", true },
    { "game.events.actor_died", "game.* events", true },
    { "game.events.zone_changed", "game.* events", true },
    { "game.events.vehicle_seat", "game.* events", false },
#ifdef SCUBA_HAVE_ACTOR_STATS
    { "game.actor_stats.read", "game.actor_stats", true },
    { "game.actor_stats.write", "game.actor_stats_write", false, false },
#endif
};

bool g_published = false;
std::map<std::string, bool> g_servicePublished;

const sco_plugin_info* InitializerInfo() {
    static const sco_plugin_info info{ sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR,
                                       "scuba_mock_game", "1.0.0", "the maintainer" };
    return &info;
}

sco_result Initialize(const sco_api*, sco_plugin*) {
    if (!g_published) return SCO_FAILED;
    return SetCapability("game.private_session", true) ? SCO_OK : SCO_FAILED;
}

void InitializerUnload() {}

const sco::plugins::Builtin kInitializer{ "scuba_mock_game", InitializerInfo, Initialize, InitializerUnload };

void OnRelease(const void* owner) {
    MarkReleased(owner);
    for (ServiceFn fn : kServices) fn().release(owner);
}

void OnTick(const char*, const void* data, void*) {
    if (!g_published) return;
    const uint32_t nowMs = data ? *static_cast<const uint32_t*>(data) : GetScene().nowMs;
    TickScene(nowMs);
    for (ServiceFn fn : kServices)
        if (fn().tick) fn().tick();
}

const CapDef* FindCap(const std::string& name) {
    for (const auto& c : kCaps)
        if (name == c.name) return &c;
    return nullptr;
}

}  // namespace

void Configure(sco::app::Platform& platform) {
    platform.setCapabilities = Publish;
    platform.privateSession.enabled = false;
    platform.builtins = &kInitializer;
    platform.nBuiltins = 1;
}

void ResetAll() {
    ResetScene();
    for (ServiceFn fn : kServices) fn().reset();
}

void Publish() {
    static bool hookAdded = false;
    if (!hookAdded) hookAdded = sco::AddReleaseHook(OnRelease) == sco::Result::Ok;
    if (!hookAdded) {
        sco::Log("[mock_game] cleanup hook unavailable; synthetic services not published");
        return;
    }
    ResetAll();
    ApplyProfile("all");
    g_servicePublished.clear();
    size_t published = 0;
    for (ServiceFn fn : kServices) {
        const ServiceDef& def = fn();
        const sco::Result r = sco::host::ProvideGameService(def.name, def.version, def.table);
        g_servicePublished[def.name] = r == sco::Result::Ok;
        if (r == sco::Result::Ok)
            ++published;
        else
            sco::Log("[mock_game] %s not published: %s", def.name, sco::ResultName(r));
    }
    sco::Unsubscribe(nullptr, "tick", OnTick);
    g_published = sco::Subscribe(nullptr, "tick", OnTick, nullptr) == sco::Result::Ok;
    if (!g_published) sco::host::WithdrawGameServices();
    sco::Log("[mock_game] %zu of %zu synthetic game services published", published, std::size(kServices));
}

void Unpublish() {
    sco::Unsubscribe(nullptr, "tick", OnTick);
    ApplyProfile("none");
    g_published = false;
    g_servicePublished.clear();
}

bool Published() { return g_published; }

std::vector<CapabilityInfo> Capabilities() {
    std::vector<CapabilityInfo> out;
    for (const auto& c : kCaps) out.push_back(CapabilityInfo{ c.name, c.service, Ready(c.name), c.gamePack });
    return out;
}

bool SetCapability(const std::string& name, bool ready) {
    if (!FindCap(name)) return false;
    return sco::caps::Set(name.c_str(), ready, ready ? nullptr : "switched off in the scuba mock game") == sco::Result::Ok;
}

bool ApplyProfile(const std::string& profile) {
    if (profile != "all" && profile != "game_pack" && profile != "none") return false;
    for (const auto& c : kCaps) {
        const bool on = (profile == "all" && c.defaultOn) || (profile == "game_pack" && c.gamePack);
        sco::caps::Set(c.name, on, on ? nullptr : "off in the scuba mock game profile");
    }
    return true;
}

std::vector<ServiceInfo> Services() {
    std::vector<ServiceInfo> out;
    for (ServiceFn fn : kServices) {
        const ServiceDef& def = fn();
        const auto it = g_servicePublished.find(def.name);
        out.push_back(ServiceInfo{ def.name, def.version, it != g_servicePublished.end() && it->second });
    }
    return out;
}

bool SetServicePublished(const std::string& name, bool published) {
    for (ServiceFn fn : kServices) {
        const ServiceDef& def = fn();
        if (name != def.name) continue;
        bool& state = g_servicePublished[def.name];
        if (state == published) return true;
        const sco::Result r = published ? sco::host::ProvideGameService(def.name, def.version, def.table)
                                        : sco::host::WithdrawGameService(def.name);
        if (r != sco::Result::Ok) return false;
        state = published;
        return true;
    }
    return false;
}

}  // namespace scuba::hostd::mock_game
