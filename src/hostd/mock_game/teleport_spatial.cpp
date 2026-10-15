// teleport.spatial 1.0 (sc_spatial.h), the product service name the game pack keeps: your pose and
// conversions between the mock zones. 1 answered, 0 couldn't; outputs written only on 1.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_spatial.h"

#include <cstdio>

namespace scuba::hostd::mock_game {

namespace {

bool Admitted() { return OnGame() && Ready("teleport"); }

int PlayerPose(double pos[3], double rot[4], uint64_t* zone) {
    if (!Admitted() || !pos || !rot || !zone || !GetScene().spawned) return 0;
    const Entity* p = FindEntity(GetScene().player);
    if (!p) return 0;
    for (size_t i = 0; i < 3; ++i) pos[i] = p->pos[i];
    for (size_t i = 0; i < 4; ++i) rot[i] = p->rot[i];
    *zone = p->zone;
    return 1;
}

int ZoneOfEntity(uint64_t id, uint64_t* zone) {
    if (!Admitted() || !zone) return 0;
    const Entity* e = StreamedEntity(id);
    if (!e || e->zone == 0) return 0;
    *zone = e->zone;
    return 1;
}

int Convert(uint64_t from, uint64_t to, const double in[3], double out[3]) {
    if (!Admitted() || !in || !out || !Finite(in, 3) || !ZoneKnown(from) || !ZoneKnown(to)) return 0;
    Vec3 v;
    if (!ZoneToZone(from, to, Vec3{ in[0], in[1], in[2] }, v)) return 0;
    for (size_t i = 0; i < 3; ++i) out[i] = v[i];
    return 1;
}

int LocalToWorld(uint64_t zone, const double local[3], double world[3]) { return Convert(zone, 0, local, world); }
int WorldToLocal(uint64_t zone, const double world[3], double local[3]) { return Convert(0, zone, world, local); }
int ZoneToZoneFn(uint64_t from, uint64_t to, const double in[3], double out[3]) { return Convert(from, to, in, out); }

int ZoneName(uint64_t zone, char* out, uint32_t cap) {
    if (!Admitted() || !out || cap == 0 || !ZoneStreamed(zone)) return 0;
    std::snprintf(out, cap, "%s", GetScene().zones[zone].name.c_str());
    return 1;
}

void Reset() {}
void Release(const void*) {}

const sc_spatial_v1 kTable = { sizeof(sc_spatial_v1), PlayerPose, ZoneOfEntity, LocalToWorld, WorldToLocal,
                               ZoneToZoneFn, ZoneName };

}  // namespace

const ServiceDef& TeleportSpatialService() {
    static const ServiceDef def{ SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
