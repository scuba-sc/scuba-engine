// game.entities 1.1 (sc_entities.h): any entity's class and transform, owned spawns, type-filtered
// stream watches delivered a tick later as tasks of the watching plugin, and radius queries.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_entities.h"
#include "sco/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kTransform = "game.entities.transform";
constexpr const char* kSpawn = "game.entities.spawn";
constexpr const char* kClassOf = "game.entities.class_of";
constexpr const char* kQuery = "game.entities.query_radius";
constexpr const char* kWatch = "game.entities.watch";

struct Watch {
    const void* owner = nullptr;
    uint32_t what = 0;
    std::string type;
    sc_entity_watch_fn fn = nullptr;
    void* ctx = nullptr;
};

struct Delivery {
    uint64_t watchId = 0;
    const void* owner = nullptr;
    uint32_t what = 0;
    uint64_t entity = 0;
    std::string cls;
    bool dropped = false;
};

ErrorBook g_err;
std::map<uint64_t, Watch> g_watches;
uint64_t g_nextWatch = 1;
std::list<Delivery> g_deliveries;

sco_result Unready(const void* who, const char* cap) {
    return g_err.Fail(who, SCO_UNAVAILABLE, std::string(cap) + " isn't ready");
}

// A class name or a "PREFIX*" pattern: printable ASCII or '?', '*' only last, not a lone '*'.
bool ValidType(const char* type) {
    if (!type || !*type) return false;
    const size_t n = std::strlen(type);
    if (n > SC_ENTITIES_MAX_TYPE_LEN) return false;
    if (n == 1 && type[0] == '*') return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(type[i]);
        if (c == '*' && i != n - 1) return false;
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

bool Matches(const std::string& type, const std::string& cls) {
    if (!type.empty() && type.back() == '*') return cls.compare(0, type.size() - 1, type, 0, type.size() - 1) == 0;
    return type == cls;
}

void DeliverTask(void* ctx) {
    const auto* queued = static_cast<const Delivery*>(ctx);
    Delivery copy = *queued;
    g_deliveries.remove_if([queued](const Delivery& d) { return &d == queued; });
    if (copy.dropped || !Ready(kWatch)) return;
    const auto it = g_watches.find(copy.watchId);
    if (it == g_watches.end() || it->second.owner != copy.owner) return;
    const Watch watch = it->second;
    if (watch.fn) watch.fn(watch.ctx, copy.what, copy.entity, copy.cls.c_str());
}

sco_result OwnedOut(const void* self, const char* why) { return g_err.Fail(self, SCO_BAD_ARG, why); }

int Alive(uint64_t id) {
    if (!OnGame()) return 0;
    return StreamedEntity(id) ? 1 : 0;
}

sco_result ClassOf(uint64_t id, char* out, uint32_t* inoutSize) {
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kClassOf)) return Unready(nullptr, kClassOf);
    if (!inoutSize) return g_err.Fail(nullptr, SCO_BAD_ARG, "inout_size is NULL");
    const Entity* e = StreamedEntity(id);
    if (!e) return g_err.Fail(nullptr, SCO_NOT_FOUND, "the entity isn't streamed in");
    const sco_result r = CopyText(e->cls, out, inoutSize);
    if (r == SCO_BAD_ARG) return g_err.Fail(nullptr, r, "out is NULL");
    return r;
}

sco_result GetTransform(uint64_t id, double outPos[3], double outRot[4], uint64_t* outZone) {
    if (outPos) std::fill(outPos, outPos + 3, 0.0);
    if (outRot) std::fill(outRot, outRot + 4, 0.0);
    if (outZone) *outZone = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kTransform)) return Unready(nullptr, kTransform);
    if (!outPos && !outRot && !outZone) return g_err.Fail(nullptr, SCO_BAD_ARG, "every output is NULL");
    const Entity* e = StreamedEntity(id);
    if (!e || e->zone == 0) return g_err.Fail(nullptr, SCO_NOT_FOUND, "the entity isn't streamed in, or in no zone");
    if (outPos) std::copy(e->pos.begin(), e->pos.end(), outPos);
    if (outRot) std::copy(e->rot.begin(), e->rot.end(), outRot);
    if (outZone) *outZone = e->zone;
    return SCO_OK;
}

