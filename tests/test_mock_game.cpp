#include "check.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include "hostd/modules.h"
#include "sco/app.h"
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sc_actors.h"
#include "sc_creative.h"
#include "sc_cvars.h"
#include "sc_entities.h"
#include "sc_game_spatial.h"
#include "sc_game_build.h"
#include "sc_inventory.h"
#include "sc_gamerules.h"
#include "sc_input.h"
#include "sc_owned.h"
#include "sc_relocation.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace scuba::hostd;
namespace mg = scuba::hostd::mock_game;

namespace {

void Log(const char* line) { std::puts(line); }

#ifdef SCUBA_HAVE_ACTOR_STATS
constexpr size_t kActorStatsExtra = 2;
#else
constexpr size_t kActorStatsExtra = 0;
#endif

template<class T>
const T* Query(const char* name, uint32_t version = 0x10000u) {
    const void* out = nullptr;
    if (sco::host::BuildApi({ "scuba test" })->query_service(name, version, &out) != SCO_OK || !out)
        throw std::runtime_error(std::string("missing table ") + name);
    return static_cast<const T*>(out);
}

std::string Invoke(const char* name, const sco_arg* args = nullptr, uint32_t count = 0) {
    std::string reply;
    const auto result = sco::Invoke(name, reinterpret_cast<const sco::Arg*>(args), count,
        [](sco::Result r, const char* text, void* ctx) {
            CHECK(r == sco::Result::Ok);
            *static_cast<std::string*>(ctx) = text;
        }, &reply);
    CHECK(result == sco::Result::Ok);
    std::printf("%s => %s (%s)\n", name, reply.c_str(), sco::ResultName(result));
    return reply;
}

Json Call(Registry& registry, const char* request) {
    const auto out = Json::parse(registry.dispatch_text(request, {}));
    CHECK(out.has_value());
    return out ? *out : Json();
}

bool Success(const Json& response) { return response.find("result") != nullptr; }

void ToggleTests(const sco_api* api) {
    const auto caps = mg::Capabilities();
    CHECK(caps.size() == 57 + kActorStatsExtra);
    for (const auto& cap : caps) {
        CHECK(mg::SetCapability(cap.name, false));
        CHECK(api->has(cap.name.c_str()) == 0);
        CHECK(mg::SetCapability(cap.name, true));
        CHECK(api->has(cap.name.c_str()) == 1);
    }
    sco_arg arg{};
    arg.type = SCO_ARG_STRING;
    arg.v.s = "game.actors.state";
    CHECK(mg::SetCapability(arg.v.s, false));
    CHECK(Invoke("mock_game_probe.has", &arg, 1) == "0");
    uint32_t state = 77;
    CHECK(Query<sc_actors_v1>(SC_ACTORS_NAME)->state(256, &state) == SCO_UNAVAILABLE && state == 0);
    CHECK(mg::SetCapability(arg.v.s, true));
    CHECK(Invoke("mock_game_probe.has", &arg, 1) == "1");

    // 2.3.0 additions: all on, then the game_pack profile (all three held off), then a value the
    // synthetic game hasn't loaded.
    CHECK(Invoke("mock_game_probe.gated") == "0 0 0 0");
    CHECK(mg::ApplyProfile("game_pack"));
    CHECK(Invoke("mock_game_probe.gated") == "1 1 1 4");
    CHECK(mg::ApplyProfile("all"));
    mg::Asop().configLoaded = false;
    mg::Contracts().settingsLoaded = false;
    CHECK(Invoke("mock_game_probe.gated") == "2 0 2 0");
    mg::Asop().configLoaded = true;
    mg::Contracts().settingsLoaded = true;
    CHECK(!mg::SetCapability("made.up", true));
    CHECK(mg::ApplyProfile("none"));
    for (const auto& cap : mg::Capabilities()) CHECK(!cap.ready);
    CHECK(mg::ApplyProfile("game_pack"));
    CHECK(api->has("game.actors.state") == 0 && api->has("game.world.free_camera") == 0);
    CHECK(mg::ApplyProfile("all"));
    CHECK(!mg::ApplyProfile("bad"));

    const void* table = nullptr;
    CHECK(mg::SetServicePublished("game.actors", false));
    CHECK(api->query_service("game.actors", 0x10000u, &table) == SCO_NOT_FOUND && !table);
    CHECK(mg::SetServicePublished("game.actors", true));
    CHECK(api->query_service("game.actors", 0x10000u, &table) == SCO_OK);
    CHECK(api->query_service("game.actors", 0x10002u, &table) == SCO_UNAVAILABLE && !table);
    CHECK(api->query_service("game.asop", 0x10000u, &table) == SCO_OK && table);
    CHECK(api->query_service("game.contracts", 0x10000u, &table) == SCO_OK && table);
    CHECK(api->query_service(SC_INVENTORY_SERVICE_NAME, SC_INVENTORY_SERVICE_VERSION, &table) == SCO_OK && table);
    CHECK(api->query_service(SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_0, &table) == SCO_OK && table);
    CHECK(api->query_service(SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_1, &table) == SCO_OK && table);
    CHECK(api->query_service("game.loadout", 0x10001u, &table) == SCO_OK && table);
    CHECK(api->query_service("game.loadout", 0x10002u, &table) == SCO_UNAVAILABLE && !table);
    CHECK(api->query_service(SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_0, &table) == SCO_OK && table);
    CHECK(api->query_service(SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_1, &table) == SCO_OK && table);
    CHECK(api->query_service("sco.ui.surfaces", 0x10000u, &table) == SCO_OK && table);  // the kit's own, 2.3.0
}

void InventoryAndGameRulesTests() {
    const auto* inventory = Query<sc_inventory_v1>(SC_INVENTORY_SERVICE_NAME, SC_INVENTORY_SERVICE_VERSION);
    sc_inventory_snapshot snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.count = 1;
    CHECK(sc_inventory_snapshot_ports(inventory, nullptr, &snapshot) == SCO_OK);
    CHECK(snapshot.count == 1 && snapshot.total_seen == 3 && snapshot.flags == SC_INVENTORY_FLAG_TRUNCATED);
    CHECK(snapshot.generation == mg::GetScene().inventoryGeneration);
    CHECK(snapshot.ports[0].valid && snapshot.ports[0].item_class_id == 0x1234);
    CHECK(snapshot.ports[1].valid == 0);

    mg::GetScene().inventoryPorts.push_back(sc_inventory_port{ sizeof(sc_inventory_port), 0, 0, 0 });
    ++mg::GetScene().inventoryGeneration;
    snapshot.count = SC_INVENTORY_MAX_PORTS + 1;
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_OK);
    CHECK(snapshot.count == 4 && snapshot.total_seen == 4 && snapshot.flags == 0);
    CHECK(snapshot.generation == 2 && snapshot.ports[2].valid == 1 && snapshot.ports[3].valid == 0);
    snapshot.size = SC_INVENTORY_SNAPSHOT_SIZE_1_0;
    snapshot.count = 0;
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_OK && snapshot.count == 0);
    snapshot.size = SC_INVENTORY_SNAPSHOT_SIZE_1_0 - 1;
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_BAD_ARG);
    snapshot.size = sizeof(snapshot);
    snapshot.count = 0;
    mg::GetScene().inventoryAvailable = false;
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_UNAVAILABLE);
    mg::GetScene().inventoryAvailable = true;
    mg::GetScene().spawned = false;
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_UNAVAILABLE);
    mg::GetScene().spawned = true;
    CHECK(mg::SetCapability("game.inventory.read", false));
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.inventory.read", true));

    const auto* gamerules = Query<sc_gamerules_v1>(SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_0);
    sc_gamerules_state state{};
    state.size = sizeof(state);
    CHECK(sc_gamerules_read(gamerules, nullptr, &state) == SCO_OK);
    CHECK(state.valid == 1 && state.generation == 1 && state.enforce_group_qt == 1);
    CHECK(state.quantum_travel_allowed == 1 && state.quantum_boost_allowed == 0);
    CHECK(state.travel_target_threshold_angle == 12.5f && state.boost_group_distance_threshold == 2500.0f);
    mg::GetScene().gamerules.quantum_boost_allowed = 1;
    mg::GetScene().gamerules.generation = 2;
    CHECK(gamerules->read(nullptr, &state) == SCO_OK && state.generation == 2 && state.quantum_boost_allowed == 1);
    state.size = sizeof(state) - 1;
    CHECK(gamerules->read(nullptr, &state) == SCO_BAD_ARG);
    state.size = sizeof(state);
    mg::GetScene().gamerulesReceived = false;
    CHECK(gamerules->read(nullptr, &state) == SCO_UNAVAILABLE);
    mg::GetScene().gamerulesReceived = true;
    CHECK(mg::SetCapability("game.gamerules.read", false));
    CHECK(gamerules->read(nullptr, &state) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.gamerules.read", true));
    CHECK(mg::SetServicePublished(SC_INVENTORY_SERVICE_NAME, false));
    CHECK(inventory->snapshot(nullptr, &snapshot) == SCO_UNAVAILABLE);
    CHECK(mg::SetServicePublished(SC_INVENTORY_SERVICE_NAME, true));
    CHECK(mg::SetServicePublished(SC_GAMERULES_SERVICE_NAME, false));
    CHECK(gamerules->read(nullptr, &state) == SCO_UNAVAILABLE);
    uint32_t count = 0;
    const auto* input = Query<sc_input_v1>(SC_INPUT_SERVICE_NAME, SC_INPUT_SERVICE_VERSION);
    CHECK(input->count(nullptr, &count) == SCO_BAD_ARG);
    CHECK(mg::SetServicePublished(SC_GAMERULES_SERVICE_NAME, true));
}

