#include "hostd/mock_game/common.h"
#include "catalogue.h"

#include "sco/caps.h"
#include "sco/host.h"
#include "sco/runtime.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace scuba::hostd::mock_game {

namespace {

Scene g_scene;
std::mutex g_releasedLock;
std::set<const void*> g_released;

std::string& ThreadSlot(const ErrorBook* book) {
    thread_local std::map<const ErrorBook*, std::string> slots;
    return slots[book];
}

Entity MakeEntity(uint64_t id, const char* cls, uint64_t zone, Vec3 pos, double radius) {
    Entity e;
    e.id = id;
    e.cls = cls;
    e.zone = zone;
    e.pos = pos;
    e.radius = radius;
    return e;
}

void Insert(Entity e) {
    const uint64_t id = e.id;
    g_scene.entities[id] = std::move(e);
}

// Offsets of `zone` and each of its ancestors (the world, 0, last) from `zone`'s own origin.
bool Ancestry(uint64_t zone, std::vector<std::pair<uint64_t, Vec3>>& out) {
    out.clear();
    Vec3 offset{ 0, 0, 0 };
    for (int guard = 0; guard < 64; ++guard) {
        out.emplace_back(zone, offset);
        if (zone == 0) return true;
        const auto it = g_scene.zones.find(zone);
        if (it == g_scene.zones.end()) return false;
        for (size_t i = 0; i < 3; ++i) offset[i] += it->second.origin[i];
        zone = it->second.parent;
    }
    return false;
}

}  // namespace

Scene& GetScene() { return g_scene; }

void ResetScene() {
    static uint64_t nextSession = 0x5c0ba0001;
    g_scene = Scene{};
    g_scene.session = nextSession++;
    Scene& s = g_scene;
    s.zones[0x1] = Zone{ 0x1, 0, "MockSystem", { 0, 0, 0 } };
    s.zones[0x2] = Zone{ 0x2, 0x1, "MockSystem_Planet", { 2.0e10, 0, 0 } };
    s.zones[0x3] = Zone{ 0x3, 0x1, "MockSystem_Station", { -3.0e8, 5.0e7, 0 } };
    s.zones[0x4] = Zone{ 0x4, 0x3, "MockSystem_Station_Hangar_01", { 120, -40, 8 } };
    s.zones[0x5] = Zone{ 0x5, 0, "MockSystemFar", { 1.0e13, 0, 0 } };

    s.classes = { "MOCK_Ship_Starter", "MOCK_Ship_Fighter", "MockNPC_Guard", "MockNPC_Civilian",
                  "MockProp_Crate", "MockProp_Barrel" };
    s.items = { "MockHelmet_Basic", "MockArmor_Torso_Light", "MockArmor_Arms_Light", "MockArmor_Legs_Light",
                "MockUndersuit_Basic", "MockBackpack_Small", "MockVisor_Tinted",
                "MockRifle_Basic", "MockPistol_Basic", "MockOptic_Holo", "MockMultitool_Basic",
                "MockTool_Module_Mining" };
    s.content = { "ObjectContainers/mock/crate_stack.socpak", "ObjectContainers/mock/outpost_small.socpak" };
    for (const auto& item : shop::Catalogue()) {
        if (item.itemClass) s.items.insert(item.itemClass);
        if (item.prefabPath) s.content.insert(item.prefabPath);
    }

    Entity player = MakeEntity(0x100, "MockPlayer_Avatar", 0x4, { 0, 0, 0 }, 0.6);
    player.actor = true;
    Insert(player);
    s.player = 0x100;
    s.playerActor = 0x9100;
    s.inventoryPorts = {
        sc_inventory_port{ sizeof(sc_inventory_port), 1, 0x1234, 0x1001 },
        sc_inventory_port{ sizeof(sc_inventory_port), 1, 0x5678, 0x1002 },
        sc_inventory_port{ sizeof(sc_inventory_port), 1, SC_ITEM_TYPE_CHAR_ARMOR_HELMET, 0x1003 },
    };
    s.inventoryGeneration = 1;
    s.inventoryAvailable = true;
    s.gamerules = sc_gamerules_state{};
    s.gamerules.size = sizeof(sc_gamerules_state);
    s.gamerules.valid = 1;
    s.gamerules.generation = 1;
    s.gamerules.enforce_group_qt = 1;
    s.gamerules.quantum_travel_allowed = 1;
    s.gamerules.quantum_boost_allowed = 0;
    s.gamerules.travel_target_threshold_angle = 12.5f;
    s.gamerules.target_lock_angular_player = 4.0f;
    s.gamerules.target_lock_angular_ai = 6.0f;
    s.gamerules.target_lock_linear_player = 8.0f;
    s.gamerules.target_lock_linear_ai = 10.0f;
    s.gamerules.boost_group_distance_threshold = 2500.0f;
    s.gamerulesReceived = true;

    Entity ship = MakeEntity(0x101, "MOCK_Ship_Starter", 0x4, { 10, 0, 0 }, 12.0);
    ship.vehicle = true;
    ship.playerVehicle = true;
    ship.seats = { Seat{ 0x102, 0, 1000, true, "seat_pilot" }, Seat{ 0x103, 0, 500, true, "seat_copilot" },
                   Seat{ 0x104, 0, 100, false, "turret_remote" } };
    Insert(ship);
    Insert(MakeEntity(0x102, "MockSeat_Pilot", 0x4, { 10, 2, 1 }, 0));
    Insert(MakeEntity(0x103, "MockSeat_Copilot", 0x4, { 11, 2, 1 }, 0));
    Insert(MakeEntity(0x104, "MockTurret_Remote", 0x4, { 10, -3, 2 }, 0));

    Entity area = MakeEntity(0x110, "MockLandingArea_01", 0x4, { 10, 0, -0.5 }, 0);
    area.landingArea = true;
    Insert(area);
    Entity lift = MakeEntity(0x111, "MockLandingArea_01_Lift", 0x4, { 10, 0, -1 }, 0);
    lift.lift = true;
    lift.liftOfArea = 0x110;
    Insert(lift);
    Entity atc = MakeEntity(0x112, "MockATC_Station", 0x3, { 0, 0, 0 }, 0);
    atc.atc = true;
    atc.atcPending = true;
    Insert(atc);
    Entity area2 = MakeEntity(0x113, "MockLandingArea_02", 0x4, { 60, 0, -0.5 }, 0);
    area2.landingArea = true;
    Insert(area2);

    Entity guard = MakeEntity(0x120, "MockNPC_Guard", 0x4, { 5, 5, 0 }, 0.6);
    guard.actor = true;
    Insert(guard);
    Insert(MakeEntity(0x121, "MockProp_Crate", 0x4, { 0, 8, 0.5 }, 0.5));

    Vec3 eye{ 0, 0, 1.7 };
    ZoneToZone(0x4, 0, eye, s.cameraPos);
}

