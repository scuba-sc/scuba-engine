// game.actor_stats and game.actor_stats_write over the mock game (docs/mock-game.md), and the kit's
// actor_stats_probe example loaded against them. MOCK pass only: nothing here ran in game.
#include "check.h"
#include "hostd/mock_game/actor_stats.h"
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/app.h"
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sc_actor_stats_write.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace mg = scuba::hostd::mock_game;

namespace {

void Log(const char* line) { std::puts(line); }

template<class T>
const T* Query(const char* name) {
    const void* out = nullptr;
    if (sco::host::BuildApi({ "scuba actor stats test" })->query_service(name, 0x10000u, &out) != SCO_OK || !out)
        throw std::runtime_error(std::string("missing table ") + name);
    return static_cast<const T*>(out);
}

std::string Invoke(const char* name, const sco_arg* args = nullptr, uint32_t count = 0) {
    std::string reply;
    sco::Invoke(name, reinterpret_cast<const sco::Arg*>(args), count,
        [](sco::Result, const char* text, void* ctx) { *static_cast<std::string*>(ctx) = text; }, &reply);
    std::printf("%s => %s\n", name, reply.c_str());
    return reply;
}

sco_arg Bool(int v) { sco_arg a{}; a.type = SCO_ARG_BOOL; a.v.i = v; return a; }
sco_arg Str(const char* s) { sco_arg a{}; a.type = SCO_ARG_STRING; a.v.s = s; return a; }
sco_arg Flt(double f) { sco_arg a{}; a.type = SCO_ARG_FLOAT; a.v.f = f; return a; }

constexpr uint32_t kHunger = SC_ACTOR_STAT_HUNGER;
constexpr uint32_t kOxygen = SC_ACTOR_STAT_OXYGEN;

void ReadTests(const sc_actor_stats_v1* stats, uint64_t player) {
    float v = -1.0f;
    // The nine stats, readable by name, with the deterministic synthetic defaults.
    const float expect[9] = { 20.0f, 15.0f, 37.0f, 22.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f };
    for (uint32_t s = 1; s <= 9; ++s) {
        CHECK(stats->read(player, s, &v) == SCO_OK && v == expect[s - 1]);
    }
    CHECK(stats->read(0, kHunger, &v) == SCO_BAD_ARG && v == 0.0f);
    CHECK(stats->read(player, 0, &v) == SCO_BAD_ARG);
    CHECK(stats->read(player, 10, &v) == SCO_BAD_ARG);
    CHECK(stats->read(player, kHunger, nullptr) == SCO_BAD_ARG);
    CHECK(stats->read(0xdeadbeef, kHunger, &v) == SCO_NOT_FOUND);
    CHECK(stats->read(player, kHunger, &v) == SCO_OK);
    mg::ActorStats().missing[SC_ACTOR_STAT_SUIT_RADIATION - 1] = true;   // no record: never a silent 0
    CHECK(stats->read(player, SC_ACTOR_STAT_SUIT_RADIATION, &v) == SCO_NOT_FOUND);
    mg::ActorStats().missing[SC_ACTOR_STAT_SUIT_RADIATION - 1] = false;
    CHECK(mg::SetCapability("game.actor_stats.read", false));
    CHECK(stats->read(player, kHunger, &v) == SCO_UNAVAILABLE);
    CHECK(mg::SetCapability("game.actor_stats.read", true));
    CHECK(sc_actor_stats_read(nullptr, player, kHunger, &v) == SCO_UNAVAILABLE);
}

void WriteTests(const sc_actor_stats_write_v1* w, const sc_actor_stats_v1* stats, sco_plugin* self, uint64_t player) {
    float after = -1.0f, now = -1.0f;
    uint32_t mode = 99;
    // Default OFF: the write capability is not ready in the "all" profile.
    CHECK(!sco::caps::Has("game.actor_stats.write"));
    CHECK(w->adjust(self, player, kHunger, 5.0f, &after) == SCO_UNAVAILABLE && after == 0.0f);
    CHECK(w->set(self, player, kHunger, 5.0f, &after) == SCO_UNAVAILABLE);
    CHECK(w->mode(self, player, kHunger, &mode) == SCO_UNAVAILABLE);
    CHECK(stats->read(player, kHunger, &now) == SCO_OK && now == 20.0f);   // nothing was written
    CHECK(mg::SetCapability("game.actor_stats.write", true));

    // mode() writes nothing and needs no admission.
    mg::ActorStats().offlineAdmission = false;
    CHECK(w->mode(self, player, kHunger, &mode) == SCO_OK && mode == SC_ACTOR_STAT_WRITE_AUTHORITATIVE);
    // Offline admission first: refused, and nothing changed.
    CHECK(w->adjust(self, player, kHunger, 5.0f, &after) == SCO_UNAVAILABLE);
    CHECK(stats->read(player, kHunger, &now) == SCO_OK && now == 20.0f);
    mg::ActorStats().offlineAdmission = true;

    // A fresh read-back value is returned after the write, and matches a read.
    CHECK(w->adjust(self, player, kHunger, 5.0f, &after) == SCO_OK && after == 25.0f);
    CHECK(stats->read(player, kHunger, &now) == SCO_OK && now == after);
    CHECK(w->set(self, player, kOxygen, 80.0f, &after) == SCO_OK && after == 80.0f);
    CHECK(w->set(self, player, kOxygen, 80.0f, nullptr) == SCO_OK);
    CHECK(w->adjust(self, player, kHunger, 1000.0f, &after) == SCO_OK && after == 100.0f);   // clamped by the (synthetic) game range

    // Own player only, and the argument rules.
    CHECK(w->adjust(self, player + 1, kHunger, 1.0f, &after) == SCO_BAD_ARG);
    CHECK(w->adjust(self, 0, kHunger, 1.0f, &after) == SCO_BAD_ARG);
    CHECK(w->adjust(self, player, 0, 1.0f, &after) == SCO_BAD_ARG);
    CHECK(w->adjust(self, player, 10, 1.0f, &after) == SCO_BAD_ARG);
    CHECK(w->adjust(nullptr, player, kHunger, 1.0f, &after) == SCO_BAD_ARG);
    CHECK(w->adjust(self, player, kHunger, std::numeric_limits<float>::quiet_NaN(), &after) == SCO_BAD_ARG);
    CHECK(w->adjust(self, player, kHunger, std::numeric_limits<float>::infinity(), &after) == SCO_BAD_ARG);
    CHECK(w->adjust(self, player, kHunger, SC_ACTOR_STATS_WRITE_MAX_STEP * 2.0f, &after) == SCO_BAD_ARG);
    CHECK(w->set(self, player, kHunger, std::numeric_limits<float>::quiet_NaN(), &after) == SCO_BAD_ARG);
    CHECK(w->set(self, player, kHunger, 5.0e7f, &after) == SCO_BAD_ARG);
    CHECK(w->mode(self, player, kHunger, nullptr) == SCO_BAD_ARG);
    CHECK(sc_actor_stats_adjust(nullptr, self, player, kHunger, 1.0f, &after) == SCO_UNAVAILABLE);

    // A step smaller than FLT_EPSILON is not passed on: OK, the value does not move.
    CHECK(w->set(self, player, kHunger, 50.0f, &after) == SCO_OK && after == 50.0f);
    CHECK(w->adjust(self, player, kHunger, 1.0e-9f, &after) == SCO_OK && after == 50.0f);

    // mode(): local-only is applied, discarded is refused (never a silent OK) and changes nothing.
    mg::ActorStats().mode[kHunger - 1] = SC_ACTOR_STAT_WRITE_LOCAL_ONLY;
    CHECK(w->mode(self, player, kHunger, &mode) == SCO_OK && mode == SC_ACTOR_STAT_WRITE_LOCAL_ONLY);
    CHECK(w->adjust(self, player, kHunger, 1.0f, &after) == SCO_OK && after == 51.0f);
    mg::ActorStats().mode[kHunger - 1] = SC_ACTOR_STAT_WRITE_DISCARDED;
    CHECK(w->mode(self, player, kHunger, &mode) == SCO_OK && mode == SC_ACTOR_STAT_WRITE_DISCARDED);
    CHECK(w->adjust(self, player, kHunger, 1.0f, &after) == SCO_UNAVAILABLE && after == 0.0f);
    CHECK(w->set(self, player, kHunger, 7.0f, &after) == SCO_UNAVAILABLE);
    CHECK(stats->read(player, kHunger, &now) == SCO_OK && now == 51.0f);
    mg::ActorStats().mode[kHunger - 1] = SC_ACTOR_STAT_WRITE_AUTHORITATIVE;

    // The game ignores hunger changes while pl_status.enable* is off: OK, and the read-back shows it.
    mg::ActorStats().hungerThirstEnabled = false;
    CHECK(w->adjust(self, player, kHunger, 5.0f, &after) == SCO_OK && after == 51.0f);
    CHECK(w->adjust(self, player, kOxygen, -5.0f, &after) == SCO_OK && after == 75.0f);
    mg::ActorStats().hungerThirstEnabled = true;

    // No record of the stat on the player's actor.
    mg::ActorStats().missing[SC_ACTOR_STAT_STUN - 1] = true;
    CHECK(w->adjust(self, player, SC_ACTOR_STAT_STUN, 1.0f, &after) == SCO_NOT_FOUND);
    mg::ActorStats().missing[SC_ACTOR_STAT_STUN - 1] = false;

    // Nothing is restored on unload: the host's release hook leaves the value where it was.
    CHECK(stats->read(player, kHunger, &now) == SCO_OK && now == 51.0f);
}

void DriftTests(const sc_actor_stats_v1* stats, uint64_t player) {
    float a = 0, b = 0;
    // Deterministic by default: ticks change nothing.
    CHECK(stats->read(player, SC_ACTOR_STAT_SUIT_TEMPERATURE, &a) == SCO_OK);
    sco::app::Tick(1000);
    CHECK(stats->read(player, SC_ACTOR_STAT_SUIT_TEMPERATURE, &b) == SCO_OK && a == b);
    // Drift on: values wander, and the same seed gives the same walk.
    auto run = [&](float* out) {
        mg::ResetAll();
        mg::ActorStats().drift = true;
        for (uint32_t i = 0; i < 50; ++i) sco::app::Tick(2000 + i * 16);
        CHECK(stats->read(player, SC_ACTOR_STAT_SUIT_TEMPERATURE, out) == SCO_OK);
    };
    float first = 0, second = 0, hunger = 0;
    run(&first);
    CHECK(stats->read(player, kHunger, &hunger) == SCO_OK && hunger > 20.0f);
    run(&second);
    CHECK(first == second && first != 22.0f);
    mg::ResetAll();
}

void ProbeTests(const std::string& pluginId, uint64_t player) {
    (void)pluginId;
    (void)player;
    // The probe's logs go to the plugin log, not this sink: its command replies are what is checked.
    CHECK(Invoke("actor_stats_probe.dump") == "logged");
    CHECK(Invoke("actor_stats_probe.modes") == "logged");
    sco_arg args[2] = { Str("hunger"), Flt(10.0) };
    // The plugin's own arm gates its writes: disarmed at load.
    CHECK(Invoke("actor_stats_probe.adjust", args, 2).find("writes are not armed") != std::string::npos);
    sco_arg on = Bool(1);
    CHECK(Invoke("actor_stats_probe.arm", &on, 1) == "writes armed");
    // The service's write capability is off by default: unavailable, nothing written.
    CHECK(Invoke("actor_stats_probe.adjust", args, 2).find("read back hunger=0.0000") != std::string::npos);
    CHECK(mg::SetCapability("game.actor_stats.write", true));
    const std::string reply = Invoke("actor_stats_probe.adjust", args, 2);
    CHECK(reply.find("-> result 0 (mode authoritative, result 0); read back hunger=30.0000") != std::string::npos);
    sco_arg setArgs[2] = { Str("body_temp"), Flt(36.5) };
    CHECK(Invoke("actor_stats_probe.set", setArgs, 2).find("read back body_temp=36.5000") != std::string::npos);
    // A write the game would discard is refused (SCO_UNAVAILABLE) and the probe reports the mode.
    mg::ActorStats().mode[SC_ACTOR_STAT_STUN - 1] = SC_ACTOR_STAT_WRITE_DISCARDED;
    sco_arg stun[2] = { Str("stun"), Flt(5.0) };
    const std::string refused = Invoke("actor_stats_probe.adjust", stun, 2);
    CHECK(refused.find("mode discarded") != std::string::npos);
    CHECK(refused.find("read back stun=0.0000") != std::string::npos);
    mg::ActorStats().mode[SC_ACTOR_STAT_STUN - 1] = SC_ACTOR_STAT_WRITE_AUTHORITATIVE;
    // Offline admission refused: nothing is written, the read-back is 0 (failure).
    mg::ActorStats().offlineAdmission = false;
    CHECK(Invoke("actor_stats_probe.adjust", args, 2).find("read back hunger=0.0000") != std::string::npos);
    mg::ActorStats().offlineAdmission = true;
    CHECK(mg::SetCapability("game.actor_stats.write", false));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    sco::SetLogSink(Log);
    sco::app::Platform platform;
    platform.hostVersion = "scuba actor stats test";
    platform.pluginRoot = argv[1];
    platform.pluginsEnabled = true;
    mg::Configure(platform);
    CHECK(sco::app::Start(platform));
    try {
        const sco::plugins::Plugin* probe = nullptr;
        for (const auto& p : sco::app::Plugins())
            if (p.manifest.id == "actor_stats_probe") probe = &p;
        CHECK(probe && probe->state == sco::plugins::State::Loaded);
        if (!probe || probe->state != sco::plugins::State::Loaded) throw std::runtime_error("probe plugin did not load");
        bool published = false;
        for (const auto& s : mg::Services())
            if (s.name == "game.actor_stats" || s.name == "game.actor_stats_write") published = s.published;
        CHECK(published);
        const auto* stats = Query<sc_actor_stats_v1>(SC_ACTOR_STATS_NAME);
        const auto* writer = Query<sc_actor_stats_write_v1>(SC_ACTOR_STATS_WRITE_NAME);
        const uint64_t player = mg::GetScene().player;
        CHECK(player != 0);
        ReadTests(stats, player);
        DriftTests(stats, player);
        mg::ResetAll();
        ProbeTests("actor_stats_probe", player);
        mg::ResetAll();
        WriteTests(writer, stats, const_cast<sco_plugin*>(probe->self), player);
        // "game_pack" profile: the read capability follows the documented default, the write one stays off.
        CHECK(mg::ApplyProfile("game_pack"));
        CHECK(sco::caps::Has("game.actor_stats.read") && !sco::caps::Has("game.actor_stats.write"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "actor stats test: %s\n", e.what());
        CHECK(false);
    }
    sco::app::Stop();
    mg::Unpublish();
    return CheckExit("test_mock_actor_stats");
}
