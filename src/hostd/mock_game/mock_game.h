#pragma once
// scuba-hostd's mock game (docs/mock-game.md): synthetic game.* service tables, the game.* bus
// events and capabilities, over one editable in-memory scene, so game plugins load and run with
// no game. Publish is the sco::app::Platform::setCapabilities hook: Start calls it on the game
// thread before any plugin loads, and Stop withdraws the services after every plugin unloads.
#include <string>
#include <vector>

namespace sco::app { struct Platform; }

namespace scuba::hostd::mock_game {

void Configure(sco::app::Platform& platform);
// Resets the scene and every service, applies the "all" profile and publishes every table.
void Publish();
// After sco::app::Stop: forgets the published state (the kit already withdrew the tables).
void Unpublish();
bool Published();

// Scene and every service back to their defaults (plugins' ownership included).
void ResetAll();

struct CapabilityInfo {
    std::string name;
    std::string service;   // the table or event set it gates
    bool ready = false;
    bool gamePackDefault = false;   // what the real game pack 0.5.0 documents for it
};
std::vector<CapabilityInfo> Capabilities();
// False for a name that isn't one of the mock's capabilities.
bool SetCapability(const std::string& name, bool ready);
// "all": every capability on. "game_pack": the documented game-pack defaults (header comments
// that say a capability is held off until an in-game run confirms it). "none": every one off.
bool ApplyProfile(const std::string& profile);

struct ServiceInfo {
    std::string name;
    unsigned version = 0;
    bool published = false;
};
std::vector<ServiceInfo> Services();
// Withdraws or re-publishes one table (query_service then answers NOT_FOUND or the table).
bool SetServicePublished(const std::string& name, bool published);

}  // namespace scuba::hostd::mock_game
