// game.quantum 1.0 (sc_quantum.h): enable, a bounded hold that charges a synthetic drive tick by
// tick, then a short boost. One owner at a time.
#include "hostd/mock_game/common.h"
#include "hostd/mock_game/services.h"
#include "sc_quantum.h"

namespace scuba::hostd::mock_game {

namespace {

constexpr const char* kCap = "game.quantum.control";
constexpr uint32_t kChargePerTick = 100000;
constexpr uint32_t kBoostTicks = 5;

bool g_enabled = false;
const void* g_owner = nullptr;
uint32_t g_phase = SC_QUANTUM_IDLE;
uint32_t g_progress = 0;
uint32_t g_holdUntil = 0;
uint32_t g_boostLeft = 0;

bool Admit(sco_plugin* self, sco_result& r) {
    if (!OnGame()) { r = SCO_WRONG_THREAD; return false; }
    if (!LivePlugin(self)) { r = SCO_BAD_ARG; return false; }
    if (!Ready(kCap)) { r = SCO_UNAVAILABLE; return false; }
    if (g_owner && g_owner != self) { r = SCO_FAILED; return false; }
    return true;
}

void Idle() {
    g_phase = SC_QUANTUM_IDLE;
    g_progress = 0;
    g_boostLeft = 0;
    g_holdUntil = 0;
}

sco_result Enable(sco_plugin* self, uint32_t on) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (on > 1u) return SCO_BAD_ARG;
    if (on && GetScene().playerShip == 0) return SCO_FAILED;
    g_enabled = on == 1u;
    g_owner = g_enabled ? self : nullptr;
    if (!g_enabled) Idle();
    return SCO_OK;
}

sco_result Hold(sco_plugin* self, uint32_t ms) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (ms == 0 || ms > SC_QUANTUM_MAX_HOLD_MS) return SCO_BAD_ARG;
    if (!g_enabled || GetScene().playerShip == 0) return SCO_FAILED;
    if (g_phase == SC_QUANTUM_IDLE) g_phase = SC_QUANTUM_CHARGING;
    g_holdUntil = GetScene().nowMs + ms;
    return SCO_OK;
}

sco_result ReleaseHold(sco_plugin* self) {
    sco_result r = SCO_OK;
    if (!Admit(self, r)) return r;
    if (g_phase == SC_QUANTUM_CHARGING) Idle();
    g_holdUntil = 0;
    return SCO_OK;
}

sco_result State(sco_plugin* self, sc_quantum_state* out) {
    if (!OnGame()) return SCO_WRONG_THREAD;
    if (!LivePlugin(self) || !out || out->size < sizeof(sc_quantum_state)) return SCO_BAD_ARG;
    const bool held = g_phase == SC_QUANTUM_CHARGING && GetScene().nowMs < g_holdUntil;
    out->enabled = g_enabled ? 1u : 0u;
    out->held = held ? 1u : 0u;
    out->phase = g_phase;
    out->progress = g_progress;
    out->session = GetScene().session;
    if (!Ready(kCap))
        out->block = SC_QUANTUM_BLOCK_UNAVAILABLE;
    else if (g_enabled && GetScene().playerShip == 0)
        out->block = SC_QUANTUM_BLOCK_DRIVE;
    else
        out->block = SC_QUANTUM_BLOCK_NONE;
    return SCO_OK;
}

void Tick() {
    if (g_phase == SC_QUANTUM_CHARGING) {
        if (!Ready(kCap) || !g_enabled || GetScene().nowMs >= g_holdUntil || GetScene().playerShip == 0) {
            Idle();
            return;
        }
        g_progress = g_progress + kChargePerTick >= SC_QUANTUM_PROGRESS_MAX ? SC_QUANTUM_PROGRESS_MAX
                                                                             : g_progress + kChargePerTick;
        if (g_progress == SC_QUANTUM_PROGRESS_MAX) {
            g_phase = SC_QUANTUM_BOOSTING;
            g_boostLeft = kBoostTicks;
        }
    } else if (g_phase == SC_QUANTUM_BOOSTING) {
        if (g_boostLeft > 0) --g_boostLeft;
        if (g_boostLeft == 0) Idle();
    }
}

void Reset() {
    g_enabled = false;
    g_owner = nullptr;
    Idle();
}

void Release(const void* owner) {
    if (g_owner == owner) Reset();
}

const sc_quantum_v1 kTable = { sizeof(sc_quantum_v1), 0, Enable, Hold, ReleaseHold, State };

}  // namespace

const ServiceDef& QuantumService() {
    static const ServiceDef def{ SC_QUANTUM_SERVICE_NAME, SC_QUANTUM_SERVICE_VERSION, &kTable, Reset, Release, Tick };
    return def;
}

}  // namespace scuba::hostd::mock_game
