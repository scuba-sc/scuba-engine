// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include "scuba/sco/sco_host.h"
#include "sco/runtime.h"
#include <cstring>
#include <thread>

namespace {
uint32_t mode = 0;
sco::Result Snapshot(const void*, uint32_t, void* out, uint32_t* size, void*) {
    if (mode == 4) { *size = 0; return sco::Result::Ok; }
    if (mode == 1) { *size = 131073; return sco::Result::TooMany; }
    const char bytes[] = "Synthetic_Ship\n";
    const uint32_t count = sizeof(bytes) - 1;
    if (*size < count) { *size = count; return sco::Result::TooMany; }
    std::memcpy(out, bytes, count);
    *size = mode == 2 ? count - 1 : count;
    if (mode == 3) static_cast<char*>(out)[0] = '\0';
    return sco::Result::Ok;
}
}
int main() {
    scuba::ScoHost host;
    std::string selection = "unchanged";
    CHECK(!host.ReadAsopSelection(selection) && selection == "unchanged");
    CHECK(host.StorageFiles().empty());
    CHECK(host.Start({}));
    bool workerRefused = false;
    std::thread worker([&] { workerRefused = !host.InvokeControl("sco.mock.revoke"); });
    worker.join();
    CHECK(workerRefused);
    CHECK(!host.ReadAsopSelection(selection) && selection == "unchanged");
    const char owner = 0;
    CHECK(sco::RegisterRaw(&owner, "asop_selector", "asop_selector.snapshot", nullptr, Snapshot, nullptr) == sco::Result::Ok);
    CHECK(host.ReadAsopSelection(selection) && selection == "Synthetic_Ship\n");
    for (mode = 1; mode <= 3; ++mode)
        CHECK(!host.ReadAsopSelection(selection) && selection == "Synthetic_Ship\n");
    mode = 4;
    CHECK(host.ReadAsopSelection(selection) && selection.empty());
    CHECK(host.InvokeControl("sco.mock.revoke"));
    CHECK(!host.InvokeControl("sco.mock.fire_respawn_loadout"));
    CHECK(!host.InvokeControl("sco.mock.fire_mission_changed"));
    for (const auto& capability : host.MockCapabilities()) CHECK(!capability.ready);
    CHECK(host.InvokeControl("sco.mock.admit"));
    CHECK(host.InvokeControl("sco.mock.fire_respawn_loadout"));
    CHECK(host.InvokeControl("sco.mock.fire_mission_changed"));
    const auto capabilities = host.MockCapabilities();
    CHECK(!capabilities.empty());
    for (const auto& capability : capabilities) CHECK(capability.ready);
    host.Stop();
    CHECK(!host.InvokeControl("sco.mock.admit"));
    CHECK(host.MockCapabilities().empty());
    CHECK(host.StorageFiles().empty());
    CHECK(host.Start({}));
    CHECK(host.InvokeControl("sco.mock.revoke"));
    CHECK(host.InvokeControl("sco.mock.admit"));
    host.Stop();
    return 0;
}
