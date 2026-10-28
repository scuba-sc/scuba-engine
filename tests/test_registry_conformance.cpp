#include "hostd/json.h"
#include "hostd/mock_game/mock_game.h"
#include "sco/app.h"
#include "sco/host.h"
#include "sc_datacore_live.h"
#include "sc_entities.h"
#include "sc_inventory.h"

#include <cstddef>
#include "check.h"

#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>

namespace mg = scuba::hostd::mock_game;
using scuba::hostd::Json;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) { std::fprintf(stderr, "cannot open registry: %s\n", argv[1]); return 2; }
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::string error;
    const auto registry = Json::parse(text, &error);
    if (!registry || !registry->is_object()) {
        std::fprintf(stderr, "invalid registry: %s\n", error.c_str()); return 2;
    }
    const Json* entries = registry->find("services");
    if (!entries || !entries->is_array()) { std::fprintf(stderr, "registry services missing\n"); return 2; }
    std::map<std::string, unsigned> versions;
    for (size_t i = 0; i < entries->size(); ++i) {
        const auto& entry = entries->at(i);
        const auto* kind = entry.find("kind");
        if (!kind || !kind->is_string()) { std::fprintf(stderr, "registry entry missing kind\n"); return 2; }
        if (kind->as_string() != "service") continue;
        const auto* name = entry.find("service");
        const auto* ver = entry.find("version");
        unsigned major = 0, minor = 0; char extra = 0;
        if (!name || !name->is_string() || name->as_string().empty() || !ver || !ver->is_string() ||
            std::sscanf(ver->as_string().c_str(), "%u.%u%c", &major, &minor, &extra) != 2 ||
            major == 0 || major > 65535 || minor > 65535 ||
            !versions.emplace(name->as_string(), (major << 16) | minor).second) {
            std::fprintf(stderr, "invalid or duplicate registry service at index %zu\n", i); return 2;
        }
    }
    if (versions.empty()) { std::fprintf(stderr, "registry contains no services\n"); return 2; }
    sco::app::Platform platform;
    platform.hostVersion = "registry conformance test";
    platform.pluginsEnabled = false;
    mg::Configure(platform);
    if (!sco::app::Start(platform)) { std::fprintf(stderr, "mock host start failed\n"); return 2; }
    const sco_api* api = sco::host::BuildApi({platform.hostVersion});
    const auto services = mg::Services();
    CHECK(!services.empty());
    std::set<std::string> seen;
    for (const auto& service : services) {
        CHECK(seen.insert(service.name).second);
        CHECK(service.published);
        const auto it = versions.find(service.name);
        if (it == versions.end()) {
            std::fprintf(stderr, "mock service absent from registry: %s\n", service.name.c_str());
            CHECK(false);
            continue;
        }
        if (service.version != it->second)
            std::fprintf(stderr, "%s: mock %u.%u, registry %u.%u\n", service.name.c_str(),
                         service.version >> 16, service.version & 0xffff, it->second >> 16, it->second & 0xffff);
        CHECK(service.version == it->second);
        const void* table = nullptr;
        CHECK(api->query_service(service.name.c_str(), service.version, &table) == SCO_OK && table);
        table = nullptr;
        const auto result = api->query_service(service.name.c_str(), it->second, &table);
        CHECK(result == SCO_OK && table);
        table = reinterpret_cast<const void*>(1);
        CHECK(api->query_service(service.name.c_str(), service.version + 1, &table) == SCO_UNAVAILABLE && !table);
    }
    for (const auto& [name, version] : versions) {
        (void)version;
        if (!seen.count(name)) {
            std::fprintf(stderr, "registry service not mocked: %s\n", name.c_str());
            CHECK(false);
        }
    }
    // Published tables must refuse operations for which the mock has no producer.
    const void* raw = nullptr;
    auto queried = api->query_service(SC_INVENTORY_SERVICE_NAME, SC_INVENTORY_SERVICE_VERSION_1_2, &raw);
    CHECK(queried == SCO_OK && raw);
    if (queried == SCO_OK && raw) {
        const auto* inventory = static_cast<const sc_inventory_v1_2*>(raw);
        CHECK(inventory->base.size >= offsetof(sc_inventory_v1_2, snapshot_layout) + sizeof(inventory->snapshot_layout));
        if (inventory->base.size >= offsetof(sc_inventory_v1_2, snapshot_layout) + sizeof(inventory->snapshot_layout)) CHECK(inventory->snapshot_layout);
        if (inventory->base.size >= offsetof(sc_inventory_v1_2, snapshot_layout) + sizeof(inventory->snapshot_layout) && inventory->snapshot_layout) {
            sc_inventory_layout_snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            sc_inventory_layout_entry entry{}; entry.size = sizeof(entry);
            CHECK(inventory->snapshot_layout(nullptr, &snapshot, &entry, sizeof(entry), 1) == SCO_UNAVAILABLE);
        }
    }
    raw = nullptr;
    queried = api->query_service(SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_2, &raw);
    CHECK(queried == SCO_OK && raw);
    if (queried == SCO_OK && raw) {
        const auto* entities = static_cast<const sc_entities_v1*>(raw);
        CHECK(entities->size >= offsetof(sc_entities_v1, find_by_name) + sizeof(entities->find_by_name));
        if (entities->size >= offsetof(sc_entities_v1, find_by_name) + sizeof(entities->find_by_name)) CHECK(entities->find_by_name);
        if (entities->size >= offsetof(sc_entities_v1, find_by_name) + sizeof(entities->find_by_name) && entities->find_by_name) {
            uint64_t id = 42;
            CHECK(entities->find_by_name("example", &id) == SCO_UNAVAILABLE);
            CHECK(id == 0);
        }
    }
    raw = nullptr;
    queried = api->query_service(SC_DATACORE_LIVE_SERVICE_NAME, SC_DATACORE_LIVE_SERVICE_VERSION, &raw);
    CHECK(queried == SCO_OK && raw);
    if (queried == SCO_OK && raw) {
        const auto* datacore = static_cast<const sc_datacore_live_v1*>(raw);
        CHECK(datacore->size >= offsetof(sc_datacore_live_v1, write_pool) + sizeof(datacore->write_pool));
        if (datacore->size >= offsetof(sc_datacore_live_v1, write_pool) + sizeof(datacore->write_pool)) CHECK(datacore->write_pool);
        if (datacore->size >= offsetof(sc_datacore_live_v1, write_pool) + sizeof(datacore->write_pool) && datacore->write_pool) {
            const uint32_t value = 123;
            size_t written = 42;
            CHECK(datacore->write_pool(nullptr, SC_DC_POOL_UINT32, 0, &value, sizeof(value), &written) == SCO_BAD_ARG);
            CHECK(written == 0);
        }
    }
    sco::app::Stop();
    mg::Unpublish();
    return CheckExit("test_registry_conformance");
}
