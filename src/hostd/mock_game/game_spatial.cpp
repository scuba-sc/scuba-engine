// game.spatial 1.0 (sc_game_spatial.h): zone records and pose conversion over the mock zones.
// Every call returns 1 or 0 and leaves outputs unchanged on 0.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_game_spatial.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "game.spatial";
uint64_t g_epoch = 1;
uint64_t g_epochTick = 0;
std::set<uint64_t> g_observed;

bool Admitted() { return OnGame() && Ready(kCap); }

bool Observe(uint64_t zone) {
    auto observations = g_observed;
    uint64_t cursor = zone;
    std::set<uint64_t> visited;
    while (cursor != 0) {
        const auto it = GetScene().zones.find(cursor);
        if (it == GetScene().zones.end() || !visited.insert(cursor).second) return false;
        observations.insert(cursor);
        if (observations.size() > SC_GAME_SPATIAL_MAX_ZONES) return false;
        cursor = it->second.parent;
    }
    if (observations != g_observed) {
        g_observed = std::move(observations);
        ++g_epoch;
    }
    return zone != 0;
}

uint64_t CurrentEpoch() {
    const uint64_t tick = GetScene().tick;
    if (tick != g_epochTick) {
        g_epochTick = tick;
        ++g_epoch;
        if (GetScene().spawned) {
            const Entity* player = FindEntity(GetScene().player);
            if (player) Observe(player->zone);
        }
    }
    return g_epoch;
}

void Fill(const Zone& z, sc_game_zone* out) {
    out->flags = SC_GAME_ZONE_SYSTEM_UNKNOWN;
    out->epoch = CurrentEpoch();
    out->zone_id = z.id;
    out->parent_id = z.parent;
    out->root_id = RootOf(z.id);
    out->system_id = 0;
    std::memset(out->name, 0, sizeof(out->name));
    std::snprintf(out->name, sizeof(out->name), "%s", z.name.c_str());
    if (z.name.size() >= sizeof(out->name) - 1) out->flags |= SC_GAME_ZONE_NAME_TRUNCATED;
}

int PoseToZone(uint64_t from, uint64_t to, const double pos[3], const double rot[4], double outPos[3],
               double outRot[4]) {
    if (!Admitted() || !pos || !rot || !outPos || !outRot) return 0;
    const auto posStart = reinterpret_cast<uintptr_t>(outPos);
    const auto rotStart = reinterpret_cast<uintptr_t>(outRot);
    if (posStart < rotStart + sizeof(double) * 4 && rotStart < posStart + sizeof(double) * 3) return 0;
    Quat q;
    if (!Finite(pos, 3) || !NormalizeQuat(rot, q)) return 0;
    if (!ZoneKnown(from) || !ZoneKnown(to)) return 0;
    Vec3 converted;
    if (!ZoneToZone(from, to, Vec3{ pos[0], pos[1], pos[2] }, converted)) return 0;
    for (size_t i = 0; i < 3; ++i) outPos[i] = converted[i];
    for (size_t i = 0; i < 4; ++i) outRot[i] = q[i];
    return 1;
}

int ZoneRecord(uint64_t zoneId, sc_game_zone* out) {
    if (!Admitted() || !out || out->size < sizeof(sc_game_zone) || zoneId == 0) return 0;
    const auto& zones = GetScene().zones;
    const auto it = zones.find(zoneId);
    if (it == zones.end() || !Observe(zoneId)) return 0;
    Fill(it->second, out);
    return 1;
}

int FindZone(const char* name, sc_game_zone* out) {
    if (!Admitted() || !name || !*name || !out || out->size < sizeof(sc_game_zone)) return 0;
    if (std::strlen(name) > 95) return 0;
    CurrentEpoch();
    const Zone* found = nullptr;
    for (const auto& [id, z] : GetScene().zones) {
        if (!g_observed.count(id) || z.name.size() >= 95 || z.name != name) continue;
        if (found) return 0;
        found = &z;
    }
    if (!found) return 0;
    Fill(*found, out);
    return 1;
}

int ListZones(uint64_t epoch, uint32_t offset, sc_game_zone* out, uint32_t stride, uint32_t cap,
              uint32_t* outCount, uint32_t* outMore, uint64_t* outEpoch) {
    if (!Admitted() || !outCount || !outMore || !outEpoch) return 0;
    if (cap > SC_GAME_SPATIAL_MAX_PAGE) return 0;
    const uint64_t now = CurrentEpoch();
    if (epoch != 0 && epoch != now) return 0;
    const auto& zones = GetScene().zones;
    const size_t total = g_observed.size();
    if (offset > total) return 0;
    const bool countOnly = cap == 0 || out == nullptr;
    if (!countOnly) {
        if (cap > SC_GAME_SPATIAL_MAX_PAGE || stride < sizeof(sc_game_zone) || stride > 4096) return 0;
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(cap, total - offset));
        auto* base = reinterpret_cast<unsigned char*>(out);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t slotSize = 0;
            std::memcpy(&slotSize, base + static_cast<size_t>(i) * stride, sizeof(slotSize));
            if (slotSize < sizeof(sc_game_zone)) return 0;
        }
    }
    std::vector<const Zone*> sorted;
    for (const auto& [id, z] : zones) {
        if (g_observed.count(id)) sorted.push_back(&z);
    }
    uint32_t written = 0;
    if (!countOnly) {
        auto* base = reinterpret_cast<unsigned char*>(out);
        for (size_t i = offset; i < total && written < cap; ++i, ++written) {
            unsigned char* slot = base + static_cast<size_t>(written) * stride;
            sc_game_zone copy{};
            std::memcpy(&copy.size, slot, sizeof(copy.size));
            Fill(*sorted[i], &copy);
            std::memcpy(slot, &copy, sizeof(copy));
        }
    }
    *outCount = written;
    *outMore = static_cast<uint32_t>(total - offset - written);
    *outEpoch = now;
    return 1;
}

void Reset() {
    ++g_epoch;
    g_epochTick = GetScene().tick;
    g_observed.clear();
    const Entity* player = FindEntity(GetScene().player);
    if (player) Observe(player->zone);
}
void Release(const void*) {}

const sc_game_spatial_v1 kTable = { sizeof(sc_game_spatial_v1), PoseToZone, ZoneRecord, FindZone, ListZones };

}  // namespace

void BumpSpatialEpoch() { ++g_epoch; }

const ServiceDef& GameSpatialService() {
    static const ServiceDef def{ SC_GAME_SPATIAL_NAME, SC_GAME_SPATIAL_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
