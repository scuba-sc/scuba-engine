#pragma once
// Shared state and helpers of scuba-hostd's mock game services (docs/mock-game.md). Internal to
// src/hostd/mock_game/: one in-memory scene that every synthetic game.* table reads and edits.
//
// Threading: every table function checks sco::OnGameThread() itself; the scene is only touched on
// the game thread (hostd's main thread), so it has no lock. last_error is the one any-thread call.
#include "sco_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scuba::hostd::mock_game {

using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>;   // x, y, z, w

// Who an entity belongs to, and through which service it was made.
enum class Origin : uint8_t {
    Scene,       // part of the synthetic scene (or added by a hostd handler): nobody's
    Npc,         // game.actors spawn_npc
    Entities,    // game.entities spawn
    Prefab,      // game.prefabs spawn (owned through game.entities)
    SpawnAs,     // spawn.entities spawn_as
    SpawnNear,   // spawn.entities spawn_near_player: nobody's
};

struct Seat {
    uint64_t seatId = 0;
    uint64_t occupant = 0;
    uint32_t priority = 0;
    bool usable = true;
    std::string name;
};

struct Entity {
    uint64_t id = 0;
    std::string cls;
    uint64_t zone = 0;
    Vec3 pos{};
    Quat rot{ 0, 0, 0, 1 };
    double radius = 0.5;          // ray-cast sphere, metres; 0: never hit
    bool streamed = true;
    uint32_t streamDelay = 0;     // ticks until it streams in
    bool removing = false;        // leaves the world on the next tick
    const void* owner = nullptr;  // the plugin handle that owns it (Npc, Entities, Prefab, SpawnAs)
    Origin origin = Origin::Scene;
    bool kept = false;            // game.entities keep: nobody's, never despawned by the host
    bool retained = false;        // game.owned retain_build
    bool actor = false;
    float health = 100.0f;
    uint32_t actorState = 0;      // sc_actor_state
    bool vehicle = false;
    bool playerVehicle = false;   // registered player vehicle: anyone may move or power it
    std::vector<Seat> seats;
    bool dashboard = true;
    bool poweredOn = false;
    bool landingArea = false;
    bool lift = false;
    uint64_t liftOfArea = 0;
    uint32_t liftState = 0;       // last sc_hangar_lift_op requested, 0 none
    bool atc = false;
    bool atcPending = false;
};

struct Zone {
    uint64_t id = 0;
    uint64_t parent = 0;   // 0: the world
    std::string name;
    Vec3 origin{};         // this zone's origin in its parent's frame (frames are translation-only)
};

struct Scene {
    std::map<uint64_t, Zone> zones;
    std::map<uint64_t, Entity> entities;
    std::set<std::string> classes;   // spawnable entity classes
    std::set<std::string> items;     // equippable loadout item classes
    std::set<std::string> content;   // prefab object-container paths
    uint64_t nextId = 0x10000;
    uint64_t player = 0;             // the player's entity id
    uint64_t playerActor = 0;        // the player's actor id
    bool spawned = true;
    uint64_t playerShip = 0;         // the ship the player is aboard, 0 none
    Vec3 cameraPos{};                // world frame
    Quat cameraRot{ 0, 0, 0, 1 };
    bool freeCameraFlag = false;
    uint32_t streamTicks = 1;        // ticks a fresh spawn takes to stream in
    uint32_t nowMs = 0;
    uint64_t tick = 0;
    uint64_t session = 0x5c0ba0001;  // opaque session generation
};

Scene& GetScene();
// Replaces the scene with the default synthetic one (docs/mock-game.md, "Default scene").
void ResetScene();

// ---- threads, handles, capabilities ---------------------------------------------------------

bool OnGame();
// A handle sco::host::NewPlugin made that has not been released.
bool LivePlugin(const sco_plugin* self);
void MarkReleased(const void* owner);
bool Ready(const char* capability);
bool FireRespawnLoadoutCompleted();
bool FireMissionChanged(uint64_t mission, uint32_t state, uint32_t phase, uint32_t objective);

// ---- last_error ---------------------------------------------------------------------------------

// One per service. On the game thread a failure is kept per caller (a plugin handle, or nullptr
// for calls without one); on any other thread per thread.
class ErrorBook {
public:
    sco_result Fail(const void* who, sco_result r, const std::string& why);
    sco_result Copy(const void* who, char* out, uint32_t* inoutSize) const;
    void Forget(const void* who);
    void Reset();

private:
    std::map<const void*, std::string> byCaller_;
};

sco_result WrongThread(ErrorBook& book);
// NUL-terminated copy with the size handshake: *inoutSize is out's capacity in, the bytes
// written (NUL included) on SCO_OK, or the bytes needed on SCO_TOO_MANY.
sco_result CopyText(const std::string& text, char* out, uint32_t* inoutSize);

// ---- frames -------------------------------------------------------------------------------------

bool Finite(const double* v, size_t n);
// Normalised copy; false for a missing, non-finite or near-zero (norm < 1e-12) quaternion.
bool NormalizeQuat(const double* in, Quat& out);
Quat QuatMul(const Quat& a, const Quat& b);
// 0 (the world) or a zone of the scene.
bool ZoneKnown(uint64_t zone);
// A non-zero zone of the scene.
bool ZoneStreamed(uint64_t zone);
// A position in zone `from`'s frame to zone `to`'s (either may be 0, the world). Goes through the
// common ancestor, not the world, so distant frames keep their precision.
bool ZoneToZone(uint64_t from, uint64_t to, const Vec3& in, Vec3& out);
// The outermost non-world ancestor (the zone itself when its parent is the world); 0 for 0.
uint64_t RootOf(uint64_t zone);

// ---- entities -----------------------------------------------------------------------------------

bool ClassExists(const char* cls);
// An entity that exists (streamed or not); nullptr when unknown or gone.
Entity* FindEntity(uint64_t id);
// An entity that is streamed in.
Entity* StreamedEntity(uint64_t id);
// Adds e with a fresh id; it streams in after Scene::streamTicks ticks (at once for 0).
uint64_t AddEntity(Entity e);
// Starts a removal: the entity leaves on the next tick and is nobody's from now on.
void BeginRemoval(Entity& e);
// Live (not removing, not kept) entities with this origin that some plugin owns.
size_t CountOwned(Origin origin);
size_t CountOwnedBy(Origin origin, const void* owner);
// Takes actor out of any seat; true when it was seated.
bool Unseat(uint64_t actor);
// The plugin may move it: its own Entities/Prefab/SpawnAs/Npc entity, or a player vehicle.
bool MayMove(const Entity& e, const void* self);

// Watch delivery (entities.cpp): called when an entity streams in or out.
void NotifyStream(uint32_t what, uint64_t id, const std::string& cls);

// One host tick: streaming, removals, then every service's tick.
void TickScene(uint32_t nowMs);

// ---- player and events (events.cpp) ------------------------------------------------------------

// Each fires its game.* bus event when the event's capability is ready; true when fired.
bool SpawnPlayer(uint64_t zone, const Vec3* pos);
bool KillPlayer();
bool KillActor(uint64_t entityId);
bool MovePlayer(uint64_t zone, const Vec3& pos, const Quat* rot);   // fires game.zone.changed
bool FireVehicleSeat(bool boarded, uint64_t ship, int32_t seatIndex);
// The capability that gates a game.* bus event, or nullptr.
const char* EventCapability(const char* event);

}  // namespace scuba::hostd::mock_game