sco_result SetTransform(sco_plugin* self, uint64_t id, uint64_t zone, const double pos[3], const double rot[4]) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return OwnedOut(self, "unknown or released plugin handle");
    if (!Ready(kTransform)) return Unready(self, kTransform);
    Quat q;
    if (id == 0 || !pos || !rot) return OwnedOut(self, "id 0, or a null pos or rot");
    if (!Finite(pos, 3)) return OwnedOut(self, "a position that isn't finite");
    if (!NormalizeQuat(rot, q)) return OwnedOut(self, "a zero or non-finite rotation");
    Entity* e = StreamedEntity(id);
    if (!e) return g_err.Fail(self, SCO_NOT_FOUND, "not streamed in yet: wait for alive");
    if (!ZoneKnown(zone)) return g_err.Fail(self, SCO_NOT_FOUND, "the zone isn't streamed in");
    if (!MayMove(*e, self)) return g_err.Fail(self, SCO_FAILED, "not yours to move");
    Vec3 local;
    if (!ZoneToZone(zone, e->zone, Vec3{ pos[0], pos[1], pos[2] }, local))
        return g_err.Fail(self, SCO_FAILED, "the host couldn't convert the pose");
    e->pos = local;
    e->rot = q;
    return SCO_OK;
}

sco_result Spawn(sco_plugin* self, const char* cls, uint64_t zone, const double pos[3], const double rot[4],
                 uint64_t* outId) {
    if (outId) *outId = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return OwnedOut(self, "unknown or released plugin handle");
    if (!Ready(kSpawn)) return Unready(self, kSpawn);
    Quat q;
    if (!cls || !pos || !rot || !outId) return OwnedOut(self, "a null class, pos, rot or out_id");
    if (zone == 0) return OwnedOut(self, "zone 0");
    if (!Finite(pos, 3)) return OwnedOut(self, "a position that isn't finite");
    if (!NormalizeQuat(rot, q)) return OwnedOut(self, "a zero or non-finite rotation");
    if (!ZoneStreamed(zone)) return g_err.Fail(self, SCO_NOT_FOUND, "the zone isn't streamed in");
    if (!ClassExists(cls)) return g_err.Fail(self, SCO_NOT_FOUND, std::string("unknown class ") + cls);
    if (CountOwned(Origin::Entities) + CountOwned(Origin::Prefab) >= SC_ENTITIES_MAX_OWNED)
        return g_err.Fail(self, SCO_TOO_MANY, "SC_ENTITIES_MAX_OWNED entities are alive");
    Entity e;
    e.cls = cls;
    e.zone = zone;
    e.pos = { pos[0], pos[1], pos[2] };
    e.rot = q;
    e.owner = self;
    e.origin = Origin::Entities;
    e.radius = std::strncmp(cls, "MOCK_Ship", 9) == 0 ? 12.0 : 0.5;
    if (std::strncmp(cls, "MOCK_Ship", 9) == 0) {
        e.vehicle = true;
        e.seats = { Seat{ 0, 0, 1000, true, "seat_pilot" } };
    }
    if (std::strncmp(cls, "MockNPC", 7) == 0) e.actor = true;
    *outId = AddEntity(std::move(e));
    return SCO_OK;
}

sco_result Despawn(sco_plugin* self, uint64_t id) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return OwnedOut(self, "unknown or released plugin handle");
    if (!Ready(kSpawn)) return Unready(self, kSpawn);
    Entity* e = FindEntity(id);
    if (!e || (e->origin != Origin::Entities && e->origin != Origin::Prefab) || e->owner != self || e->kept ||
        e->removing)
        return g_err.Fail(self, SCO_NOT_FOUND, "not an entity you spawned here, or despawned already");
    BeginRemoval(*e);
    return SCO_OK;
}

sco_result WatchFn(sco_plugin* self, uint32_t what, const char* type, sc_entity_watch_fn fn, void* ctx,
                   uint64_t* outWatch) {
    if (outWatch) *outWatch = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self) || !fn || !outWatch) return OwnedOut(self, "a null self, fn or out_watch_id");
    if (!Ready(kWatch)) return Unready(self, kWatch);
    if ((what & (SC_ENTITY_STREAMED_IN | SC_ENTITY_STREAMED_OUT)) == 0) return OwnedOut(self, "`what` has no known bit");
    if (!ValidType(type)) return OwnedOut(self, "not a class name or a PREFIX* pattern");
    if (g_watches.size() >= SC_ENTITIES_MAX_WATCHES)
        return g_err.Fail(self, SCO_TOO_MANY, "SC_ENTITIES_MAX_WATCHES watches exist");
    const uint64_t id = g_nextWatch++;
    g_watches[id] = Watch{ self, what & (SC_ENTITY_STREAMED_IN | SC_ENTITY_STREAMED_OUT), type, fn, ctx };
    *outWatch = id;
    return SCO_OK;
}

