// The mock_game.* bridge methods (docs/mock-game.md): read the scene, toggle capabilities and
// tables, move the player and camera, edit entities, and fire the game.* events.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include "hostd/modules.h"

#include <cmath>

namespace scuba::hostd {

namespace {

namespace mg = mock_game;

void RequirePublished() {
    if (!mg::Published()) throw HandlerError(rpc_error::kUnavailable, "the mock game is off (--no-mock-game)");
}

const Json& Need(const Json& params, const char* key) {
    const Json* v = params.find(key);
    if (!v) throw HandlerError(rpc_error::kInvalidParams, std::string("missing ") + key);
    return *v;
}

uint64_t Id(const Json& params, const char* key, uint64_t fallback, bool required) {
    const Json* v = params.find(key);
    if (!v) {
        if (required) throw HandlerError(rpc_error::kInvalidParams, std::string("missing ") + key);
        return fallback;
    }
    const double d = v->as_number(-1.0);
    if (!v->is_number() || d < 0.0 || d > 9007199254740991.0 || std::floor(d) != d)
        throw HandlerError(rpc_error::kInvalidParams, std::string(key) + " must be a non-negative integer");
    return static_cast<uint64_t>(d);
}

template <size_t N>
bool Array(const Json& params, const char* key, std::array<double, N>& out, bool required) {
    const Json* v = params.find(key);
    if (!v) {
        if (required) throw HandlerError(rpc_error::kInvalidParams, std::string("missing ") + key);
        return false;
    }
    if (!v->is_array() || v->size() != N)
        throw HandlerError(rpc_error::kInvalidParams, std::string(key) + " must be an array of " + std::to_string(N) + " numbers");
    for (size_t i = 0; i < N; ++i) {
        if (!v->at(i).is_number() || !std::isfinite(v->at(i).as_number()))
            throw HandlerError(rpc_error::kInvalidParams, std::string(key) + " must hold finite numbers");
        out[i] = v->at(i).as_number();
    }
    return true;
}

Json Vec(const double* v, size_t n) {
    Json out = Json::MakeArray();
    for (size_t i = 0; i < n; ++i) out.push_back(v[i]);
    return out;
}

Json EntityJson(const mg::Entity& e) {
    Json j = Json::MakeObject();
    j.set("id", e.id);
    j.set("class", e.cls);
    j.set("zone", e.zone);
    j.set("pos", Vec(e.pos.data(), 3));
    j.set("rot", Vec(e.rot.data(), 4));
    j.set("streamed", e.streamed);
    j.set("removing", e.removing);
    j.set("owned", e.owner != nullptr);
    j.set("kept", e.kept);
    j.set("actor", e.actor);
    if (e.actor) j.set("health", static_cast<double>(e.health));
    if (e.vehicle) {
        Json seats = Json::MakeArray();
        for (const auto& s : e.seats) {
            Json seat = Json::MakeObject();
            seat.set("name", s.name);
            seat.set("occupant", s.occupant);
            seat.set("usable", s.usable);
            seats.push_back(std::move(seat));
        }
        j.set("seats", std::move(seats));
        j.set("powered_on", e.poweredOn);
    }
    if (e.lift) j.set("lift_state", e.liftState);
    if (e.atc) j.set("atc_pending", e.atcPending);
    return j;
}

Json StateJson() {
    const mg::Scene& s = mg::GetScene();
    Json out = Json::MakeObject();
    Json player = Json::MakeObject();
    player.set("entity", s.player);
    player.set("actor", s.playerActor);
    player.set("spawned", s.spawned);
    player.set("ship", s.playerShip);
    if (const mg::Entity* p = mg::FindEntity(s.player)) {
        player.set("zone", p->zone);
        player.set("pos", Vec(p->pos.data(), 3));
        player.set("rot", Vec(p->rot.data(), 4));
    }
    out.set("player", std::move(player));
    Json camera = Json::MakeObject();
    camera.set("pos", Vec(s.cameraPos.data(), 3));
    camera.set("rot", Vec(s.cameraRot.data(), 4));
    camera.set("free_camera", s.freeCameraFlag);
    out.set("camera", std::move(camera));
    Json zones = Json::MakeArray();
    for (const auto& [id, z] : s.zones) {
        Json zj = Json::MakeObject();
        zj.set("id", id);
        zj.set("parent", z.parent);
        zj.set("name", z.name);
        zj.set("origin", Vec(z.origin.data(), 3));
        zones.push_back(std::move(zj));
    }
    out.set("zones", std::move(zones));
    Json entities = Json::MakeArray();
    for (const auto& [id, e] : s.entities) entities.push_back(EntityJson(e));
    out.set("entities", std::move(entities));
    out.set("economy_balance", mg::EconomyBalance());
    out.set("tick", s.tick);
    return out;
}

Json CapsJson() {
    Json out = Json::MakeArray();
    for (const auto& c : mg::Capabilities()) {
        Json j = Json::MakeObject();
        j.set("name", c.name);
        j.set("service", c.service);
        j.set("ready", c.ready);
        j.set("game_pack_default", c.gamePackDefault);
        out.push_back(std::move(j));
    }
    return out;
}

Json Fired(bool fired) {
    Json out = Json::MakeObject();
    out.set("fired", fired);
    return out;
}

void Changed(HostContext& host, const char* what) {
    if (!host.broadcaster) return;
    Json data = Json::MakeObject();
    data.set("what", what);
    host.broadcaster->broadcast("mock_game.changed", std::move(data));
}

}  // namespace

void RegisterMockGameHandlers(Registry& registry, HostContext& host) {
    registry.register_handler("mock_game.state", [](const Json&, const CallContext&) {
        RequirePublished();
        return StateJson();
    });

    registry.register_handler("mock_game.caps.list", [](const Json&, const CallContext&) {
        RequirePublished();
        return CapsJson();
    });

    registry.register_handler("mock_game.caps.set", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const Json& name = Need(params, "name");
        const Json& ready = Need(params, "ready");
        if (!name.is_string() || !ready.is_bool())
            throw HandlerError(rpc_error::kInvalidParams, "name must be a string and ready a bool");
        if (!mg::SetCapability(name.as_string(), ready.as_bool()))
            throw HandlerError(rpc_error::kInvalidParams, "not a mock game capability: " + name.as_string());
        Changed(host, "caps");
        return CapsJson();
    });

