// game.world.control 1.0 (sc_world_control.h): filtered rays skipping your own entities, and an
// exclusive free-camera lease that restores the captured camera on release or unload.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_world_control.h"

#include <algorithm>
#include <cmath>

namespace scuba::hostd::mock_game {

namespace {

const void* g_leaseOwner = nullptr;
uint64_t g_lease = 0;
uint64_t g_nextLease = 0xca000001;
Vec3 g_savedPos{};
Quat g_savedRot{};
bool g_savedFlag = false;

void Restore() {
    Scene& s = GetScene();
    s.cameraPos = g_savedPos;
    s.cameraRot = g_savedRot;
    s.freeCameraFlag = g_savedFlag;
    g_leaseOwner = nullptr;
    g_lease = 0;
}

sco_result RaycastFiltered(sco_plugin* self, uint64_t zone, const double from[3], const double dir[3], double maxDist,
                           const uint64_t* skipIds, uint32_t skipCount, sc_world_hit* hit) {
    if (hit && hit->size >= sizeof(sc_world_hit)) {
        const uint32_t size = hit->size;
        *hit = sc_world_hit{};
        hit->size = size;
    }
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || skipCount > SC_WORLD_CONTROL_MAX_SKIPS || (skipCount > 0 && !skipIds)) {
        if (hit && hit->size >= sizeof(sc_world_hit)) {
            const uint32_t size = hit->size;
            *hit = sc_world_hit{};
            hit->size = size;
        }
        return SCO_BAD_ARG;
    }
    if (!Ready("game.world.raycast_filtered")) return SCO_UNAVAILABLE;
    std::set<uint64_t> skip;
    for (uint32_t i = 0; i < skipCount; ++i) {
        if (skipIds[i] == 0 || !skip.insert(skipIds[i]).second) return SCO_BAD_ARG;
        const Entity* e = FindEntity(skipIds[i]);
        if (!e || e->owner != self || e->kept ||
            (e->origin != Origin::Entities && e->origin != Origin::Prefab && e->origin != Origin::SpawnAs))
            return SCO_FAILED;
    }
    std::string why;
    return CastRay(zone, from, dir, maxDist, skip, hit, why);
}

sco_result CameraAcquire(sco_plugin* self, uint64_t* outLease) {
    if (outLease) *outLease = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !outLease) return SCO_BAD_ARG;
    if (!Ready("game.world.free_camera")) return SCO_UNAVAILABLE;
    if (g_leaseOwner && g_leaseOwner != self) return SCO_FAILED;
    if (g_leaseOwner == self) {
        *outLease = g_lease;
        return SCO_OK;
    }
    Scene& s = GetScene();
    g_savedPos = s.cameraPos;
    g_savedRot = s.cameraRot;
    g_savedFlag = s.freeCameraFlag;
    g_leaseOwner = self;
    g_lease = g_nextLease++;
    s.freeCameraFlag = true;
    *outLease = g_lease;
    return SCO_OK;
}

sco_result CameraStep(sco_plugin* self, uint64_t lease, const sc_world_camera_step* step) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !step || step->size < sizeof(sc_world_camera_step) || step->_pad != 0) return SCO_BAD_ARG;
    if (!Ready("game.world.free_camera")) return SCO_UNAVAILABLE;
    if (!Finite(step->translation, 3) || !Finite(step->rotation_xyzw, 4)) return SCO_BAD_ARG;
    const double* t = step->translation;
    if (std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) > SC_WORLD_CONTROL_STEP_METRES) return SCO_BAD_ARG;
    Quat q;
    if (!NormalizeQuat(step->rotation_xyzw, q)) return SCO_BAD_ARG;
    const double angle = 2.0 * std::acos(std::min(1.0, std::fabs(q[3])));
    if (angle > SC_WORLD_CONTROL_STEP_RADIANS) return SCO_BAD_ARG;
    if (lease == 0 || lease != g_lease) return SCO_NOT_FOUND;
    if (g_leaseOwner != self) return SCO_FAILED;
    Scene& s = GetScene();
    for (size_t i = 0; i < 3; ++i) s.cameraPos[i] += t[i];
    Quat r;
    NormalizeQuat(QuatMul(q, s.cameraRot).data(), r);
    s.cameraRot = r;
    return SCO_OK;
}

sco_result CameraRelease(sco_plugin* self, uint64_t lease) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (lease == 0 || lease != g_lease) return SCO_NOT_FOUND;
    if (g_leaseOwner != self) return SCO_FAILED;
    Restore();
    return SCO_OK;
}

void Reset() {
    g_leaseOwner = nullptr;
    g_lease = 0;
}

void Release(const void* owner) {
    if (g_leaseOwner == owner) Restore();
}

const sc_world_control_v1 kTable = { sizeof(sc_world_control_v1), 0, RaycastFiltered, CameraAcquire, CameraStep,
                                     CameraRelease };

}  // namespace

const ServiceDef& WorldControlService() {
    static const ServiceDef def{ SC_WORLD_CONTROL_NAME, SC_WORLD_CONTROL_VERSION_1_0, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