sco_result Unwatch(sco_plugin* self, uint64_t watchId) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return OwnedOut(self, "unknown or released plugin handle");
    const auto it = g_watches.find(watchId);
    if (it == g_watches.end() || it->second.owner != self)
        return g_err.Fail(self, SCO_NOT_FOUND, "not your watch, or ended already");
    g_watches.erase(it);
    return SCO_OK;
}

sco_result QueryRadius(uint64_t zone, const double pos[3], double radius, const char* filter, uint64_t* outIds,
                       uint32_t max, uint32_t* outCount, uint32_t* outMore) {
    if (outCount) *outCount = 0;
    if (outMore) *outMore = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kQuery)) return Unready(nullptr, kQuery);
    if (!pos || !outCount || !outMore || (!outIds && max > 0) || zone == 0)
        return g_err.Fail(nullptr, SCO_BAD_ARG, "a null pos, out_count or out_more, a null out_ids with max > 0, or zone 0");
    if (!Finite(pos, 3) || !std::isfinite(radius) || !(radius > 0.0))
        return g_err.Fail(nullptr, SCO_BAD_ARG, "a position or radius that isn't finite and positive");
    const bool anyClass = !filter || !*filter;
    if (!anyClass && !ValidType(filter)) return g_err.Fail(nullptr, SCO_BAD_ARG, "not a class name or prefix");
    if (!ZoneStreamed(zone)) return g_err.Fail(nullptr, SCO_NOT_FOUND, "the zone isn't streamed in");
    uint32_t count = 0, more = 0;
    const Vec3 center{ pos[0], pos[1], pos[2] };
    for (const auto& [id, e] : GetScene().entities) {
        if (!e.streamed || e.removing) continue;
        if (!anyClass && !Matches(filter, e.cls)) continue;
        Vec3 there;
        if (!ZoneToZone(e.zone, zone, e.pos, there)) continue;
        const double dx = there[0] - center[0], dy = there[1] - center[1], dz = there[2] - center[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > radius) continue;
        if (count < max)
            outIds[count++] = id;
        else
            ++more;
    }
    *outCount = count;
    *outMore = more;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

sco_result Keep(sco_plugin* self, uint64_t id) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self) || id == 0) return OwnedOut(self, "a null or released self, or id 0");
    if (!Ready(kSpawn)) return Unready(self, kSpawn);
    Entity* e = FindEntity(id);
    if (!e || (e->origin != Origin::Entities && e->origin != Origin::Prefab) || e->owner != self || e->kept ||
        e->removing)
        return g_err.Fail(self, SCO_NOT_FOUND, "not an entity you spawned here and still own");
    if (e->retained) return g_err.Fail(self, SCO_FAILED, "a game.owned retained placement: use remove_build");
    e->kept = true;
    e->owner = nullptr;
    return SCO_OK;
}

void Reset() {
    g_err.Reset();
    g_watches.clear();
    for (auto& d : g_deliveries) d.dropped = true;
}

void Release(const void* owner) {
    g_err.Forget(owner);
    for (auto it = g_watches.begin(); it != g_watches.end();)
        it = it->second.owner == owner ? g_watches.erase(it) : std::next(it);
    g_deliveries.remove_if([owner](const Delivery& d) { return d.owner == owner; });
    for (auto& [id, e] : GetScene().entities) {
        if (e.owner != owner) continue;
        if (e.origin == Origin::Entities || e.origin == Origin::Prefab || e.origin == Origin::Npc)
            BeginRemoval(e);
        else
            e.owner = nullptr;
    }
}

const sc_entities_v1 kTable = { sizeof(sc_entities_v1), 0, Alive, ClassOf, GetTransform, SetTransform, Spawn,
                                Despawn, WatchFn, Unwatch, QueryRadius, LastError, Keep };

}  // namespace

void NotifyStream(uint32_t what, uint64_t id, const std::string& cls) {
    if (!Ready(kWatch)) return;
    for (const auto& [watchId, w] : g_watches) {
        if ((w.what & what) == 0 || !Matches(w.type, cls)) continue;
        g_deliveries.push_back(Delivery{ watchId, w.owner, what, id, cls, false });
        Delivery* d = &g_deliveries.back();
        if (sco::Post(DeliverTask, d, w.owner) != sco::Result::Ok) g_deliveries.pop_back();
    }
}

const ServiceDef& EntitiesService() {
    static const ServiceDef def{ SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_1, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
