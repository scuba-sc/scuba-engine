// game.prefabs 1.0 (sc_prefabs.h): object containers spawned as game.entities-owned entities.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_entities.h"
#include "sc_prefabs.h"

#include <cstring>

namespace scuba::hostd::mock_game {

namespace {

// ObjectContainers/<components>/name.socpak; components of [A-Za-z0-9_-.{}], none empty, "." or "..".
bool ValidContentPath(const char* path) {
    const size_t length = std::strlen(path);
    if (length > SC_PREFABS_MAX_PATH) return false;
    const std::string p(path, length);
    const std::string prefix = "ObjectContainers/", suffix = ".socpak";
    if (p.size() <= prefix.size() + suffix.size() || p.compare(0, prefix.size(), prefix) != 0 ||
        p.compare(p.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        const std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '.' || c == '{' || c == '}'))
                return false;
        start = end + 1;
    }
    return true;
}

sco_result Spawn(sco_plugin* self, const char* path, uint64_t zone, const double pos[3], const double rot[4],
                 uint64_t* outId) {
    if (outId) *outId = 0;
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self)) return SCO_BAD_ARG;
    if (!Ready("game.entities.prefabs")) return SCO_UNAVAILABLE;
    Quat q;
    if (!path || !pos || !rot || !outId || zone == 0 || !ValidContentPath(path) || !Finite(pos, 3) ||
        !NormalizeQuat(rot, q))
        return SCO_BAD_ARG;
    if (!ZoneStreamed(zone) || !GetScene().content.count(path)) return SCO_NOT_FOUND;
    if (CountOwnedBy(Origin::Prefab, self) >= SC_PREFABS_MAX_PER_OWNER ||
        CountOwned(Origin::Entities) + CountOwned(Origin::Prefab) >= SC_ENTITIES_MAX_OWNED)
        return SCO_TOO_MANY;
    Entity e;
    e.cls = path;
    e.zone = zone;
    e.pos = { pos[0], pos[1], pos[2] };
    e.rot = q;
    e.radius = 2.0;
    e.owner = self;
    e.origin = Origin::Prefab;
    *outId = AddEntity(std::move(e));
    return SCO_OK;
}

void Reset() {}
void Release(const void*) {}

const sc_prefabs_v1 kTable = { sizeof(sc_prefabs_v1), 0, Spawn };

}  // namespace

const ServiceDef& PrefabsService() {
    static const ServiceDef def{ SC_PREFABS_NAME, SC_PREFABS_VERSION_1_0, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
