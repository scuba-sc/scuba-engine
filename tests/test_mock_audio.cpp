#include "hostd/mock_game/audio.h"
#include "hostd/mock_game/services.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include <cassert>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace mg = scuba::hostd::mock_game;
// Standalone host seam: the actual service implementation is under test.
namespace scuba::hostd::mock_game {
bool gameThread = true, published = true, triggers = true, once = true;
std::set<const sco_plugin*> live;
bool OnGame() { return gameThread; }
bool LivePlugin(const sco_plugin* p) { return live.count(p) != 0; }
bool Published() { return published; }
bool Ready(const char* cap) { return std::strcmp(cap, "game.audio.triggers") == 0 ? triggers : once; }
std::vector<ServiceInfo> Services() { return {{SC_AUDIO_NAME, SC_AUDIO_VERSION_1_1, published}}; }
}
int main() {
    auto* a = reinterpret_cast<sco_plugin*>(1);
    auto* b = reinterpret_cast<sco_plugin*>(2);
    mg::live = {a, b};
    const auto& def = mg::AudioService();
    const auto* t = static_cast<const sc_audio_v1*>(def.table);
    assert(def.version == SC_AUDIO_VERSION_1_1 && t->size == sizeof(*t));
    uint64_t trigger = 99, again = 0, playback = 0;
    assert(t->resolve(a, "mock.beep", &trigger) == SCO_OK && trigger);
    assert(t->resolve(a, "mock.beep", &again) == SCO_OK && again == trigger);
    assert(t->play(a, trigger, &playback) == SCO_OK && playback && playback != trigger);
    assert(t->stop(b, playback) == SCO_FAILED);
    assert(t->stop(a, playback) == SCO_OK);
    assert(t->stop(a, playback) == SCO_NOT_FOUND);
    assert(t->play_once(a, trigger) == SCO_OK);
    assert(t->resolve(a, "unknown", &again) == SCO_NOT_FOUND && again == 0);
    assert(t->resolve(a, "", &again) == SCO_BAD_ARG && again == 0);
    mg::triggers = false;
    assert(t->play(a, trigger, &playback) == SCO_UNAVAILABLE && playback == 0);
    assert(t->play_once(a, trigger) == SCO_OK);
    mg::triggers = true;
    assert(t->play(a, trigger, &playback) == SCO_OK);
    def.release(a);
    assert(t->stop(a, playback) == SCO_NOT_FOUND);
    assert(t->play(a, trigger, &playback) == SCO_NOT_FOUND);
    mg::gameThread = false;
    assert(t->resolve(a, "mock.beep", &again) == SCO_WRONG_THREAD && again == 0);
    mg::gameThread = true;
    def.reset();
    assert(t->resolve(a, "mock.beep", &again) == SCO_OK && again != trigger);
    assert(t->resolve(b, "mock.beep", &trigger) == SCO_OK && trigger != again);
    assert(t->play(a, trigger, &playback) == SCO_FAILED);
    assert(t->resolve(a, "bad\nname", &playback) == SCO_BAD_ARG && playback == 0);
    std::string longName(128, 'x');
    assert(t->resolve(a, longName.c_str(), &playback) == SCO_BAD_ARG && playback == 0);
    assert(t->resolve(nullptr, "mock.beep", &playback) == SCO_BAD_ARG && playback == 0);
    assert(t->resolve(a, "mock.beep", nullptr) == SCO_BAD_ARG);
    std::vector<uint64_t> active;
    for (unsigned i = 0; i < SC_AUDIO_MAX_PLAYBACKS; ++i) {
        assert(t->play(a, again, &playback) == SCO_OK);
        active.push_back(playback);
    }
    assert(t->play(a, again, &playback) == SCO_TOO_MANY && playback == 0);
    assert(t->play_once(a, again) == SCO_OK);
    assert(t->stop(a, active.front()) == SCO_OK);
    assert(t->play(a, again, &playback) == SCO_OK);
    mg::once = false;
    assert(t->play_once(a, again) == SCO_UNAVAILABLE);
    mg::once = true;
    mg::published = false;
    assert(t->play_once(a, again) == SCO_UNAVAILABLE);
    mg::published = true;
    def.release(a);
    assert(t->stop(a, playback) == SCO_NOT_FOUND);
    def.reset();
    for (unsigned i = 0; i < SC_AUDIO_MAX_TRIGGERS; ++i) {
        const auto name = std::string("mock.") + std::to_string(i);
        mg::AudioNames().insert(name);
        assert(t->resolve(a, name.c_str(), &playback) == SCO_OK);
    }
    assert(t->resolve(a, "mock.beep", &playback) == SCO_TOO_MANY && playback == 0);
    assert(sc_audio_play_once(nullptr, a, 0) == SCO_UNAVAILABLE);
}