bool OnGame() { return sco::OnGameThread(); }

bool LivePlugin(const sco_plugin* self) {
    if (!self || !sco::host::PluginId(self)) return false;
    std::lock_guard<std::mutex> hold(g_releasedLock);
    return g_released.count(self) == 0;
}

void MarkReleased(const void* owner) {
    std::lock_guard<std::mutex> hold(g_releasedLock);
    g_released.insert(owner);
}

bool Ready(const char* capability) { return sco::caps::Has(capability); }

sco_result ErrorBook::Fail(const void* who, sco_result r, const std::string& why) {
    if (OnGame())
        byCaller_[who] = why;
    else
        ThreadSlot(this) = why;
    return r;
}

sco_result ErrorBook::Copy(const void* who, char* out, uint32_t* inoutSize) const {
    if (!OnGame()) return CopyText(ThreadSlot(this), out, inoutSize);
    const auto it = byCaller_.find(who);
    return CopyText(it == byCaller_.end() ? std::string() : it->second, out, inoutSize);
}

void ErrorBook::Forget(const void* who) { byCaller_.erase(who); }

void ErrorBook::Reset() { byCaller_.clear(); }

sco_result WrongThread(ErrorBook& book) { return book.Fail(nullptr, SCO_WRONG_THREAD, "game thread only"); }

sco_result CopyText(const std::string& text, char* out, uint32_t* inoutSize) {
    if (!inoutSize) return SCO_BAD_ARG;
    if (text.size() >= UINT32_MAX) return SCO_FAILED;
    const uint32_t needed = static_cast<uint32_t>(text.size() + 1);
    if (*inoutSize < needed) {
        *inoutSize = needed;
        return SCO_TOO_MANY;
    }
    if (!out) return SCO_BAD_ARG;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    *inoutSize = needed;
    return SCO_OK;
}

