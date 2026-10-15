// game.world 1.1 (sc_world.h): ray casts against the scene (entity spheres and each zone's z = 0
// ground plane) and the camera pose.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace scuba::hostd::mock_game {

namespace {

struct ZoneEntityIndex {
    uint64_t version = 0;
    size_t size = 0;
    std::unordered_map<uint64_t, std::vector<const Entity*>> byZone;

    void UpdateUnlocked(const Scene& s) {
        if (version == s.entityVersion && size == s.entities.size()) return;
        byZone.clear();
        for (const auto& [id, e] : s.entities) {
            byZone[e.zone].push_back(&e);
        }
        version = s.entityVersion;
        size = s.entities.size();
    }
};

ZoneEntityIndex g_zoneIndex;
std::mutex g_zoneIndexMutex;

constexpr const char* kRay = "game.world.raycast";
constexpr const char* kCamera = "game.world.camera";
constexpr const char* kFreeFlag = "game.world.free_camera_flag";
ErrorBook g_err;

void ClearHit(sc_world_hit* hit) {
    const uint32_t size = hit->size;
    std::memset(hit, 0, sizeof(sc_world_hit));
    hit->size = size;
}

sco_result Raycast(uint64_t zone, const double from[3], const double dir[3], double maxDist, sc_world_hit* hit) {
    if (hit && hit->size >= sizeof(sc_world_hit)) ClearHit(hit);
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kRay)) {
        if (hit && hit->size >= sizeof(sc_world_hit)) ClearHit(hit);
        return g_err.Fail(nullptr, SCO_UNAVAILABLE, "game.world.raycast isn't ready");
    }
    std::string why;
    const sco_result r = CastRay(zone, from, dir, maxDist, {}, hit, why);
    return r == SCO_OK ? SCO_OK : g_err.Fail(nullptr, r, why);
}

sco_result Camera(double outPos[3], double outRot[4], uint64_t* outZone) {
    if (outPos) std::fill(outPos, outPos + 3, 0.0);
    if (outRot) std::fill(outRot, outRot + 4, 0.0);
    if (outZone) *outZone = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!Ready(kCamera)) return g_err.Fail(nullptr, SCO_UNAVAILABLE, "game.world.camera isn't ready");
    if (!outPos && !outRot && !outZone) return g_err.Fail(nullptr, SCO_BAD_ARG, "every output is NULL");
    const Scene& s = GetScene();
    const Entity* player = FindEntity(s.player);
    if (!s.spawned || !player) return g_err.Fail(nullptr, SCO_NOT_FOUND, "you haven't spawned yet");
    if (outPos) std::copy(s.cameraPos.begin(), s.cameraPos.end(), outPos);
    if (outRot) std::copy(s.cameraRot.begin(), s.cameraRot.end(), outRot);
    if (outZone) *outZone = player->zone;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(nullptr, out, inoutSize);
}

sco_result FreeCameraFlag(uint32_t* outSet) {
    if (outSet) *outSet = 0;
    if (!OnGame()) return WrongThread(g_err);
    if (!outSet) return g_err.Fail(nullptr, SCO_BAD_ARG, "out_set is NULL");
    if (!Ready(kFreeFlag)) return g_err.Fail(nullptr, SCO_UNAVAILABLE, "game.world.free_camera_flag isn't ready");
    *outSet = GetScene().freeCameraFlag ? 1u : 0u;
    return SCO_OK;
}

void Reset() { g_err.Reset(); }
void Release(const void*) {}

const sc_world_v1 kTable = { sizeof(sc_world_v1), 0, Raycast, Camera, LastError, FreeCameraFlag };

}  // namespace

sco_result CastRay(uint64_t zone, const double from[3], const double dir[3], double maxDist,
                   const std::set<uint64_t>& skip, sc_world_hit* hit, std::string& why) {
    if (!hit || hit->size < sizeof(sc_world_hit)) {
        why = "a NULL or too-small out_hit";
        return SCO_BAD_ARG;
    }
    ClearHit(hit);
    if (!from || !dir || zone == 0 || !Finite(from, 3) || !Finite(dir, 3) || !std::isfinite(maxDist) || !(maxDist > 0.0)) {
        why = "a NULL pointer, zone 0, a non-finite value or a bad max_dist";
        return SCO_BAD_ARG;
    }
    const double len = std::hypot(dir[0], dir[1], dir[2]);
    if (!std::isfinite(len) || !(len > 0.0)) {
        why = "a zero direction";
        return SCO_BAD_ARG;
    }
    if (!ZoneStreamed(zone) || !GetScene().spawned) {
        why = "the zone isn't streamed in, or you haven't spawned";
        return SCO_NOT_FOUND;
    }
    const double dist = std::min(maxDist, static_cast<double>(SC_WORLD_RAY_MAX_DISTANCE));
    const Vec3 o{ from[0], from[1], from[2] };
    const Vec3 d{ dir[0] / len, dir[1] / len, dir[2] / len };
    double best = dist + 1.0;
    uint64_t bestEntity = 0;
    std::set<uint64_t> castZones;
    uint64_t ancestor = zone;
    for (uint32_t depth = 0; depth < 3 && ancestor != 0; ++depth) {
        const auto current = GetScene().zones.find(ancestor);
        if (current == GetScene().zones.end() || current->second.parent == 0) break;
        castZones.insert(ancestor);
        ancestor = current->second.parent;
    }
    if (d[2] < 0.0 && o[2] >= 0.0) best = std::min(best, -o[2] / d[2]);

    std::vector<const Entity*> candidates;
    {
        std::lock_guard<std::mutex> lock(g_zoneIndexMutex);
        g_zoneIndex.UpdateUnlocked(GetScene());
        for (uint64_t z : castZones) {
            const auto it = g_zoneIndex.byZone.find(z);
            if (it != g_zoneIndex.byZone.end()) {
                candidates.insert(candidates.end(), it->second.begin(), it->second.end());
            }
        }
    }

    std::unordered_map<uint64_t, Vec3> zoneOffsets;
    for (uint64_t z : castZones) {
        Vec3 offset;
        if (ZoneToZone(z, zone, Vec3{ 0, 0, 0 }, offset)) {
            zoneOffsets[z] = offset;
        }
    }

    for (const Entity* pe : candidates) {
        const Entity& e = *pe;
        if (!e.streamed || e.removing || e.radius <= 0.0 || skip.count(e.id)) continue;
        const auto it = zoneOffsets.find(e.zone);
        if (it == zoneOffsets.end()) continue;
        const Vec3& zoneOffset = it->second;
        const Vec3 c{ e.pos[0] + zoneOffset[0], e.pos[1] + zoneOffset[1], e.pos[2] + zoneOffset[2] };
        const Vec3 oc{ o[0] - c[0], o[1] - c[1], o[2] - c[2] };
        const double b = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const double cc = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - e.radius * e.radius;
        const double disc = b * b - cc;
        if (disc < 0.0) continue;
        double t = -b - std::sqrt(disc);
        if (t < 0.0) t = -b + std::sqrt(disc);
        if (t >= 0.0 && t < best) {
            best = t;
            bestEntity = e.id;
        }
    }
    if (best > dist) {
        why = "no hit";
        return SCO_NOT_FOUND;
    }
    (void)bestEntity;
    for (size_t i = 0; i < 3; ++i) hit->pos[i] = o[i] + d[i] * best;
    hit->distance = best;
    return SCO_OK;
}

const ServiceDef& WorldService() {
    static const ServiceDef def{ SC_WORLD_NAME, SC_WORLD_VERSION_1_1, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
