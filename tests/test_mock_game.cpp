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
#include "sc_owned.h"
#include "sc_relocation.h"
#include "sc_actor_stats.h"
#include "sc_inventory.h"
#include "sc_gamerules.h"
#include "sc_loadout.h"
#include "sc_game_events.h"
#include "sc_datacore_live.h"
#include "sc_missions.h"
#include "sc_quantum.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace scuba::hostd;
namespace mg = scuba::hostd::mock_game;

namespace {

void Log(const char* line) { std::puts(line); }

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
    CHECK(caps.size() == 60);
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
    CHECK(api->query_service("game.loadout", 0x10001u, &table) == SCO_OK && table);
    CHECK(api->query_service("game.loadout", 0x10002u, &table) == SCO_OK && table);
    CHECK(api->query_service("game.gamerules", 0x10000u, &table) == SCO_OK && table);  // 2.4.0
    CHECK(api->query_service("sco.ui.surfaces", 0x10000u, &table) == SCO_OK && table);  // the kit's own, 2.3.0
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
    req.timeout_ms = 1000;
    CHECK(relocate->request(self, &req, &ticket) == SCO_OK);
    CHECK(mg::SetCapability("game.relocation", false));
    sco::app::Tick(61);
    CHECK(relocate->status(self, ticket, &status) == SCO_OK && status.reason == SC_RELOCATION_REASON_SAFETY);
    CHECK(mg::SetCapability("game.relocation", true));
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
        CHECK(mg::Services().size() == 28);
        for (const auto& service : mg::Services()) CHECK(service.published);
        const sco_api* api = sco::host::BuildApi({ platform.hostVersion });
        ToggleTests(api);
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
        const auto* quantum = Query<sc_quantum_v1>(SC_QUANTUM_SERVICE_NAME);
        mg::GetScene().playerShip = 257;
        CHECK(quantum->enable(reloaded.self, 1) == SCO_OK);
        CHECK(quantum->hold(reloaded.self, 100) == SCO_OK);
        CHECK(mg::SetCapability("game.quantum.control", false));
        CHECK(quantum->release(reloaded.self) == SCO_OK);
        CHECK(quantum->enable(reloaded.self, 0) == SCO_OK);
        CHECK(mg::SetCapability("game.quantum.control", true));
        mg::GetScene().playerShip = 0;
        const auto* missionService = Query<sc_missions_v1>(SC_MISSIONS_SERVICE_NAME);
        sc_mission_offer offer{}; offer.size = sizeof(offer);
        CHECK(missionService->offer(reloaded.self, 0, &offer) == SCO_OK);
        uint64_t mission = 0, replay = 0;
        CHECK(missionService->accept(reloaded.self, &offer, 1, &mission) == SCO_OK && mission != 0);
        CHECK(missionService->accept(reloaded.self, &offer, 1, &replay) == SCO_OK && replay == mission);
        for (uint32_t phase = 0; phase < offer.phase_count; ++phase) {
            CHECK(missionService->advance(reloaded.self, mission, phase) == SCO_FAILED);
            for (uint32_t objective = 0; objective < offer.objectives_in_phase; ++objective)
                CHECK(missionService->objective(reloaded.self, mission, phase, objective) == SCO_OK);
            CHECK(missionService->advance(reloaded.self, mission, phase) == SCO_OK);
        }
        sc_mission_status completed{}; completed.size = sizeof(completed);
        CHECK(missionService->status(reloaded.self, mission, &completed) == SCO_OK && completed.state == SC_MISSION_COMPLETED);
        CHECK(mg::SetCapability("game.missions.read", false));
        CHECK(missionService->status(reloaded.self, mission, &completed) == SCO_UNAVAILABLE && completed.state == SC_MISSION_COMPLETED);
        CHECK(mg::SetCapability("game.missions.read", true));
        float oxygen = -1;
        CHECK(Query<sc_actor_stats_v1>(SC_ACTOR_STATS_NAME)->read(
            mg::GetScene().player, SC_ACTOR_STAT_OXYGEN, &oxygen) == SCO_OK && oxygen == 100.0f);
        CHECK(mg::SetCapability("game.actor_stats.read", false));
        CHECK(Query<sc_actor_stats_v1>(SC_ACTOR_STATS_NAME)->read(
            mg::GetScene().player, SC_ACTOR_STAT_OXYGEN, &oxygen) == SCO_UNAVAILABLE && oxygen == 0);
        CHECK(mg::SetCapability("game.actor_stats.read", true));
        sc_inventory_snapshot inventory{}; inventory.size = sizeof(inventory); inventory.count = SC_INVENTORY_MAX_PORTS;
        CHECK(Query<sc_inventory_v1>(SC_INVENTORY_SERVICE_NAME)->snapshot(reloaded.self, &inventory) == SCO_OK);
        CHECK(inventory.count <= SC_INVENTORY_MAX_PORTS && inventory.total_seen >= inventory.count);
        inventory.count = 0;
        inventory.ports[0].entity_id = 0x12345678;
        CHECK(Query<sc_inventory_v1>(SC_INVENTORY_SERVICE_NAME)->snapshot(reloaded.self, &inventory) == SCO_OK);
        CHECK(inventory.count == 0 && inventory.ports[0].entity_id == 0x12345678);
        sc_gamerules_state rules{}; rules.size = sizeof(rules);
        CHECK(Query<sc_gamerules_v1>(SC_GAMERULES_SERVICE_NAME)->read(reloaded.self, &rules) == SCO_OK && rules.valid == 1);
        const auto* gameRules = Query<sc_gamerules_v1>(SC_GAMERULES_SERVICE_NAME);
        uint32_t parameterCount = 99, parameterTotal = 0;
        CHECK(gameRules->list_params(reloaded.self, 0, nullptr, 0, 0, &parameterCount, &parameterTotal) == SCO_OK && parameterCount == 0 && parameterTotal == 17);
        sc_gamerules_param_info parameter{}; parameter.size = sizeof(parameter);
        CHECK(gameRules->list_params(reloaded.self, 3, &parameter, sizeof(parameter), 1, &parameterCount, &parameterTotal) == SCO_OK);
        CHECK(std::strcmp(parameter.name, "terminalsDisabledTime") == 0 && parameter.type == SC_GAMERULES_PARAM_FLOAT);
        CHECK(gameRules->list_params(reloaded.self, 18, &parameter, sizeof(parameter), 1, &parameterCount, &parameterTotal) == SCO_BAD_ARG);
        const auto* live = Query<sc_datacore_live_v1>(SC_DATACORE_LIVE_SERVICE_NAME);
        uint32_t written = 0x12345678, copiedValue = 0; size_t copied = 0;
        CHECK(live->write_pool(reloaded.self, SC_DC_POOL_UINT32, 8, &written, sizeof(written), &copied) == SCO_OK && copied == sizeof(written));
        CHECK(live->read_pool(reloaded.self, SC_DC_POOL_UINT32, 8, &copiedValue, sizeof(copiedValue), &copied) == SCO_OK && copiedValue == written);
        CHECK(live->read_pool(reloaded.self, SC_DC_POOL_UINT32, UINT64_MAX, &copiedValue, sizeof(copiedValue), &copied) == SCO_BAD_ARG);
        CHECK(mg::SetCapability("game.datacore_live.write", false));
        CHECK(live->write_pool(reloaded.self, SC_DC_POOL_UINT32, 8, &written, sizeof(written), &copied) == SCO_UNAVAILABLE);
        CHECK(mg::SetCapability("game.datacore_live.write", true));
        const auto* async = Query<sc_loadout_v1_2>(SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_VERSION_1_2);
        sc_loadout_item item{SC_LOADOUT_SLOT_PRIMARY_OPTIC, 0, "MockOptic_Holo"};
        sc_loadout_request request{sizeof(request), 1, sizeof(item), 0, &item};
        uint64_t job = 0;
        CHECK(async->submit(reloaded.self, &request, &job) == SCO_OK && job != 0);
        sc_loadout_job_report report{}; report.size = sizeof(report);
        CHECK(async->status(reloaded.self, job, &report) == SCO_OK && report.state == SC_LOADOUT_JOB_PENDING);
        sco::app::Tick(80);
        CHECK(async->status(reloaded.self, job, &report) == SCO_OK && report.state == SC_LOADOUT_JOB_COMPLETE && report.report.applied == 1);
        CHECK(mg::EquippedItems()[SC_LOADOUT_SLOT_PRIMARY_OPTIC] == "MockOptic_Holo");
        CHECK(async->cancel(reloaded.self, job) == SCO_OK);
        unsigned respawns = 0, missions = 0;
        CHECK(api->subscribe(reloaded.self, SC_GAME_EVENT_LOADOUT_RESPAWN_COMPLETED,
            [](const char*, const void* data, void* context) {
                const auto* event = static_cast<const sc_game_loadout_respawn_completed*>(data);
                CHECK(event && event->size == sizeof(*event));
                ++*static_cast<unsigned*>(context);
            }, &respawns) == SCO_OK);
        CHECK(api->subscribe(reloaded.self, SC_GAME_EVENT_MISSION_CHANGED,
            [](const char*, const void* data, void* context) {
                const auto* event = static_cast<const sc_game_mission_changed*>(data);
                CHECK(event && event->mission_id == 7 && event->new_state == 1);
                ++*static_cast<unsigned*>(context);
            }, &missions) == SCO_OK);
        CHECK(mg::FireRespawnLoadoutCompleted() && respawns == 1);
        CHECK(mg::FireMissionChanged(7, 1, 0, 0) && missions == 1);
        CHECK(mg::SetCapability("game.events.loadout_respawn_completed", false));
        CHECK(!mg::FireRespawnLoadoutCompleted() && respawns == 1);
        CHECK(mg::SetCapability("game.events.loadout_respawn_completed", true));
        CHECK(async->submit(reloaded.self, &request, &job) == SCO_OK);
        CHECK(mg::SetCapability("game.loadout.async", false));
        sco::app::Tick(81);
        CHECK(async->status(reloaded.self, job, &report) == SCO_OK && report.state == SC_LOADOUT_JOB_FAILED);
        CHECK(async->cancel(reloaded.self, job) == SCO_OK);
        CHECK(mg::SetCapability("game.loadout.async", true));
        CHECK(async->submit(reloaded.self, &request, &job) == SCO_OK);
        sco::app::Tick(81 + SC_LOADOUT_TIMEOUT_MS);
        CHECK(async->status(reloaded.self, job, &report) == SCO_OK && report.state == SC_LOADOUT_JOB_FAILED);
        CHECK(async->cancel(reloaded.self, job) == SCO_OK);
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
