#include "sco_api.h"
#include "sc_actors.h"
#include "sc_asop.h"
#include "sc_contracts.h"
#include "sc_creative.h"
#include "sc_cvars_write.h"
#include "sc_economy.h"
#include "sc_entities.h"
#include "sc_game_events.h"
#include "sc_game_spatial.h"
#include "sc_hangar.h"
#include "sc_loadout.h"
#include "sc_mining.h"
#include "sc_missions.h"
#include "sc_owned.h"
#include "sc_prefabs.h"
#include "sc_private_session.h"
#include "sc_quantum.h"
#include "sc_relocation.h"
#include "sc_reputation.h"
#include "sc_spawn.h"
#include "sc_spatial.h"
#include "sc_vehicles.h"
#include "sc_world_control.h"

#include <cstdio>
#include <cstring>
#include <thread>

namespace {

const sco_api* g_api = nullptr;
sco_plugin* g_self = nullptr;
uint32_t g_checks = 0, g_failures = 0, g_streams = 0, g_economyEvents = 0, g_playerEvents = 0;
uint64_t g_npc = 0, g_seatNpc = 0, g_removeNpc = 0, g_entity = 0, g_kept = 0, g_cleanup = 0;
uint64_t g_prefab = 0, g_spawnAs = 0, g_near = 0, g_actorAs = 0;
uint64_t g_watch = 0;
constexpr double kPos[3]{ 50, 50, 4 };
constexpr double kRot[4]{ 0, 0, 0, 1 };

const sc_actors_v1* g_actors = nullptr;
const sc_asop_v1* g_asop = nullptr;
const sc_contracts_v1* g_contracts = nullptr;
const sc_creative_v1* g_creative = nullptr;
const sc_cvars_v1* g_cvars = nullptr;
const sc_cvars_write_v1* g_cvarsWrite = nullptr;
const sc_economy_v1* g_economy = nullptr;
const sc_entities_v1* g_entities = nullptr;
const sc_game_spatial_v1* g_spatial = nullptr;
const sc_hangar_v1* g_hangar = nullptr;
const sc_loadout_v1* g_loadout = nullptr;
const sc_mining_v1* g_mining = nullptr;
const sc_missions_v1* g_missions = nullptr;
const sc_owned_v1* g_owned = nullptr;
const sc_prefabs_v1* g_prefabs = nullptr;
const sc_private_session_v1* g_session = nullptr;
const sc_quantum_v1* g_quantum = nullptr;
const sc_relocation_v1* g_relocation = nullptr;
const sc_reputation_v1* g_reputation = nullptr;
const sc_vehicles_v1* g_vehicles = nullptr;
const sc_world_v1* g_world = nullptr;
const sc_world_control_v1* g_control = nullptr;
const sc_spawn_service_v1* g_spawn = nullptr;
const sc_spatial_v1* g_teleport = nullptr;

void Check(bool ok, const char* expression) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        g_api->log(g_self, SCO_LOG_ERROR, expression);
    }
}

#define PROBE(expression) Check((expression), #expression)

template<class Table>
bool Query(const char* name, uint32_t version, const Table*& out, size_t expectedSize = sizeof(Table)) {
    const void* table = nullptr;
    const bool ok = g_api->query_service(name, version, &table) == SCO_OK && table != nullptr;
    PROBE(ok);
    if (!ok) return false;
    out = static_cast<const Table*>(table);
    PROBE(out->size == expectedSize);
    return true;
}

void Stream(void*, uint32_t what, uint64_t id, const char* cls) {
    PROBE(id != 0 && cls != nullptr);
    PROBE(what == SC_ENTITY_STREAMED_IN || what == SC_ENTITY_STREAMED_OUT);
    ++g_streams;
}

void Event(const char* name, const void* data, void*) {
    PROBE(data != nullptr);
    if (std::strcmp(name, SC_ECONOMY_TRANSACTION_EVENT) == 0) {
        const auto* ev = static_cast<const sc_economy_event*>(data);
        PROBE(ev->size == sizeof(sc_economy_event));
        PROBE(ev->receipt.after == ev->receipt.before + ev->receipt.delta);
        sc_economy_balance balance{};
        balance.size = sizeof(balance);
        PROBE(g_economy->balance(g_self, &balance) == SCO_UNAVAILABLE);
        ++g_economyEvents;
    } else {
        PROBE(*static_cast<const uint32_t*>(data) >= sizeof(uint32_t));
        ++g_playerEvents;
    }
}