void MetadataTests(sco_plugin* self) {
    const auto* input = Query<sc_input_v1>(SC_INPUT_SERVICE_NAME, SC_INPUT_SERVICE_VERSION);
    uint32_t count = 0;
    CHECK(input->count(self, &count) == SCO_OK && count == 1267);
    for (uint32_t id = 0; id < count - 1; ++id) {
        sc_input_action_info byId{};
        byId.size = sizeof(byId);
        CHECK(input->by_id(self, id, &byId) == SCO_OK && byId.id == id);
        sc_input_action_info byName{};
        byName.size = sizeof(byName);
        CHECK(input->by_name(self, byId.name, &byName) == SCO_OK);
        CHECK(byName.id == byId.id && byName.flags == byId.flags && std::strcmp(byName.name, byId.name) == 0);
        if (id == 0) CHECK(std::strcmp(byId.name, "FIRST") == 0 && byId.flags == SC_INPUT_ACTION_SENTINEL);
        if (id >= 187) CHECK(byId.flags & SC_INPUT_ACTION_BUILD_UNRESOLVED);
    }
    sc_input_action_info alias{};
    alias.size = sizeof(alias);
    CHECK(input->by_id(self, 1265, &alias) == SCO_OK && alias.id == 1265 && std::strcmp(alias.name, "LAST") == 0);
    CHECK(input->by_name(self, "LAST", &alias) == SCO_OK && alias.id == 1265 &&
          alias.flags == (SC_INPUT_ACTION_BUILD_UNRESOLVED | SC_INPUT_ACTION_SENTINEL));
    CHECK(input->by_name(self, "INVALID", &alias) == SCO_OK && alias.id == 1265 &&
          alias.flags == (SC_INPUT_ACTION_BUILD_UNRESOLVED | SC_INPUT_ACTION_SENTINEL));
    CHECK(input->by_name(self, "attack1", &alias) == SCO_OK && alias.id == 133);
    CHECK(input->by_name(self, "ATTACK1", &alias) == SCO_NOT_FOUND);
    CHECK(input->by_name(self, "", &alias) == SCO_BAD_ARG);
    CHECK(input->by_name(self, "not_a_real_action", &alias) == SCO_NOT_FOUND);
    alias.size = sizeof(alias) - 1;
    CHECK(input->by_id(self, 1, &alias) == SCO_BAD_ARG);

    const auto* gamerules = Query<sc_gamerules_v1>(SC_GAMERULES_SERVICE_NAME, SC_GAMERULES_VERSION_1_0);
    sc_gamerules_param_info params[SC_GAMERULES_PARAM_MAX_PAGE]{};
    uint32_t n = 99, total = 99;
    CHECK(sc_gamerules_list_params(gamerules, self, 0, nullptr, 0, 0, &n, &total) == SCO_OK);
    CHECK(n == 0 && total == 17);
    for (auto& param : params) param.size = sizeof(param);
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, sizeof(params[0]), 17, &n, &total) == SCO_OK);
    CHECK(n == 17 && total == 17);
    constexpr const char* names[] = {"capturableDebug", "phaseDebug", "objectiveDebug", "terminalsDisabledTime",
      "log", "enabled", "log", "log", "disableCollisionFilter", "QTEMode", "showRaceCheckpoints",
      "showRaceInfo", "overrideRespawnTimer", "editorUseSpawnpoints", "allowNonLocationSpawnpoints",
      "despawnOnProjectileHit", "allowAllPlayerCameras"};
    constexpr const char* groups[] = {"control", "control", "control", "control", "state", "scoring", "scoring",
      "damagehandling", "damagehandling", "accessibility", "", "", "", "", "", "", ""};
    constexpr uint32_t types[] = {1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1};
    for (uint32_t i = 0; i < 17; ++i)
        CHECK(std::strcmp(params[i].name, names[i]) == 0 && std::strcmp(params[i].group, groups[i]) == 0 &&
              params[i].type == types[i]);
    n = total = 99;
    CHECK(sc_gamerules_list_params(gamerules, self, 17, params, sizeof(params[0]), 1, &n, &total) == SCO_OK &&
          n == 0 && total == 17);
    n = total = 99;
    CHECK(sc_gamerules_list_params(gamerules, self, 18, params, sizeof(params[0]), 1, &n, &total) == SCO_BAD_ARG &&
          n == 99 && total == 99);
    CHECK(sc_gamerules_list_params(gamerules, self, 0, nullptr, sizeof(params[0]), 1, &n, &total) == SCO_BAD_ARG);
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, sizeof(params[0]) - 1, 1, &n, &total) == SCO_BAD_ARG);
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, SC_GAMERULES_PARAM_MAX_STRIDE + 1, 1,
                                   &n, &total) == SCO_BAD_ARG);
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, sizeof(params[0]),
                                   SC_GAMERULES_PARAM_MAX_PAGE + 1, &n, &total) == SCO_BAD_ARG);
    params[0].size = sizeof(params[0]) - 1;
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, sizeof(params[0]), 1, &n, &total) == SCO_BAD_ARG);
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, sizeof(params[0]), 1, &n, &n) == SCO_BAD_ARG);
    params[0].size = sizeof(params[0]);
    CHECK(mg::SetCapability("game.input.registry", false));
    CHECK(input->count(self, &count) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.input.registry", true));
    CHECK(mg::SetServicePublished(SC_INPUT_SERVICE_NAME, false));
    CHECK(input->count(self, &count) == SCO_UNAVAILABLE);
    CHECK(mg::SetServicePublished(SC_INPUT_SERVICE_NAME, true));
    CHECK(mg::SetCapability("game.gamerules.params", false));
    CHECK(sc_gamerules_list_params(gamerules, self, 0, params, sizeof(params[0]), 1, &n, &total) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.gamerules.params", true));
}

void BuildTests() {
    const auto* build = Query<sc_game_build_v1>(SC_GAME_BUILD_SERVICE_NAME);
    sc_game_build_info info{};
    info.size = sizeof(info);
    CHECK(sc_game_build_read(build, nullptr, &info) == SCO_OK);
    CHECK(info.size == sizeof(info));
    CHECK(info.pe_timestamp == 0 && info.size_of_image == 0);
    CHECK(info.pack_major == 0 && info.pack_minor == 0 && info.pack_patch == 0);
    CHECK(info.verified_build[0] == 0 && info.verified_pe_timestamp == 0 && info.pin_matches == 0);

    struct ShortInfo { uint32_t size; uint32_t timestamp; } shortInfo{ sizeof(ShortInfo), 99 };
    CHECK(build->read(nullptr, reinterpret_cast<sc_game_build_info*>(&shortInfo)) == SCO_BAD_ARG);
    CHECK(shortInfo.timestamp == 99);

    CHECK(mg::SetCapability("game.build", false));
    info = {};
    info.size = sizeof(info);
    info.pe_timestamp = 123;
    CHECK(build->read(nullptr, &info) == SCO_UNAVAILABLE && info.size == sizeof(info));
    CHECK(info.pe_timestamp == 0 && info.pin_matches == 0);
    CHECK(mg::SetCapability("game.build", true));
    info = {};
    info.size = sizeof(info);
    CHECK(build->read(nullptr, &info) == SCO_OK && info.pin_matches == 0);
}

void HandlerTests() {
    Registry registry;
    Broadcaster broadcast;
    HostContext host;
    host.broadcaster = &broadcast;
    RegisterMockGameHandlers(registry, host);
    CHECK(Success(Call(registry, R"({"id":1,"method":"mock_game.state"})")));
    CHECK(Success(Call(registry, R"({"id":2,"method":"mock_game.caps.set","params":{"name":"game.events.player_spawned","ready":false}})")));
    const std::string before = Invoke("mock_game_probe.events");
    const Json noEvent = Call(registry, R"({"id":3,"method":"mock_game.event.player_spawned"})");
    CHECK(Success(noEvent) && !noEvent.find("result")->find("fired")->as_bool());
    CHECK(Invoke("mock_game_probe.events") == before);
    CHECK(Success(Call(registry, R"({"id":4,"method":"mock_game.caps.set","params":{"name":"game.events.player_spawned","ready":true}})")));
    CHECK(Success(Call(registry, R"({"id":5,"method":"mock_game.event.player_spawned","params":{"zone":4,"pos":[0,0,0]}})")));
    CHECK(Success(Call(registry, R"({"id":6,"method":"mock_game.event.player_died"})")));
    uint64_t actor = 99;
    CHECK(Query<sc_actors_v1>(SC_ACTORS_NAME)->local_player(&actor, nullptr) == SCO_NOT_FOUND && actor == 0);
    CHECK(Success(Call(registry, R"({"id":7,"method":"mock_game.event.player_spawned"})")));
    CHECK(Success(Call(registry, R"({"id":8,"method":"mock_game.event.actor_died","params":{"entity":288}})")));
    CHECK(mg::FindEntity(288)->health == 0.0f);
    CHECK(Success(Call(registry, R"({"id":9,"method":"mock_game.event.vehicle_seated","params":{"ship":257,"seat":0}})")));
    CHECK(mg::GetScene().playerShip == 257);
    CHECK(Success(Call(registry, R"({"id":10,"method":"mock_game.event.vehicle_seated","params":{"boarded":false}})")));
    CHECK(mg::GetScene().playerShip == 0);
    CHECK(Success(Call(registry, R"({"id":11,"method":"mock_game.player.move","params":{"zone":3,"pos":[1,2,3]}})")));
    CHECK(Success(Call(registry, R"({"id":12,"method":"mock_game.player.move","params":{"zone":4,"pos":[0,0,0]}})")));
    CHECK(Success(Call(registry, R"({"id":13,"method":"mock_game.camera.move","params":{"pos":[10,20,30],"rot":[0,0,0,2]}})")));
    CHECK(mg::GetScene().cameraPos[1] == 20 && mg::GetScene().cameraRot[3] == 1);
    CHECK(Call(registry, R"({"id":14,"method":"mock_game.camera.move","params":{"pos":[1,2]}})").find("error"));
    CHECK(Call(registry, R"({"id":15,"method":"mock_game.event.actor_died","params":{"entity":999}})").find("error"));
    const auto events = broadcast.drain();
    CHECK(!events.empty());
    const auto parsed = Json::parse(events.front());
    CHECK(parsed && parsed->find("event")->as_string() == "mock_game.changed" && parsed->find("data"));
}

void RelocationTests(sco_plugin* self) {
    const auto* relocate = Query<sc_relocation_v1>(SC_RELOCATION_NAME);
    sc_relocation_request req{};
    req.size = sizeof(req);
    req.timeout_ms = 1000;
    req.zone_id = 4;
    req.pos[0] = 30;
    req.rot_xyzw[3] = 1;
    uint64_t ticket = 0;
    CHECK(relocate->request(self, &req, &ticket) == SCO_OK);
    sco::app::Tick(40);
    sco::app::Tick(50);
    sc_relocation_status status{};
    status.size = sizeof(status);
    CHECK(relocate->status(self, ticket, &status) == SCO_OK && status.state == SC_RELOCATION_COMPLETE);
    CHECK(mg::FindEntity(256)->pos[0] == 30);
    req.zone_id = 5;
    CHECK(relocate->request(self, &req, &ticket) == SCO_FAILED);
    req.zone_id = 4;
    req.timeout_ms = 1;
    CHECK(relocate->request(self, &req, &ticket) == SCO_OK);
    sco::app::Tick(60);
    CHECK(relocate->status(self, ticket, &status) == SCO_OK && status.reason == SC_RELOCATION_REASON_TIMEOUT);
}

void Quotas(sco_plugin* self) {
    const auto* actors = Query<sc_actors_v1>(SC_ACTORS_NAME);
    const auto* entities = Query<sc_entities_v1>(SC_ENTITIES_NAME);
    constexpr double pos[3]{ 0, 0, 5 }, rot[4]{ 0, 0, 0, 1 };
    uint64_t id = 0;
    for (uint32_t i = 0; i < SC_ACTORS_MAX_NPCS; ++i)
        CHECK(actors->spawn_npc(self, "MockNPC_Guard", 4, pos, &id) == SCO_OK);
    CHECK(actors->spawn_npc(self, "MockNPC_Guard", 4, pos, &id) == SCO_TOO_MANY && id == 0);
    for (uint32_t i = 0; i < SC_ENTITIES_MAX_OWNED; ++i)
        CHECK(entities->spawn(self, "MockProp_Crate", 4, pos, rot, &id) == SCO_OK);
    CHECK(entities->spawn(self, "MockProp_Crate", 4, pos, rot, &id) == SCO_TOO_MANY && id == 0);
}

void BoundaryTests(sco_plugin* self) {
    const auto* spatial = Query<sc_game_spatial_v1>(SC_GAME_SPATIAL_NAME);
    sc_game_zone record{};
    record.size = sizeof(record);
    CHECK(spatial->find_zone("MockSystem_Planet", &record) == 0);
    CHECK(spatial->zone_record(2, &record) == 1 && record.system_id == 0);
    const uint64_t oldEpoch = record.epoch;
    uint32_t count = 99, more = 99;
    uint64_t epoch = 99;
    sco::app::Tick(80);
    CHECK(spatial->list_zones(oldEpoch, 0, nullptr, 0, 0, &count, &more, &epoch) == 0 &&
          count == 99 && more == 99 && epoch == 99);
    constexpr uint32_t stride = sizeof(sc_game_zone) + 1;
    alignas(sc_game_zone) unsigned char bytes[stride * 2];
    std::memset(bytes, 0xa5, sizeof(bytes));
    const uint32_t advertised = sizeof(sc_game_zone) + 1;
    std::memcpy(bytes, &advertised, sizeof(advertised));
    std::memcpy(bytes + stride, &advertised, sizeof(advertised));
    CHECK(spatial->list_zones(0, 0, reinterpret_cast<sc_game_zone*>(bytes), stride, 2,
                              &count, &more, &epoch) == 1);
    CHECK(count == 2 && bytes[sizeof(sc_game_zone)] == 0xa5 && bytes[sizeof(bytes) - 1] == 0xa5);
    double alias[7]{};
    constexpr double pos[3]{ 0, 0, 1 }, rot[4]{ 0, 0, 0, 1 };
    CHECK(spatial->pose_to_zone(4, 3, pos, rot, alias, alias + 2) == 0);
    const auto* entities = Query<sc_entities_v1>(SC_ENTITIES_NAME);
    uint64_t entity = 0;
    CHECK(entities->spawn(self, "MockProp_Crate", 4, pos, rot, &entity) == SCO_OK);
    const auto* owned = Query<sc_owned_v1>(SC_OWNED_NAME);
    CHECK(owned->retain_build(self, entity) == SCO_OK);
    uint64_t ticket = 99;
    CHECK(mg::SetCapability("game.entities.spawn", false));
    CHECK(owned->remove_build(self, entity, &ticket) == SCO_UNAVAILABLE && ticket == 0);
    CHECK(mg::SetCapability("game.entities.spawn", true));
    CHECK(owned->remove_build(self, entity, &ticket) == SCO_OK);
    const auto* actors = Query<sc_actors_v1>(SC_ACTORS_NAME);
    double invalid[3]{ 0, 0, std::numeric_limits<double>::infinity() };
    CHECK(actors->spawn_npc(self, "MockNPC_Guard", 4, invalid, &entity) == SCO_BAD_ARG && entity == 0);
}

}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    sco::SetLogSink(Log);
    sco::app::Platform platform;
    platform.hostVersion = "scuba mock game test";
    platform.pluginRoot = argv[1];
    platform.pluginsEnabled = true;
    mg::Configure(platform);
    CHECK(sco::app::Start(platform));
    try {
        CHECK(sco::app::Plugins().size() == 2);
        const auto& plugin = sco::app::Plugins().back();
        CHECK(plugin.manifest.id == "mock_game_probe" && plugin.state == sco::plugins::State::Loaded);
        if (plugin.state != sco::plugins::State::Loaded) throw std::runtime_error(plugin.reason);
        CHECK(mg::Services().size() == 28 + kActorStatsExtra);
        for (const auto& service : mg::Services()) CHECK(service.published);
        const sco_api* api = sco::host::BuildApi({ platform.hostVersion });
        ToggleTests(api);
        BuildTests();
        InventoryAndGameRulesTests();
        MetadataTests(const_cast<sco_plugin*>(plugin.self));
        HandlerTests();
        const mg::Vec3 cameraBefore = mg::GetScene().cameraPos;
        const sco_plugin* oldHandle = plugin.self;
        CHECK(Invoke("mock_game_probe.seed").find("0 failures") != std::string::npos);
        sco::app::Tick(10);
        sco::app::Tick(20);
        CHECK(Invoke("mock_game_probe.run").find("0 failures") != std::string::npos);
        RelocationTests(plugin.self);
        std::vector<uint64_t> mustRemove, mustKeep;
        for (const auto& [id, e] : mg::GetScene().entities) {
            if (e.kept || e.origin == mg::Origin::SpawnNear || e.origin == mg::Origin::SpawnAs) mustKeep.push_back(id);
            else if (e.owner == plugin.self) mustRemove.push_back(id);
        }
        CHECK(!mustRemove.empty() && !mustKeep.empty());
        CHECK(sco::app::Reload("mock_game_probe") == sco::Result::Ok);
        CHECK(!mg::LivePlugin(oldHandle));
        sco::app::Tick(70);
        for (uint64_t id : mustRemove) CHECK(!mg::FindEntity(id));
        for (uint64_t id : mustKeep) CHECK(mg::FindEntity(id) && !mg::FindEntity(id)->owner);
        CHECK(mg::GetScene().cameraPos == cameraBefore && !mg::GetScene().freeCameraFlag);
        uint32_t flags = 99;
        const auto* creative = Query<sc_creative_v1>(SC_CREATIVE_SERVICE_NAME);
        const auto* cvars = Query<sc_cvars_v1>(SC_CVARS_NAME);
        const auto& reloaded = sco::app::Plugins().back();
        CHECK(creative->state(reloaded.self, &flags) == SCO_OK && flags == 0);
        CHECK(cvars->get_bool(reloaded.self, "mock_hud_enabled", 1, &flags) == SCO_OK && flags == 1);
        uint64_t id = 0;
        constexpr double pos[3]{ 0, 0, 1 };
        CHECK(Query<sc_actors_v1>(SC_ACTORS_NAME)->spawn_npc(
            const_cast<sco_plugin*>(oldHandle), "MockNPC_Guard", 4, pos, &id) == SCO_BAD_ARG);
        BoundaryTests(reloaded.self);
        mg::ResetAll();
        Quotas(reloaded.self);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mock game test: %s\n", e.what());
        CHECK(false);
    }
    sco::app::Stop();
    mg::Unpublish();
    CHECK(!mg::Published());
    CHECK(!sco::caps::Has("game.actors.local_player"));
    const void* table = nullptr;
    CHECK(sco::QueryService("game.actors", 0x10000u, &table) != sco::Result::Ok);
    return CheckExit("test_mock_game");
}
