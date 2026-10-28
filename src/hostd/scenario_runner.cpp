// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/scenario_runner.h"
#include "hostd/mock_game/common.h"
#include "sco/runtime.h"
#include <utility>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <initializer_list>

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

namespace {
bool Fields(const Json& o, std::initializer_list<const char*> allowed) {
    for (const auto& key : o.keys()) {
        bool found = false;
        for (const auto* name : allowed) if (key == name) found = true;
        if (!found) return false;
    }
    return true;
}
bool Id(const Json* v) {
    return v && v->is_number() && std::isfinite(v->as_number()) && v->as_number() >= 1 &&
           v->as_number() <= 9007199254740991.0 && std::floor(v->as_number()) == v->as_number();
}
bool Vector(const Json* v, size_t count) {
    if (!v || !v->is_array() || v->size() != count) return false;
    for (size_t i = 0; i < count; ++i)
        if (!v->at(i).is_number() || !std::isfinite(v->at(i).as_number())) return false;
    return true;
}
mock_game::Vec3 Position(const Json& v) {
    return {v.at(0).as_number(), v.at(1).as_number(), v.at(2).as_number()};
}
}

bool LoadScenario(const std::string& path, Json& scenario, std::string& error) {
    constexpr size_t limit = 65536;
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open scenario"; return false; }
    std::string text(limit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    auto count = static_cast<size_t>(file.gcount());
    if (count > limit) { error = "scenario exceeds 65536 bytes"; return false; }
    if (file.bad()) { error = "cannot read scenario"; return false; }
    text.resize(count);
    auto parsed = Json::parse(text, &error, true);
    if (!parsed || !parsed->is_object()) { if (parsed) error = "scenario must be an object"; return false; }
    const Json* steps = parsed->find("steps");
    if (!Fields(*parsed, {"steps"}) || !steps || !steps->is_array() || steps->size() > 128) {
        error = "scenario requires only steps, an array of at most 128 entries"; return false;
    }
    for (size_t i = 0; i < steps->size(); ++i) {
        const Json& step = steps->at(i);
        const Json* control = step.find("control");
        const Json* expect = step.find("expect");
        bool valid = step.is_object() && control && control->is_string() &&
            (!expect || (expect->is_string() && (expect->as_string() == "ok" || expect->as_string() == "refused")));
        if (valid && control->as_string() == "spawn") {
            const auto* cls = step.find("class");
            valid = Fields(step, {"control", "class", "zone", "pos", "expect"}) && cls && cls->is_string() &&
                !cls->as_string().empty() && cls->as_string().size() <= 95 &&
                std::all_of(cls->as_string().begin(), cls->as_string().end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                }) && Id(step.find("zone")) && Vector(step.find("pos"), 3);
        } else if (valid && control->as_string() == "teleport") {
            valid = Fields(step, {"control", "id", "zone", "pos", "expect"}) && Id(step.find("id")) && Id(step.find("zone")) && Vector(step.find("pos"), 3);
        } else if (valid && control->as_string() == "despawn") {
            valid = Fields(step, {"control", "id", "expect"}) && Id(step.find("id"));
        } else if (valid && control->as_string() == "pose") {
            valid = Fields(step, {"control", "zone", "pos", "rot", "expect"}) && Id(step.find("zone")) && Vector(step.find("pos"), 3) &&
                (!step.find("rot") || Vector(step.find("rot"), 4));
            if (valid && step.find("rot")) {
                mock_game::Quat q{}, normalized{};
                for (size_t n = 0; n < 4; ++n) q[n] = step.find("rot")->at(n).as_number();
                valid = mock_game::NormalizeQuat(q.data(), normalized);
            }
        } else valid = false;
        if (!valid) { error = "invalid scenario step " + std::to_string(i + 1); return false; }
    }
    scenario = std::move(*parsed);
    return true;
}

bool RunScenario(const Json& scenario) {
    const Json& steps = *scenario.find("steps");
    size_t passed = 0;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.at(i);
        const auto& control = step.find("control")->as_string();
        const char* reason = nullptr;
        if (!Available()) reason = "mock game unavailable";
        else if (control == "spawn") {
            auto& scene = mock_game::GetScene();
            auto zone = static_cast<uint64_t>(step.find("zone")->as_number());
            if (!mock_game::ZoneStreamed(zone)) reason = "unknown zone";
            else if (scene.entities.size() >= 1024) reason = "entity limit";
            else {
                mock_game::Entity e;
                e.cls = step.find("class")->as_string();
                e.zone = zone;
                e.pos = Position(*step.find("pos"));
                scene.classes.insert(e.cls);
                mock_game::AddEntity(std::move(e));
            }
        } else if (control == "despawn") {
            auto* e = mock_game::FindEntity(static_cast<uint64_t>(step.find("id")->as_number()));
            if (!e || e->removing || e->id == mock_game::GetScene().player) reason = "unknown entity or player";
            else mock_game::BeginRemoval(*e);
        } else if (control == "teleport") {
            auto* e = mock_game::FindEntity(static_cast<uint64_t>(step.find("id")->as_number()));
            auto zone = static_cast<uint64_t>(step.find("zone")->as_number());
            if (!e || e->removing) reason = "unknown entity";
            else if (!mock_game::ZoneStreamed(zone)) reason = "unknown zone";
            else { e->zone = zone; e->pos = Position(*step.find("pos")); }
        } else {
            auto zone = static_cast<uint64_t>(step.find("zone")->as_number());
            auto* player = mock_game::FindEntity(mock_game::GetScene().player);
            if (!player || !mock_game::ZoneStreamed(zone)) reason = "unknown player or zone";
            else {
                mock_game::Quat rot{}, normalized{};
                const auto* r = step.find("rot");
                if (r) { for (size_t n = 0; n < 4; ++n) rot[n] = r->at(n).as_number(); mock_game::NormalizeQuat(rot.data(), normalized); }
                mock_game::MovePlayer(zone, Position(*step.find("pos")), r ? &normalized : nullptr);
            }
        }
        const bool ok = reason == nullptr;
        const auto* expected = step.find("expect");
        if (ok == (!expected || expected->as_string() == "ok")) ++passed;
        std::printf("SCENARIO step %zu %s%s%s\n", i + 1, ok ? "ok" : "refused", reason ? " " : "", reason ? reason : "");
    }
    std::printf("SCENARIO pass %zu/%zu\n", passed, steps.size());
    std::fflush(stdout);
    return passed == steps.size();
}
}  // namespace scuba::hostd
