// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/scenario_runner.h"
#include "hostd/mock_game/common.h"
#include "sco/runtime.h"
#include <utility>

namespace scuba::hostd {
namespace {
bool Available() { return mock_game::Published() && sco::OnGameThread(); }
}

bool ScenarioRunner::SetProfile(const std::string& profile) const {
    return Available() && mock_game::ApplyProfile(profile);
}
bool ScenarioRunner::SetCapability(const std::string& name, bool ready) const {
    return Available() && mock_game::SetCapability(name, ready);
}
bool ScenarioRunner::SetService(const std::string& name, bool published) const {
    return Available() && mock_game::SetServicePublished(name, published);
}
std::vector<mock_game::CapabilityInfo> ScenarioRunner::Capabilities() const {
    return Available() ? mock_game::Capabilities() : std::vector<mock_game::CapabilityInfo>{};
}
std::vector<mock_game::ServiceInfo> ScenarioRunner::Services() const {
    return Available() ? mock_game::Services() : std::vector<mock_game::ServiceInfo>{};
}
uint64_t ScenarioRunner::AddEntity(const std::string& name, bool actor) const {
    if (!Available() || name.empty() || name.size() > 95) return 0;
    for (const char c : name)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
    auto& scene = mock_game::GetScene();
    if (scene.entities.size() >= 1024 || scene.zones.empty()) return 0;
    mock_game::Entity entity;
    entity.cls = name;
    entity.actor = actor;
    entity.zone = scene.zones.begin()->first;
    entity.streamed = true;
    scene.classes.insert(name);
    return mock_game::AddEntity(std::move(entity));
}
size_t ScenarioRunner::EntityCount() const {
    return Available() ? mock_game::GetScene().entities.size() : 0;
}
bool ScenarioRunner::SpawnPlayer() const {
    return Available() && mock_game::SpawnPlayer(0, nullptr);
}
bool ScenarioRunner::FireVehicleSeat(bool boarded, uint64_t ship, int32_t seatIndex) const {
    return Available() && mock_game::FireVehicleSeat(boarded, ship, seatIndex);
}
}  // namespace scuba::hostd
