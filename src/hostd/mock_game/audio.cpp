#include "hostd/mock_game/audio.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "hostd/mock_game/services.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

namespace scuba::hostd::mock_game {
namespace {
struct Trigger { const void* owner; std::string name; };
struct Playback { const void* owner; };
std::set<std::string> names{"mock.beep"};
std::map<uint64_t, Trigger> triggers;
std::map<uint64_t, Playback> playbacks;
uint64_t nextToken = 1; // Never reset: stale tokens cannot become valid again.

uint64_t Token() {
    if (!nextToken) return 0;
    return nextToken++;
}
bool Available() {
    if (!Published()) return false;
    const auto services = Services();
    return std::any_of(services.begin(), services.end(), [](const ServiceInfo& s) {
        return s.name == SC_AUDIO_NAME && s.published;
    });
}
sco_result Check(sco_plugin* self) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!self || !LivePlugin(self)) return SCO_BAD_ARG;
    return Available() ? SCO_OK : SCO_UNAVAILABLE;
}
sco_result FindTrigger(sco_plugin* self, uint64_t id) {
    const auto it = triggers.find(id);
    if (it == triggers.end()) return SCO_NOT_FOUND;
    return it->second.owner == self ? SCO_OK : SCO_FAILED;
}
sco_result Resolve(sco_plugin* self, const char* name, uint64_t* out) {
    if (out) *out = 0;
    const auto r = Check(self);
    if (r != SCO_OK) return r;
    if (!name || !out) return SCO_BAD_ARG;
    size_t n = 0;
    while (n <= SC_AUDIO_MAX_NAME && name[n]) {
        const auto c = static_cast<unsigned char>(name[n]);
        if (c < 32 || c > 126) return SCO_BAD_ARG;
        ++n;
    }
    if (!n || n > SC_AUDIO_MAX_NAME) return SCO_BAD_ARG;
    if (!Ready("game.audio.triggers") && !Ready("game.audio.play_once")) return SCO_UNAVAILABLE;
    if (!names.count(name)) return SCO_NOT_FOUND;
    for (const auto& entry : triggers)
        if (entry.second.owner == self && entry.second.name == name) { *out = entry.first; return SCO_OK; }
    if (triggers.size() >= SC_AUDIO_MAX_TRIGGERS) return SCO_TOO_MANY;
    const uint64_t id = Token();
    if (!id) return SCO_TOO_MANY;
    triggers.emplace(id, Trigger{self, name});
    *out = id;
    return SCO_OK;
}
sco_result Play(sco_plugin* self, uint64_t trigger, uint64_t* out) {
    if (out) *out = 0;
    const auto r = Check(self);
    if (r != SCO_OK) return r;
    if (!out) return SCO_BAD_ARG;
    if (!Ready("game.audio.triggers")) return SCO_UNAVAILABLE;
    const auto found = FindTrigger(self, trigger);
    if (found != SCO_OK) return found;
    if (playbacks.size() >= SC_AUDIO_MAX_PLAYBACKS) return SCO_TOO_MANY;
    const uint64_t id = Token();
    if (!id) return SCO_TOO_MANY;
    playbacks.emplace(id, Playback{self});
    *out = id;
    return SCO_OK;
}
sco_result Stop(sco_plugin* self, uint64_t id) {
    const auto r = Check(self);
    if (r != SCO_OK) return r;
    const auto it = playbacks.find(id);
    if (it == playbacks.end()) return SCO_NOT_FOUND;
    if (it->second.owner != self) return SCO_FAILED;
    playbacks.erase(it);
    return SCO_OK;
}
sco_result PlayOnce(sco_plugin* self, uint64_t trigger) {
    const auto r = Check(self);
    if (r != SCO_OK) return r;
    if (!Ready("game.audio.play_once")) return SCO_UNAVAILABLE;
    return FindTrigger(self, trigger);
}
void Reset() { triggers.clear(); playbacks.clear(); names = {"mock.beep"}; }
void Release(const void* owner) {
    for (auto it = triggers.begin(); it != triggers.end();) {
        if (it->second.owner == owner) it = triggers.erase(it); else ++it;
    }
    for (auto it = playbacks.begin(); it != playbacks.end();) {
        if (it->second.owner == owner) it = playbacks.erase(it); else ++it;
    }
}
const sc_audio_v1 table{sizeof(sc_audio_v1), 0, Resolve, Play, Stop, PlayOnce};
}
std::set<std::string>& AudioNames() { return names; }
const ServiceDef& AudioService() {
    static const ServiceDef def{SC_AUDIO_NAME, SC_AUDIO_VERSION_1_1, &table, Reset, Release, nullptr};
    return def;
}
}