bool Finite(const double* v, size_t n) {
    if (!v) return false;
    for (size_t i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool NormalizeQuat(const double* in, Quat& out) {
    if (!Finite(in, 4)) return false;
    const double norm = std::hypot(std::hypot(in[0], in[1]), std::hypot(in[2], in[3]));
    if (!std::isfinite(norm) || !(norm >= 1e-12)) return false;
    for (size_t i = 0; i < 4; ++i) out[i] = in[i] / norm;
    return true;
}

Quat QuatMul(const Quat& a, const Quat& b) {
    return Quat{ a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                 a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                 a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                 a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
}

bool ZoneKnown(uint64_t zone) { return zone == 0 || g_scene.zones.count(zone) != 0; }

bool ZoneStreamed(uint64_t zone) { return zone != 0 && g_scene.zones.count(zone) != 0; }

bool ZoneToZone(uint64_t from, uint64_t to, const Vec3& in, Vec3& out) {
    std::vector<std::pair<uint64_t, Vec3>> up, down;
    if (!Ancestry(from, up) || !Ancestry(to, down)) return false;
    for (const auto& [zoneTo, offsetTo] : down) {
        for (const auto& [zoneFrom, offsetFrom] : up) {
            if (zoneFrom != zoneTo) continue;
            for (size_t i = 0; i < 3; ++i) out[i] = in[i] + offsetFrom[i] - offsetTo[i];
            return true;
        }
    }
    return false;
}

uint64_t RootOf(uint64_t zone) {
    for (int guard = 0; zone != 0 && guard < 64; ++guard) {
        const auto it = g_scene.zones.find(zone);
        if (it == g_scene.zones.end()) return 0;
        if (it->second.parent == 0) return zone;
        zone = it->second.parent;
    }
    return 0;
}

bool ClassExists(const char* cls) { return cls && g_scene.classes.count(cls) != 0; }

Entity* FindEntity(uint64_t id) {
    if (id == 0) return nullptr;
    const auto it = g_scene.entities.find(id);
    return it == g_scene.entities.end() ? nullptr : &it->second;
}

Entity* StreamedEntity(uint64_t id) {
    Entity* e = FindEntity(id);
    return e && e->streamed ? e : nullptr;
}

uint64_t AddEntity(Entity e) {
    e.id = g_scene.nextId++;
    e.streamDelay = g_scene.streamTicks;
    e.streamed = e.streamDelay == 0;
    const uint64_t id = e.id;
    const std::string cls = e.cls;
    const bool streamed = e.streamed;
    g_scene.entities[id] = std::move(e);
    if (streamed) NotifyStream(1u, id, cls);
    return id;
}

void BeginRemoval(Entity& e) {
    e.removing = true;
    e.owner = nullptr;
    e.retained = false;
}

size_t CountOwned(Origin origin) {
    size_t n = 0;
    for (const auto& [id, e] : g_scene.entities)
        if (e.origin == origin && e.owner && !e.removing && !e.kept) ++n;
    return n;
}

size_t CountOwnedBy(Origin origin, const void* owner) {
    size_t n = 0;
    for (const auto& [id, e] : g_scene.entities)
        if (e.origin == origin && e.owner == owner && !e.removing && !e.kept) ++n;
    return n;
}

bool Unseat(uint64_t actor) {
    if (actor == 0) return false;
    for (auto& [id, e] : g_scene.entities) {
        for (auto& seat : e.seats) {
            if (seat.occupant != actor) continue;
            seat.occupant = 0;
            if (actor == g_scene.player && g_scene.playerShip == id) g_scene.playerShip = 0;
            return true;
        }
    }
    return false;
}

bool MayMove(const Entity& e, const void* self) {
    if (e.playerVehicle) return true;
    if (!self || e.owner != self || e.kept || e.removing) return false;
    return e.origin == Origin::Entities || e.origin == Origin::Prefab || e.origin == Origin::SpawnAs ||
           e.origin == Origin::Npc;
}

void TickScene(uint32_t nowMs) {
    g_scene.nowMs = nowMs;
    ++g_scene.tick;
    std::vector<uint64_t> gone;
    for (auto& [id, e] : g_scene.entities) {
        if (!e.streamed) {
            if (e.streamDelay > 0) --e.streamDelay;
            if (e.streamDelay == 0) {
                e.streamed = true;
                NotifyStream(1u, id, e.cls);
            }
        } else if (e.removing) {
            gone.push_back(id);
        }
    }
    for (uint64_t id : gone) {
        const std::string cls = g_scene.entities[id].cls;
        Unseat(id);
        for (auto& [shipId, ship] : g_scene.entities)
            for (auto& seat : ship.seats)
                if (seat.occupant == id) seat.occupant = 0;
        if (g_scene.playerShip == id) g_scene.playerShip = 0;
        g_scene.entities.erase(id);
        NotifyStream(2u, id, cls);
    }
}

}  // namespace scuba::hostd::mock_game
