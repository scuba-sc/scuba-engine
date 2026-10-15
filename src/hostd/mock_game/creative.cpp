// game.creative 1.0 (sc_creative.h): local-player toggles, each owned by the plugin that last
// switched it on and switched off when that plugin unloads.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_creative.h"

#include <cmath>

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kGod = "game.creative.god_mode";
constexpr const char* kFly = "game.creative.fly";
constexpr const char* kAmmo = "game.creative.ammo";
constexpr const char* kShipAmmo = "game.creative.ship_ammo";
constexpr float kGameFlySpeed = 0.0f;   // 0: the game's own speed

struct Toggle {
    bool on = false;
    const void* owner = nullptr;
};

ErrorBook g_err;
Toggle g_god, g_fly, g_ammo, g_shipAmmo;
float g_flySpeed = kGameFlySpeed;
const void* g_flySpeedOwner = nullptr;

sco_result Switch(sco_plugin* self, const char* cap, Toggle& t, int on) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(cap)) return g_err.Fail(self, SCO_UNAVAILABLE, std::string(cap) + " isn't ready");
    t.on = on != 0;
    t.owner = t.on ? self : nullptr;
    return SCO_OK;
}

sco_result SetGodMode(sco_plugin* self, int on) { return Switch(self, kGod, g_god, on); }

sco_result SetFlyMode(sco_plugin* self, int on) {
    if (on && OnGame() && LivePlugin(self) && Ready(kFly) && !GetScene().spawned)
        return g_err.Fail(self, SCO_FAILED, "you're not spawned yet");
    return Switch(self, kFly, g_fly, on);
}

sco_result SetFlySpeed(sco_plugin* self, float mps) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kFly)) return g_err.Fail(self, SCO_UNAVAILABLE, "game.creative.fly isn't ready");
    if (!std::isfinite(mps) || !(mps > 0.0f) || mps > static_cast<float>(SC_CREATIVE_FLY_SPEED_MAX))
        return g_err.Fail(self, SCO_BAD_ARG, "the speed must be finite, above 0 and at most SC_CREATIVE_FLY_SPEED_MAX");
    g_flySpeed = mps;
    g_flySpeedOwner = self;
    return SCO_OK;
}

sco_result SetInfiniteAmmo(sco_plugin* self, int on) { return Switch(self, kAmmo, g_ammo, on); }
sco_result SetInfiniteShipAmmo(sco_plugin* self, int on) { return Switch(self, kShipAmmo, g_shipAmmo, on); }

sco_result State(sco_plugin* self, uint32_t* outFlags) {
    if (!OnGame()) return WrongThread(g_err);
    if (!LivePlugin(self)) return g_err.Fail(self, SCO_BAD_ARG, "unknown or released plugin handle");
    if (!Ready(kGod) && !Ready(kFly) && !Ready(kAmmo) && !Ready(kShipAmmo))
        return g_err.Fail(self, SCO_UNAVAILABLE, "no game.creative capability is ready");
    if (!outFlags) return g_err.Fail(self, SCO_BAD_ARG, "out_flags is NULL");
    uint32_t flags = 0;
    if (g_god.on) flags |= SC_CREATIVE_GOD_MODE;
    if (g_fly.on) flags |= SC_CREATIVE_FLY;
    if (g_ammo.on) flags |= SC_CREATIVE_INFINITE_AMMO;
    if (g_shipAmmo.on) flags |= SC_CREATIVE_INFINITE_SHIP_AMMO;
    *outFlags = flags;
    return SCO_OK;
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (self && !LivePlugin(self)) return SCO_BAD_ARG;
    return g_err.Copy(self, out, inoutSize);
}

void Reset() {
    g_err.Reset();
    g_god = g_fly = g_ammo = g_shipAmmo = Toggle{};
    g_flySpeed = kGameFlySpeed;
    g_flySpeedOwner = nullptr;
}

void Release(const void* owner) {
    g_err.Forget(owner);
    for (Toggle* t : { &g_god, &g_fly, &g_ammo, &g_shipAmmo })
        if (t->owner == owner) *t = Toggle{};
    if (g_flySpeedOwner == owner) {
        g_flySpeed = kGameFlySpeed;
        g_flySpeedOwner = nullptr;
    }
}

const sc_creative_v1 kTable = { sizeof(sc_creative_v1), 0, SetGodMode, SetFlyMode, SetFlySpeed,
                                SetInfiniteAmmo, SetInfiniteShipAmmo, State, LastError };

}  // namespace

const ServiceDef& CreativeService() {
    static const ServiceDef def{ SC_CREATIVE_SERVICE_NAME, SC_CREATIVE_SERVICE_VERSION, &kTable, Reset, Release, nullptr };
    return def;
}

}  // namespace scuba::hostd::mock_game