template<class Fn>
void ErrorHandshake(Fn fn) {
    uint32_t size = 0;
    PROBE(fn(g_self, nullptr, &size) == SCO_TOO_MANY && size > 1);
    char text[512]{};
    size = sizeof(text);
    PROBE(fn(g_self, text, &size) == SCO_OK && text[0] != '\0');
}

sco_result Seed(const sco_arg*, uint32_t, void*, char* reply, uint32_t capacity) {
    PROBE(g_entities->watch(g_self, 3u, "Mock*", Stream, nullptr, &g_watch) == SCO_OK);
    PROBE(g_actors->spawn_npc(g_self, "MockNPC_Guard", 4, kPos, &g_npc) == SCO_OK);
    PROBE(g_actors->spawn_npc(g_self, "MockNPC_Guard", 4, kPos, &g_seatNpc) == SCO_OK);
    PROBE(g_actors->spawn_npc(g_self, "MockNPC_Civilian", 4, kPos, &g_removeNpc) == SCO_OK);
    PROBE(g_entities->spawn(g_self, "MockProp_Crate", 4, kPos, kRot, &g_entity) == SCO_OK);
    PROBE(g_entities->spawn(g_self, "MockProp_Crate", 4, kPos, kRot, &g_kept) == SCO_OK);
    PROBE(g_entities->spawn(g_self, "MockProp_Barrel", 4, kPos, kRot, &g_cleanup) == SCO_OK);
    PROBE(g_prefabs->spawn(g_self, "ObjectContainers/mock/crate_stack.socpak", 4, kPos, kRot, &g_prefab) == SCO_OK);
    PROBE(g_spawn->spawn_as(g_self, "MOCK_Ship_Fighter", kPos, &g_spawnAs) == nullptr);
    PROBE(g_spawn->spawn_as(g_self, "MockNPC_Guard", kPos, &g_actorAs) == nullptr);
    PROBE(g_spawn->spawn_near_player("MockProp_Crate", kPos, &g_near) == nullptr);
    PROBE(g_entities->alive(g_entity) == 0);
    PROBE(g_mining->request(g_self, 1) == SCO_OK);
    std::snprintf(reply, capacity, "%u checks, %u failures", g_checks, g_failures);
    return g_failures ? SCO_FAILED : SCO_OK;
}

void WrongThread() {
    std::thread worker([] {
        uint64_t id = 42;
        uint32_t flags = 42;
        PROBE(g_actors->local_player(&id, nullptr) == SCO_WRONG_THREAD && id == 0);
        int32_t seconds = 42;
        PROBE(g_asop->claim_timeout(g_self, &seconds) == SCO_WRONG_THREAD && seconds == 42);
        PROBE(g_asop->shard_persisted(g_self, &flags) == SCO_WRONG_THREAD && flags == 42);
        float radius = 42.0f;
        PROBE(g_contracts->stream_radius(g_self, &radius) == SCO_WRONG_THREAD && radius == 42.0f);
        PROBE(g_creative->set_god_mode(g_self, 1) == SCO_WRONG_THREAD);
        PROBE(g_cvars->get_bool(g_self, "mock_hud_enabled", 1, &flags) == SCO_WRONG_THREAD && flags == 42);
        PROBE(g_cvarsWrite->restore_all(g_self) == SCO_WRONG_THREAD);
        PROBE(g_economy->balance(g_self, nullptr) == SCO_WRONG_THREAD);
        PROBE(g_entities->alive(256) == 0);
        PROBE(g_entities->despawn(g_self, g_cleanup) == SCO_WRONG_THREAD);
        PROBE(g_spatial->zone_record(4, nullptr) == 0);
        PROBE(g_hangar->lift_request(g_self, 273, 1) == SCO_WRONG_THREAD);
        PROBE(g_loadout->apply(g_self, nullptr, nullptr) == SCO_WRONG_THREAD);
        PROBE(g_mining->request(g_self, 0) == SCO_WRONG_THREAD);
        PROBE(g_missions->offer(g_self, 0, nullptr) == SCO_WRONG_THREAD);
        PROBE(g_owned->retain_build(g_self, g_entity) == SCO_WRONG_THREAD);
        PROBE(g_owned->receipt(g_self, 1, nullptr) == SCO_WRONG_THREAD);
        PROBE(g_prefabs->spawn(g_self, nullptr, 4, kPos, kRot, &id) == SCO_WRONG_THREAD);
        PROBE(g_session->leave(g_self) == SCO_WRONG_THREAD);
        PROBE(g_quantum->release(g_self) == SCO_WRONG_THREAD);
        PROBE(g_quantum->enable(g_self, 2) == SCO_WRONG_THREAD);
        PROBE(g_relocation->cancel(g_self, 1) == SCO_WRONG_THREAD);
        PROBE(g_reputation->checks(g_self, nullptr) == SCO_WRONG_THREAD);
        PROBE(g_vehicles->player_ship(&id) == SCO_WRONG_THREAD);
        PROBE(g_world->camera(nullptr, nullptr, &id) == SCO_WRONG_THREAD && id == 0);
        PROBE(g_control->camera_acquire(g_self, &id) == SCO_WRONG_THREAD && id == 0);
        PROBE(g_spawn->spawn_near_player("MockProp_Crate", kPos, &id) != nullptr);
        PROBE(g_teleport->player_pose(nullptr, nullptr, &id) == 0);
        char error[128]{};
        uint32_t size = sizeof(error);
        PROBE(g_actors->last_error(g_self, error, &size) == SCO_OK);
        PROBE(std::strcmp(error, "game thread only") == 0);
    });
    worker.join();
}