    registry.register_handler("mock_game.caps.profile", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const Json& name = Need(params, "name");
        if (!name.is_string() || !mg::ApplyProfile(name.as_string()))
            throw HandlerError(rpc_error::kInvalidParams, "profile must be all, game_pack or none");
        Changed(host, "caps");
        return CapsJson();
    });

    registry.register_handler("mock_game.services.list", [](const Json&, const CallContext&) {
        RequirePublished();
        Json out = Json::MakeArray();
        for (const auto& s : mg::Services()) {
            Json j = Json::MakeObject();
            j.set("name", s.name);
            j.set("version", s.version);
            j.set("published", s.published);
            out.push_back(std::move(j));
        }
        return out;
    });

    registry.register_handler("mock_game.services.set", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const Json& name = Need(params, "name");
        const Json& published = Need(params, "published");
        if (!name.is_string() || !published.is_bool())
            throw HandlerError(rpc_error::kInvalidParams, "name must be a string and published a bool");
        if (!mg::SetServicePublished(name.as_string(), published.as_bool()))
            throw HandlerError(rpc_error::kHandlerFailed, "could not change " + name.as_string());
        Changed(host, "services");
        Json out = Json::MakeObject();
        out.set("name", name.as_string());
        out.set("published", published.as_bool());
        return out;
    });

    registry.register_handler("mock_game.player.move", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const mg::Entity* p = mg::FindEntity(mg::GetScene().player);
        const uint64_t zone = Id(params, "zone", p ? p->zone : 0, false);
        if (!mg::ZoneStreamed(zone)) throw HandlerError(rpc_error::kInvalidParams, "unknown zone");
        mg::Vec3 pos{};
        Array(params, "pos", pos, true);
        mg::Quat rot{};
        const bool hasRot = Array(params, "rot", rot, false);
        if (hasRot && !mg::NormalizeQuat(rot.data(), rot)) throw HandlerError(rpc_error::kInvalidParams, "a zero rotation");
        const bool fired = mg::MovePlayer(zone, pos, hasRot ? &rot : nullptr);
        if (const Json* cam = params.find("camera"); cam && cam->as_bool()) {
            const mg::Vec3 eye{ pos[0], pos[1], pos[2] + 1.7 };
            mg::ZoneToZone(zone, 0, eye, mg::GetScene().cameraPos);
            if (hasRot) mg::GetScene().cameraRot = rot;
        }
        Changed(host, "player");
        return Fired(fired);
    });

    registry.register_handler("mock_game.camera.move", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        mg::Vec3 pos{};
        Array(params, "pos", pos, true);
        mg::Quat rot{};
        if (Array(params, "rot", rot, false)) {
            if (!mg::NormalizeQuat(rot.data(), rot)) throw HandlerError(rpc_error::kInvalidParams, "a zero rotation");
            mg::GetScene().cameraRot = rot;
        }
        mg::GetScene().cameraPos = pos;
        Changed(host, "camera");
        Json out = Json::MakeObject();
        out.set("pos", Vec(pos.data(), 3));
        return out;
    });

    registry.register_handler("mock_game.event.player_spawned", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const uint64_t zone = Id(params, "zone", 0, false);
        if (zone != 0 && !mg::ZoneStreamed(zone)) throw HandlerError(rpc_error::kInvalidParams, "unknown zone");
        mg::Vec3 pos{};
        const bool hasPos = Array(params, "pos", pos, false);
        const bool fired = mg::SpawnPlayer(zone, hasPos ? &pos : nullptr);
        Changed(host, "player");
        return Fired(fired);
    });

    registry.register_handler("mock_game.event.player_died", [&host](const Json&, const CallContext&) {
        RequirePublished();
        if (!mg::GetScene().spawned) throw HandlerError(rpc_error::kHandlerFailed, "the player isn't spawned");
        const bool fired = mg::KillPlayer();
        Changed(host, "player");
        return Fired(fired);
    });

    registry.register_handler("mock_game.event.actor_died", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const uint64_t id = Id(params, "entity", 0, true);
        const mg::Entity* e = mg::StreamedEntity(id);
        if (!e || !e->actor) throw HandlerError(rpc_error::kInvalidParams, "not a streamed-in actor");
        const bool fired = mg::KillActor(id);
        Changed(host, "entities");
        return Fired(fired);
    });

    registry.register_handler("mock_game.event.vehicle_seated", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        mg::Scene& s = mg::GetScene();
        const bool boarded = params.find("boarded") ? params.find("boarded")->as_bool(true) : true;
        if (!boarded) {
            uint64_t ship = s.playerShip;
            int32_t index = -1;
            if (const mg::Entity* e = mg::FindEntity(ship))
                for (size_t i = 0; i < e->seats.size(); ++i)
                    if (e->seats[i].occupant == s.player) index = static_cast<int32_t>(i);
            if (!mg::Unseat(s.player)) throw HandlerError(rpc_error::kHandlerFailed, "the player isn't seated");
            Changed(host, "player");
            return Fired(mg::FireVehicleSeat(false, ship, index));
        }
        const uint64_t ship = Id(params, "ship", 0x101, false);
        const uint64_t seat = Id(params, "seat", 0, false);
        mg::Entity* e = mg::StreamedEntity(ship);
        if (!e || !e->vehicle || seat >= e->seats.size()) throw HandlerError(rpc_error::kInvalidParams, "no such ship or seat");
        if (!s.spawned) throw HandlerError(rpc_error::kHandlerFailed, "the player isn't spawned");
        if (e->seats[seat].occupant != 0 && e->seats[seat].occupant != s.player)
            throw HandlerError(rpc_error::kHandlerFailed, "the seat is taken");
        mg::Unseat(s.player);
        e->seats[seat].occupant = s.player;
        s.playerShip = ship;
        Changed(host, "player");
        return Fired(mg::FireVehicleSeat(true, ship, static_cast<int32_t>(seat)));
    });

    registry.register_handler("mock_game.entity.spawn", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const Json& cls = Need(params, "class");
        if (!cls.is_string() || cls.as_string().empty()) throw HandlerError(rpc_error::kInvalidParams, "class must be a string");
        const uint64_t zone = Id(params, "zone", 0, true);
        if (!mg::ZoneStreamed(zone)) throw HandlerError(rpc_error::kInvalidParams, "unknown zone");
        mg::Entity e;
        e.cls = cls.as_string();
        e.zone = zone;
        Array(params, "pos", e.pos, true);
        e.actor = params.find("actor") && params.find("actor")->as_bool();
        e.radius = e.actor ? 0.6 : 0.5;
        mg::GetScene().classes.insert(e.cls);
        const uint64_t id = mg::AddEntity(std::move(e));
        Changed(host, "entities");
        Json out = Json::MakeObject();
        out.set("id", id);
        return out;
    });

    registry.register_handler("mock_game.entity.remove", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        mg::Entity* e = mg::FindEntity(Id(params, "id", 0, true));
        if (!e || e->id == mg::GetScene().player) throw HandlerError(rpc_error::kInvalidParams, "unknown entity, or the player");
        mg::BeginRemoval(*e);
        Changed(host, "entities");
        Json out = Json::MakeObject();
        out.set("removing", true);
        return out;
    });

    registry.register_handler("mock_game.entity.move", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        mg::Entity* e = mg::FindEntity(Id(params, "id", 0, true));
        if (!e) throw HandlerError(rpc_error::kInvalidParams, "unknown entity");
        const uint64_t zone = Id(params, "zone", e->zone, false);
        if (!mg::ZoneStreamed(zone)) throw HandlerError(rpc_error::kInvalidParams, "unknown zone");
        mg::Vec3 pos{};
        Array(params, "pos", pos, true);
        e->zone = zone;
        e->pos = pos;
        mg::NoteEntityChanged();
        Changed(host, "entities");
        return EntityJson(*e);
    });

    registry.register_handler("mock_game.session.add_peer", [&host](const Json&, const CallContext&) {
        RequirePublished();
        if (!mg::AddSessionPeer()) throw HandlerError(rpc_error::kHandlerFailed, "no hosted or connected session with room");
        Changed(host, "session");
        Json out = Json::MakeObject();
        out.set("added", true);
        return out;
    });

    registry.register_handler("mock_game.economy.set_balance", [&host](const Json& params, const CallContext&) {
        RequirePublished();
        const uint64_t balance = Id(params, "balance", 0, true);
        if (balance >= 1000000000000ull) throw HandlerError(rpc_error::kInvalidParams, "balance at or above SC_ECONOMY_BALANCE_LIMIT");
        mg::EconomyBalance() = static_cast<int64_t>(balance);
        Changed(host, "economy");
        Json out = Json::MakeObject();
        out.set("balance", balance);
        return out;
    });

    registry.register_handler("mock_game.reset", [&host](const Json&, const CallContext&) {
        RequirePublished();
        mg::ResetAll();
        Changed(host, "reset");
        return StateJson();
    });
}

}  // namespace scuba::hostd
