// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "hostd/mock_game/mock_game.h"
#include "hostd/json.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scuba::hostd {

bool LoadScenario(const std::string& path, Json& scenario, std::string& error);

// One step's outcome: what the control did, and whether that matched its `expect` (absent = ok).
struct ScenarioStepResult {
    std::string control;
    bool observed = false;    // the control itself took effect
    bool matched = false;     // observed == expected
    std::string detail;       // why it was refused, when it was
};

// Runs every step in order. The results are per step, so a caller can show which one failed; the
// run as a whole passes only when every step matched.
bool RunScenario(const Json& scenario, std::vector<ScenarioStepResult>& results);

// Same, with the SCENARIO lines on stdout that scuba-hostd has always printed.
bool RunScenario(const Json& scenario);

// Synthetic controls for a running mock host. Call only on the host game thread.
// This does not own the host or represent production admission/in-game evidence.
class ScenarioRunner {
public:
    bool SetProfile(const std::string& profile) const;
    bool SetCapability(const std::string& name, bool ready) const;
    bool SetService(const std::string& name, bool published) const;
    std::vector<mock_game::CapabilityInfo> Capabilities() const;
    std::vector<mock_game::ServiceInfo> Services() const;
    uint64_t AddEntity(const std::string& className, bool actor) const;
    size_t EntityCount() const;
    bool SpawnPlayer() const;
    bool FireVehicleSeat(bool boarded, uint64_t ship, int32_t seatIndex) const;
};

}  // namespace scuba::hostd
