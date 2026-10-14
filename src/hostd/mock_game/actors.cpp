// game.actors 1.1 (sc_actors.h) over the mock scene.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_actors.h"

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kLocal = "game.actors.local_player";
constexpr const char* kSpawn = "game.actors.spawn_npc";
constexpr const char* kDespawn = "game.actors.despawn";
constexpr const char* kHealth = "game.actors.health";
constexpr const char* kState = "game.actors.state";

ErrorBook g_err;

sco_result Unready(const void* who, const char* cap) {
    return g_err.Fail(who, SCO_UNAVAILABLE, std::string(cap) + " isn't ready");
}

sco_result LocalPlayer(uint64_t* outActor, uint64_t* outEntity) {
    if (outActor) *outActor = 0;
    if (outEntity) *outEntity = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kLocal)) return Unready(nullptr, kLocal);
    if (!outActor && !outEntity) return g_err.Fail(nullptr, SCO_BAD_ARG, "both outputs are NULL");
    const Scene& s = GetScene();
    if (!s.spawned) return g_err.Fail(nullptr, SCO_NOT_FOUND, "you haven't spawned yet");
    if (outActor) *outActor = s.playerActor;
    if (outEntity) *outEntity = s.player;
    return SCO_OK;
}

sco_result SpawnNpc(sco_plugin* self, const char* cls, uint64_t zone, const double pos[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kSpawn)) return Unready(self, kSpawn);
    if (!cls || !pos || !outId) return g_err.Fail(self, SCO_BAD_ARG, "a null class, pos or out_id");
    if (zone == 0) return g_err.Fail(self, SCO_BAD_ARG, "zone 0");
    if (!Finite(pos, 3)) return g_err.Fail(self, SCO_BAD_ARG, "a position that isn't finite");
    if (!ZoneStreamed(zone)) return g_err.Fail(self, SCO_NOT_FOUND, "the zone isn't streamed in");
    if (!ClassExists(cls)) return g_err.Fail(self, SCO_NOT_FOUND, std::string("unknown class ") + cls);
    if (CountOwned(Origin::Npc) >= SC_ACTORS_MAX_NPCS)
        return g_err.Fail(self, SCO_TOO_MANY, "SC_ACTORS_MAX_NPCS NPCs are alive");
    Entity e;
    e.cls = cls;
    e.zone = zone;
    e.pos = { pos[0], pos[1], pos[2] };
    e.radius = 0.6;
    e.actor = true;
    e.owner = self;
    e.origin = Origin::Npc;
    *outId = AddEntity(std::move(e));
    return SCO_OK;
}

sco_result Despawn(sco_plugin* self, uint64_t id) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kDespawn)) return Unready(self, kDespawn);
    Entity* e = FindEntity(id);
    if (!e || e->origin != Origin::Npc || e->owner != self || e->removing)
        return g_err.Fail(self, SCO_NOT_FOUND, "not an NPC you spawned here, or despawned already");
    BeginRemoval(*e);
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

sco_result Health(uint64_t id, float* outCur, float* outMax) {
    if (outCur) *outCur = 0.0f;
    if (outMax) *outMax = 0.0f;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kHealth)) return Unready(nullptr, kHealth);
    if (!outCur && !outMax) return g_err.Fail(nullptr, SCO_BAD_ARG, "both outputs are NULL");
    const Entity* e = StreamedEntity(id);
    if (!e || !e->actor) return g_err.Fail(nullptr, SCO_NOT_FOUND, "not a streamed-in actor");
    if (outCur) *outCur = e->health;
    return SCO_OK;
}

sco_result State(uint64_t id, uint32_t* outState) {
    if (outState) *outState = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kState)) return Unready(nullptr, kState);
    if (!outState) return g_err.Fail(nullptr, SCO_BAD_ARG, "out_state is NULL");
    const Entity* e = StreamedEntity(id);
    if (!e || !e->actor) return g_err.Fail(nullptr, SCO_NOT_FOUND, "not a streamed-in actor");
    *outState = e->actorState;
    return SCO_OK;
}

void Reset() { g_err.Reset(); }
void Release(const void* owner) { g_err.Forget(owner); }

const sc_actors_v1 kTable = { sizeof(sc_actors_v1), 0, LocalPlayer, SpawnNpc, Despawn, LastError, Health, State };

}  // namespace

const ServiceDef& ActorsService() {
    static const ServiceDef def{ SC_ACTORS_NAME, SC_ACTORS_VERSION_1_1, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
