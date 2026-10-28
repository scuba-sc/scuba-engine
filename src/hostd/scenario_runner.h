// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "hostd/mock_game/mock_game.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scuba::hostd {

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