sco_result Run(const sco_arg*, uint32_t, void*, char* reply, uint32_t capacity) {
    uint64_t actor = 0, player = 0, id = 0, zone = 0;
    uint32_t count = 0, more = 0, flags = 0;
    double pos[3]{}, rot[4]{};
    PROBE(g_actors->local_player(&actor, &player) == SCO_OK && actor != 0 && player == 256);
    float cur = 0, maximum = 1;
    PROBE(g_actors->health(g_npc, &cur, &maximum) == SCO_OK && cur == 100.0f && maximum == 0.0f);
    PROBE(g_actors->state(g_npc, &flags) == SCO_OK && flags == SC_ACTOR_ALIVE);
    PROBE(g_actors->despawn(g_self, 123456) == SCO_NOT_FOUND);
    ErrorHandshake(g_actors->last_error);
    PROBE(g_actors->despawn(g_self, g_npc) == SCO_OK);
    PROBE(g_actors->spawn_npc(nullptr, "MockNPC_Guard", 4, kPos, &id) == SCO_BAD_ARG && id == 0);

    int32_t claimSeconds = 0;
    PROBE(g_asop->claim_timeout(g_self, &claimSeconds) == SCO_OK && claimSeconds == 30);
    PROBE(g_asop->shard_persisted(g_self, &flags) == SCO_OK && flags == 1);
    PROBE(g_asop->claim_timeout(g_self, nullptr) == SCO_BAD_ARG);
    ErrorHandshake(g_asop->last_error);
    PROBE(g_asop->shard_persisted(g_self, nullptr) == SCO_BAD_ARG);
    float radius = 0;
    PROBE(g_contracts->stream_radius(g_self, &radius) == SCO_OK && radius == 5000.0f);
    PROBE(g_contracts->stream_radius(g_self, nullptr) == SCO_BAD_ARG);
    ErrorHandshake(g_contracts->last_error);

    PROBE(g_creative->set_god_mode(g_self, 1) == SCO_OK);
    PROBE(g_creative->set_fly_mode(g_self, 1) == SCO_OK);
    PROBE(g_creative->set_fly_speed(g_self, 10.0f) == SCO_OK);
    PROBE(g_creative->set_infinite_ammo(g_self, 1) == SCO_OK);
    PROBE(g_creative->set_infinite_ship_ammo(g_self, 1) == SCO_OK);
    PROBE(g_creative->state(g_self, &flags) == SCO_OK && flags == 15);
    PROBE(g_creative->set_fly_speed(g_self, 0.0f) == SCO_BAD_ARG);
    ErrorHandshake(g_creative->last_error);

    PROBE(g_cvars->get_bool(g_self, "mock_hud_enabled", SC_CVAR_BOOL, &flags) == SCO_OK && flags == 1);
    PROBE(g_cvarsWrite->set_bool(g_self, "mock_hud_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    PROBE(g_cvars->get_bool(g_self, "mock_hud_enabled", SC_CVAR_BOOL, &flags) == SCO_OK && flags == 0);
    PROBE(g_cvarsWrite->restore_all(g_self) == SCO_OK);
    PROBE(g_cvarsWrite->set_bool(g_self, "mock_hud_enabled", SC_CVAR_BOOL, 0) == SCO_OK);
    PROBE(g_cvars->get_bool(g_self, "missing", SC_CVAR_BOOL, &flags) == SCO_NOT_FOUND);

    sc_economy_balance balance{};
    balance.size = sizeof(balance);
    PROBE(g_economy->balance(g_self, &balance) == SCO_OK && balance.local_balance == 25000);
    sc_economy_request payment{ sizeof(sc_economy_request), 0, 1, balance.local_balance, -100 };
    sc_economy_receipt receipt{};
    receipt.size = sizeof(receipt);
    PROBE(g_economy->apply(g_self, &payment, &receipt) == SCO_OK && receipt.after == 24900);
    PROBE(g_economy->apply(g_self, &payment, &receipt) == SCO_OK && g_economyEvents == 1);
    PROBE(g_economy->receipt(g_self, 1, &receipt) == SCO_OK && receipt.key == 1);
    ++payment.delta;
    PROBE(g_economy->apply(g_self, &payment, &receipt) == SCO_BAD_ARG);

    PROBE(g_entities->alive(g_entity) == 1 && g_streams >= 7);
    char cls[128]{};
    uint32_t size = 0;
    PROBE(g_entities->class_of(g_entity, nullptr, &size) == SCO_TOO_MANY);
    size = sizeof(cls);
    PROBE(g_entities->class_of(g_entity, cls, &size) == SCO_OK && std::strcmp(cls, "MockProp_Crate") == 0);
    PROBE(g_entities->get_transform(g_entity, pos, rot, &zone) == SCO_OK && zone == 4 && pos[0] == 50);
    constexpr double moved[3]{ 60, 50, 5 };
    PROBE(g_entities->set_transform(g_self, g_entity, 4, moved, kRot) == SCO_OK);
    PROBE(g_entities->set_transform(g_self, 289, 4, moved, kRot) == SCO_FAILED);
    ErrorHandshake(g_entities->last_error);
    PROBE(g_entities->query_radius(4, kPos, 100, "MockProp*", nullptr, 0, &count, &more) == SCO_OK && more >= 3);
    PROBE(g_entities->keep(g_self, g_kept) == SCO_OK);
    PROBE(g_entities->set_transform(g_self, g_kept, 4, kPos, kRot) == SCO_FAILED);
    PROBE(g_entities->unwatch(g_self, g_watch) == SCO_OK);

    sc_game_zone record{};
    record.size = sizeof(record);
    PROBE(g_spatial->pose_to_zone(4, 3, kPos, kRot, pos, rot) == 1 && pos[0] == 170);
    PROBE(g_spatial->zone_record(4, &record) == 1 && record.root_id == 1 && record.system_id == 0);
    PROBE(g_spatial->find_zone("MockSystem_Station_Hangar_01", &record) == 1 && record.zone_id == 4);
    sc_game_zone page[2]{};
    page[0].size = page[1].size = sizeof(sc_game_zone);
    uint64_t epoch = 0;
    PROBE(g_spatial->list_zones(0, 0, page, sizeof(sc_game_zone), 2, &count, &more, &epoch) == 1 &&
          count == 2 && more == 1 && epoch != 0);
    PROBE(g_spatial->list_zones(epoch, 2, page, sizeof(sc_game_zone), 2, &count, &more, &epoch) == 1 && more == 0);

    PROBE(g_hangar->lift_of_landing_area(g_self, 272, &id) == SCO_OK && id == 273);
    PROBE(g_hangar->lift_request(g_self, id, SC_HANGAR_LIFT_OPEN) == SCO_OK);
    PROBE(g_hangar->lift_request(g_self, id, SC_HANGAR_LIFT_CLOSE) == SCO_OK);
    PROBE(g_hangar->atc_cancel_request(g_self, 274) == SCO_OK);
    PROBE(g_hangar->lift_request(g_self, id, 9) == SCO_BAD_ARG);
    ErrorHandshake(g_hangar->last_error);

    sc_loadout_item items[2]{};
    items[0].slot = SC_LOADOUT_SLOT_HEAD;
    items[1].slot = SC_LOADOUT_SLOT_TORSO;
    std::snprintf(items[0].item_class, sizeof(items[0].item_class), "%s", "MockHelmet_Basic");
    std::snprintf(items[1].item_class, sizeof(items[1].item_class), "%s", "MockArmor_Torso_Light");
    sc_loadout_request equip{ sizeof(sc_loadout_request), 2, sizeof(sc_loadout_item), 0, items };
    sc_loadout_report report{};
    report.size = sizeof(report);
    PROBE(g_loadout->apply(g_self, &equip, &report) == SCO_OK && report.applied == 2);
    items[1].slot = items[0].slot;
    report.applied = 99;
    PROBE(g_loadout->apply(g_self, &equip, &report) == SCO_BAD_ARG && report.applied == 99);
    sc_loadout_item weapons[3]{};
    weapons[0].slot = SC_LOADOUT_SLOT_PRIMARY;
    weapons[1].slot = SC_LOADOUT_SLOT_PRIMARY_OPTIC;
    weapons[2].slot = 22;
    std::snprintf(weapons[0].item_class, sizeof(weapons[0].item_class), "%s", "MockRifle_Basic");
    std::snprintf(weapons[1].item_class, sizeof(weapons[1].item_class), "%s", "MockOptic_Holo");
    std::snprintf(weapons[2].item_class, sizeof(weapons[2].item_class), "%s", "MockOptic_Holo");
    sc_loadout_request arm{ sizeof(sc_loadout_request), 3, sizeof(sc_loadout_item), 0, weapons };
    PROBE(g_loadout->apply(g_self, &arm, &report) == SCO_FAILED && report.applied == 2 &&
          report.slots[1].status == SC_LOADOUT_APPLIED && report.slots[2].status == SC_LOADOUT_UNSUPPORTED);

    sc_mining_state mining{};
    mining.size = sizeof(mining);
    PROBE(g_mining->state(g_self, &mining) == SCO_OK && mining.active == 1 && mining.cells >= 2);
    PROBE(g_mining->request(g_self, 0) == SCO_OK);

    sc_mission_offer offer{};
    offer.size = sizeof(offer);
    PROBE(g_missions->offer(g_self, 0, &offer) == SCO_OK);
    uint64_t mission = 0;
    PROBE(g_missions->accept(g_self, &offer, 1, &mission) == SCO_OK && mission != 0);
    PROBE(g_missions->accept(g_self, &offer, 1, &id) == SCO_OK && mission == id);
    ++offer.local_reward;
    PROBE(g_missions->accept(g_self, &offer, 1, &id) == SCO_BAD_ARG);
    sc_mission_status status{};
    status.size = sizeof(status);
    PROBE(g_missions->status(g_self, mission, &status) == SCO_OK && status.state == SC_MISSION_ACTIVE);
    PROBE(g_missions->objective(g_self, mission, 0, 0) == SCO_OK);
    PROBE(g_missions->objective(g_self, mission, 0, 0) == SCO_OK);
    PROBE(g_missions->objective(g_self, mission, 0, 1) == SCO_OK);
    PROBE(g_missions->advance(g_self, mission, 0) == SCO_OK);
    PROBE(g_missions->abandon(g_self, mission) == SCO_OK);
    static constexpr char scriptText[] = "sco-mission 1\nphases 2\nobjectives 1\n";
    sc_mission_script script{};
    script.size = sizeof(script);
    std::snprintf(script.relative_path, sizeof(script.relative_path), "%s", "missions/probe.scom");
    script.bytes = scriptText;
    script.byte_count = sizeof(scriptText) - 1;
    PROBE(g_missions->ingest(g_self, &script) == SCO_OK);
    PROBE(g_missions->ingest(g_self, &script) == SCO_OK);
    script.bytes = "bad";
    script.byte_count = 3;
    PROBE(g_missions->ingest(g_self, &script) == SCO_BAD_ARG);

    PROBE(g_owned->retain_build(g_self, g_entity) == SCO_OK);
    PROBE(g_entities->keep(g_self, g_entity) == SCO_FAILED);
    uint64_t ticket = 0;
    PROBE(g_owned->remove_build(g_self, g_entity, &ticket) == SCO_OK);
    sc_removal_receipt removal{};
    removal.size = sizeof(removal);
    PROBE(g_owned->receipt(g_self, ticket, &removal) == SCO_OK && removal.state == SC_REMOVAL_REQUESTED);
    PROBE(g_owned->forget(g_self, ticket) == SCO_OK);
    PROBE(g_owned->remove_npc(g_self, g_removeNpc, &ticket) == SCO_OK);
    PROBE(g_vehicles->seat(g_self, g_seatNpc, 257, 1) == SCO_OK);
    PROBE(g_owned->remove_seat_npc(g_self, 257, 1, g_seatNpc, &ticket) == SCO_OK);
    PROBE(g_entities->despawn(g_self, g_prefab) == SCO_OK);

    sc_private_session_host_request_v1 host{};
    host.size = sizeof(host);
    host.consent = 1;
    host.passphrase = "synthetic-only-passphrase";
    host.passphrase_len = 25;
    host.max_peers = 4;
    host.player_name = "Pilot";
    host.player_name_len = 5;
    host.port = SC_PRIVATE_SESSION_DEFAULT_PORT;
    PROBE(g_session->request_host(g_self, &host) == SCO_OK);
    sc_private_session_state_v1 session{};
    session.size = sizeof(session);
    PROBE(g_session->get_state(&session) == SCO_OK && session.state == SC_PRIVATE_SESSION_HOSTING);
    PROBE(g_session->leave(g_self) == SCO_OK);
    sc_private_session_join_request_v1 join{};
    join.size = sizeof(join);
    join.consent = 1;
    join.address = "127.0.0.1";
    join.address_len = 9;
    join.port = SC_PRIVATE_SESSION_DEFAULT_PORT;
    join.passphrase = host.passphrase;
    join.passphrase_len = host.passphrase_len;
    join.player_name = host.player_name;
    join.player_name_len = host.player_name_len;
    PROBE(g_session->request_join(g_self, &join) == SCO_OK);
    PROBE(g_session->leave(g_self) == SCO_OK);
    host.consent = 0;
    PROBE(g_session->request_host(g_self, &host) == SCO_FAILED);

    sc_reputation_checks reputation{};
    reputation.size = sizeof(reputation);
    PROBE(g_reputation->checks(g_self, &reputation) == SCO_OK && reputation.sites == 4);
    PROBE(g_reputation->checks(g_self, nullptr) == SCO_BAD_ARG);
    ErrorHandshake(g_reputation->last_error);

    PROBE(g_vehicles->player_ship(&id) == SCO_NOT_FOUND);
    sc_vehicle_seat seats[3]{};
    PROBE(g_vehicles->seats(257, seats, 3, &count, &more) == SCO_OK && count == 3 && more == 0);
    PROBE(g_vehicles->seat(g_self, player, 257, 0) == SCO_OK);
    PROBE(g_vehicles->seat_occupant(257, 0, &id) == SCO_OK && id == player);
    PROBE(g_vehicles->power_on(g_self, 257) == SCO_OK);
    PROBE(g_vehicles->eject(g_self, player) == SCO_OK);
    PROBE(g_vehicles->seat(g_self, player, 257, 2) == SCO_FAILED);
    ErrorHandshake(g_vehicles->last_error);
    PROBE(g_vehicles->seat(g_self, player, 257, 0) == SCO_OK);

    PROBE(g_quantum->enable(g_self, 1) == SCO_OK);
    PROBE(g_quantum->hold(g_self, 1000) == SCO_OK);
    sc_quantum_state quantum{};
    quantum.size = sizeof(quantum);
    PROBE(g_quantum->state(g_self, &quantum) == SCO_OK && quantum.held == 1);
    PROBE(g_quantum->release(g_self) == SCO_OK);
    PROBE(g_quantum->hold(g_self, 1001) == SCO_BAD_ARG);

    sc_relocation_request relocate{};
    relocate.size = sizeof(relocate);
    relocate.timeout_ms = 1000;
    relocate.zone_id = 4;
    std::memcpy(relocate.pos, kPos, sizeof(kPos));
    std::memcpy(relocate.rot_xyzw, kRot, sizeof(kRot));
    PROBE(g_relocation->request(g_self, &relocate, &ticket) == SCO_OK);
    sc_relocation_status relocation{};
    relocation.size = sizeof(relocation);
    PROBE(g_relocation->status(g_self, ticket, &relocation) == SCO_OK && relocation.state == SC_RELOCATION_PENDING);
    PROBE(g_relocation->cancel(g_self, ticket) == SCO_OK);
    PROBE(g_relocation->status(g_self, ticket, &relocation) == SCO_OK && relocation.state == SC_RELOCATION_CANCELLED);

    PROBE(g_world->camera(pos, rot, &zone) == SCO_OK && zone == 4 && rot[3] == 1);
    constexpr double rayFrom[3]{ 100, 100, 5 }, rayDir[3]{ 0, 0, -1 };
    sc_world_hit hit{};
    hit.size = sizeof(hit);
    PROBE(g_world->raycast(4, rayFrom, rayDir, 100, &hit) == SCO_OK && hit.distance == 5 && hit.flags == 0);
    PROBE(g_world->raycast(0, rayFrom, rayDir, 100, &hit) == SCO_BAD_ARG && hit.distance == 0);
    ErrorHandshake(g_world->last_error);
    PROBE(g_world->free_camera_flag(&flags) == SCO_OK && flags == 0);
    PROBE(g_control->raycast_filtered(g_self, 4, rayFrom, rayDir, 100, &g_cleanup, 1, &hit) == SCO_OK);
    uint64_t foreign = 256;
    PROBE(g_control->raycast_filtered(g_self, 4, rayFrom, rayDir, 100, &foreign, 1, &hit) == SCO_FAILED && hit.distance == 0);
    uint64_t lease = 0;
    PROBE(g_control->camera_acquire(g_self, &lease) == SCO_OK && lease != 0);
    sc_world_camera_step step{};
    step.size = sizeof(step);
    step.translation[0] = 1;
    step.rotation_xyzw[3] = 1;
    PROBE(g_control->camera_step(g_self, lease, &step) == SCO_OK);
    PROBE(g_world->free_camera_flag(&flags) == SCO_OK && flags == 1);
    PROBE(g_control->camera_release(g_self, lease) == SCO_OK);
    PROBE(g_control->camera_acquire(g_self, &lease) == SCO_OK);
    PROBE(g_control->camera_step(g_self, lease, &step) == SCO_OK);

    PROBE(g_spawn->class_exists("MOCK_Ship_Fighter") == 1);
    PROBE(g_spawn->local_player_id() == player);
    PROBE(g_spawn->player_ship_id() == 257);
    PROBE(g_spawn->entity_alive(g_spawnAs) == 1);
    PROBE(g_spawn->set_entity_transform(g_self, g_spawnAs, 4, kPos, kRot) == 1);
    PROBE(g_spawn->set_entity_transform(g_self, g_near, 4, kPos, kRot) == 0);
    PROBE(g_vehicles->seat(g_self, g_actorAs, 257, 1) == SCO_OK);
    PROBE(g_vehicles->eject(g_self, g_actorAs) == SCO_OK);
    PROBE(g_vehicles->power_on(g_self, g_spawnAs) == SCO_OK);

    PROBE(g_teleport->player_pose(pos, rot, &zone) == 1 && zone == 4);
    PROBE(g_teleport->zone_of_entity(g_cleanup, &zone) == 1 && zone == 4);
    double world[3]{};
    PROBE(g_teleport->local_to_world(4, kPos, world) == 1);
    PROBE(g_teleport->world_to_local(4, world, pos) == 1 && pos[0] == kPos[0]);
    PROBE(g_teleport->zone_to_zone(4, 3, kPos, pos) == 1 && pos[0] == 170);
    PROBE(g_teleport->zone_name(4, cls, sizeof(cls)) == 1 && std::strcmp(cls, "MockSystem_Station_Hangar_01") == 0);
    WrongThread();
    std::snprintf(reply, capacity, "%u checks, %u failures", g_checks, g_failures);
    return g_failures ? SCO_FAILED : SCO_OK;
}

sco_result Has(const sco_arg* args, uint32_t n, void*, char* reply, uint32_t capacity) {
    if (n != 1 || args[0].type != SCO_ARG_STRING) return SCO_BAD_ARG;
    std::snprintf(reply, capacity, "%d", g_api->has(args[0].v.s));
    return SCO_OK;
}

// The 2.3.0 additions under whatever capability state the test set: "<claim> <persisted> <radius>
// <weapon slot status>" (sco_result values, then a sc_loadout_status).
sco_result Gated(const sco_arg*, uint32_t, void*, char* reply, uint32_t capacity) {
    int32_t seconds = 0;
    uint32_t persisted = 0;
    float radius = 0;
    sc_loadout_item item{};
    item.slot = SC_LOADOUT_SLOT_SIDEARM;
    std::snprintf(item.item_class, sizeof(item.item_class), "%s", "MockPistol_Basic");
    sc_loadout_request request{ sizeof(sc_loadout_request), 1, sizeof(sc_loadout_item), 0, &item };
    sc_loadout_report report{};
    report.size = sizeof(report);
    const sco_result claim = g_asop->claim_timeout(g_self, &seconds);
    const sco_result shard = g_asop->shard_persisted(g_self, &persisted);
    const sco_result stream = g_contracts->stream_radius(g_self, &radius);
    (void)g_loadout->apply(g_self, &request, &report);
    std::snprintf(reply, capacity, "%d %d %d %d", static_cast<int>(claim), static_cast<int>(shard),
                  static_cast<int>(stream), static_cast<int>(report.slots[0].status));
    return SCO_OK;
}

sco_result Counts(const sco_arg*, uint32_t, void*, char* reply, uint32_t capacity) {
    std::snprintf(reply, capacity, "%u", g_playerEvents);
    return SCO_OK;
}

bool Command(const char* name, sco_command_fn fn, const sco_arg_def* args = nullptr, uint32_t n = 0) {
    sco_command cmd{};
    cmd.size = sizeof(cmd);
    cmd.name = name;
    cmd.title = name;
    cmd.fn = fn;
    cmd.args = args;
    cmd.nargs = n;
    cmd.arg_def_size = sizeof(sco_arg_def);
    return g_api->register_command(g_self, &cmd) == SCO_OK;
}

}

extern "C" SCO_EXPORT const sco_plugin_info* sco_plugin_query() {
    static const sco_plugin_info info{ sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR,
                                       "mock_game_probe", "1.0.0", "the maintainer" };
    return &info;
}

extern "C" SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    g_checks = g_failures = g_streams = g_economyEvents = g_playerEvents = 0;
    bool ok = true;
#define QUERY(table, name, version) ok = Query(name, version, table) && ok
    QUERY(g_actors, SC_ACTORS_NAME, SC_ACTORS_VERSION_1_1);
    QUERY(g_asop, SC_ASOP_SERVICE_NAME, SC_ASOP_SERVICE_VERSION);
    QUERY(g_contracts, SC_CONTRACTS_SERVICE_NAME, SC_CONTRACTS_SERVICE_VERSION);
    QUERY(g_creative, SC_CREATIVE_SERVICE_NAME, SC_CREATIVE_SERVICE_VERSION);
    QUERY(g_cvars, SC_CVARS_NAME, SC_CVARS_VERSION);
    QUERY(g_cvarsWrite, SC_CVARS_WRITE_NAME, SC_CVARS_WRITE_VERSION);
    QUERY(g_economy, SC_ECONOMY_SERVICE_NAME, SC_ECONOMY_SERVICE_VERSION);
    QUERY(g_entities, SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_1);
    QUERY(g_spatial, SC_GAME_SPATIAL_NAME, SC_GAME_SPATIAL_VERSION);
    QUERY(g_hangar, SC_HANGAR_SERVICE_NAME, SC_HANGAR_SERVICE_VERSION);
    // The provider returns its extended table even for a prefix-version query.
    ok = Query(SC_LOADOUT_SERVICE_NAME, SC_LOADOUT_VERSION_1_1, g_loadout, sizeof(sc_loadout_v1_2)) && ok;
    QUERY(g_mining, SC_MINING_SERVICE_NAME, SC_MINING_SERVICE_VERSION);
    QUERY(g_missions, SC_MISSIONS_SERVICE_NAME, SC_MISSIONS_SERVICE_VERSION);
    QUERY(g_owned, SC_OWNED_NAME, SC_OWNED_VERSION_1_0);
    QUERY(g_prefabs, SC_PREFABS_NAME, SC_PREFABS_VERSION_1_0);
    QUERY(g_session, SC_PRIVATE_SESSION_NAME, SC_PRIVATE_SESSION_VERSION);
    QUERY(g_quantum, SC_QUANTUM_SERVICE_NAME, SC_QUANTUM_SERVICE_VERSION);
    ok = Query(SC_RELOCATION_NAME, SC_RELOCATION_VERSION, g_relocation, sizeof(sc_relocation_v1_1)) && ok;
    QUERY(g_reputation, SC_REPUTATION_SERVICE_NAME, SC_REPUTATION_SERVICE_VERSION);
    QUERY(g_vehicles, SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION);
    QUERY(g_world, SC_WORLD_NAME, SC_WORLD_VERSION_1_1);
    QUERY(g_control, SC_WORLD_CONTROL_NAME, SC_WORLD_CONTROL_VERSION_1_0);
    QUERY(g_spawn, SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION);
    QUERY(g_teleport, SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION);
#undef QUERY
    if (!ok) return SCO_FAILED;
    PROBE(api->has("game.private_session") == 1);
    PROBE(api->subscribe(self, SC_ECONOMY_TRANSACTION_EVENT, Event, nullptr) == SCO_OK);
    for (const char* name : { SC_GAME_EVENT_PLAYER_SPAWNED, SC_GAME_EVENT_PLAYER_DIED,
                              SC_GAME_EVENT_ACTOR_DIED, SC_GAME_EVENT_ZONE_CHANGED,
                              SC_GAME_EVENT_VEHICLE_BOARDED, SC_GAME_EVENT_VEHICLE_EXITED })
        PROBE(api->subscribe(self, name, Event, nullptr) == SCO_OK);
    static const sco_arg_def hasArg{ "name", SCO_ARG_STRING, 0, "capability" };
    if (!Command("mock_game_probe.seed", Seed) || !Command("mock_game_probe.run", Run) ||
        !Command("mock_game_probe.has", Has, &hasArg, 1) || !Command("mock_game_probe.events", Counts) ||
        !Command("mock_game_probe.gated", Gated))
        return SCO_FAILED;
    return g_failures ? SCO_FAILED : SCO_OK;
}

extern "C" SCO_EXPORT void sco_plugin_unload() {}
