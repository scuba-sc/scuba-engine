// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/scenario_runner.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/app.h"
#undef NDEBUG
#include <cassert>
#include <thread>

int main() {
    using namespace scuba::hostd;
    ScenarioRunner runner;
    assert(!runner.SetProfile("all"));
    sco::app::Platform platform;
    platform.hostVersion = "scenario test";
    mock_game::Configure(platform);
    assert(sco::app::Start(platform));
    assert(runner.SetProfile("none"));
    for (const auto& cap : runner.Capabilities()) assert(!cap.ready);
    assert(!runner.SetProfile("invalid"));
    assert(runner.SetProfile("all"));
    assert(!runner.Capabilities().empty());
    assert(!runner.Services().empty());
    assert(!runner.SetCapability("not.a.capability", false));
    assert(runner.SetCapability("game.events.player_spawned", false));
    assert(!runner.SpawnPlayer());
    assert(runner.SetCapability("game.events.player_spawned", true));
    assert(runner.SpawnPlayer());
    assert(!runner.SetService("not.a.service", false));
    assert(runner.SetService("game.actors", false));
    assert(runner.SetService("game.actors", true));
    auto count = runner.EntityCount();
    assert(runner.AddEntity("Synthetic_Actor", true) != 0);
    assert(runner.EntityCount() == count + 1);
    assert(runner.AddEntity("invalid class", true) == 0);
    bool workerResult = true;
    std::thread worker([&] { workerResult = runner.SetProfile("none"); });
    worker.join();
    assert(!workerResult);
    sco::app::Stop();
    mock_game::Unpublish();
    assert(runner.Capabilities().empty());
    assert(runner.AddEntity("Synthetic_Actor", true) == 0);
}
