#include "hostd/reserved_mock_host.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace scuba::hostd {
struct ReservedMockHostTestAccess {
    static bool Abandon(ReservedMockHost& h) { return h.StartImpl(true); }
    static sco::reserved::Session& Session(ReservedMockHost& h) { return h.session_; }
    static const sco::reserved::MockTable* Table(ReservedMockHost& h) { return h.table_; }
};
}
namespace {
using namespace scuba::hostd;
using namespace sco::reserved;
void Require(bool condition, const char* description) {
    if (!condition) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
Json Call(const Registry& registry, const char* text) {
    auto response = Json::parse(registry.dispatch_text(text, {}));
    Require(response.has_value(), "response parses");
    return *response;
}
const Json& Result(const Json& response) {
    const auto* result = response.find("result");
    Require(result != nullptr, "RPC result");
    return *result;
}
void Normal() {
    auto& host = ReservedMockHost::Instance();
    const Registry& registry = host.registry();
    Require(host.Start(), "startup");
    Require(host.phase() == Phase::Running, "running");
    auto status = Call(registry, R"({"id":0,"method":"mock.status"})");
    const auto& info = Result(status);
    Require(info.find("nativeUnavailable")->as_bool() && info.find("mockOnly")->as_bool() &&
            info.find("scalarTickOnly")->as_bool(), "bounded status");
    Require(info.find("coreSource")->as_string() == SCUBA_RESERVED_CORE_COMMIT &&
            info.find("bootstrapVersion")->as_number() == kBootstrapVersion &&
            info.find("mockVersion")->as_number() == kMockVersion &&
            info.find("graphVersion")->as_number() == kMockGraphVersion, "status versions and source");
    auto put = Call(registry, R"({"id":1,"method":"mock.entity_put","params":{"id":"9007199254740993","zone":"1","position":[1.25,-2,3],"flags":9,"name":"Diver"}})");
    Require(Result(put).find("id")->as_string() == "9007199254740993", "64 bit id preserved");
    auto read = Call(registry, R"({"id":2,"method":"mock.entity_get","params":{"id":"9007199254740993"}})");
    Require(Result(read).find("name")->as_string() == "Diver", "entity readback");
    Require(Result(read).find("position")->at(0).as_number() == 1.25, "entity position readback");
    auto graph = Call(registry, R"({"id":3,"method":"mock.graph"})");
    Require(Result(graph).find("zones")->size() == 1 && Result(graph).find("entities")->size() == 1, "graph copies");
    auto bad = Call(registry, R"({"id":4,"method":"mock.entity_put","params":{"id":2,"zone":7,"position":[0,0,0],"flags":0,"name":"bad zone"}})");
    Require(bad.find("error") != nullptr, "invalid zone refused");
    Require(Result(Call(registry, R"({"id":5,"method":"mock.graph"})")).find("entities")->size() == 1, "refusal preserves graph");
    Require(Result(Call(registry, R"({"id":6,"method":"mock.scalar_set","params":{"value":"-42"}})")).as_bool(), "scalar set");
    Require(Result(Call(registry, R"({"id":7,"method":"mock.scalar_tick","params":{"amount":8}})")).as_bool(), "scalar tick");
    auto scalar = Call(registry, R"({"id":8,"method":"mock.scalar_snapshot"})");
    Require(Result(scalar).find("value")->as_string() == "-42" && Result(scalar).find("ticks")->as_string() == "8", "scalar snapshot");
    Require(Result(Call(registry, R"({"id":10,"method":"mock.entity_put","params":{"id":"9007199254740993","zone":1,"position":[4,5,6],"flags":1,"name":"Updated"}})")).find("name")->as_string() == "Updated", "entity replacement");
    Require(Result(Call(registry, R"({"id":11,"method":"mock.entity_remove","params":{"id":"9007199254740993"}})")).as_bool(), "entity removal");
    Require(Result(Call(registry, R"({"id":12,"method":"mock.graph"})")).find("entities")->size() == 0, "removed from graph");
    Require(Result(Call(registry, R"({"id":13,"method":"mock.entity_put","params":{"id":"9007199254740993","zone":1,"position":[1,2,3],"flags":0,"name":"Diver"}})")).find("name")->as_string() == "Diver", "entity reinserted");
    auto* table = ReservedMockHostTestAccess::Table(host);
    Require(table != nullptr && host.Close() == sco::Result::Ok, "close");
    MockEntity sentinel{}; sentinel.id = 77; std::memcpy(sentinel.name, "unchanged", 10);
    Require(table->get_entity(table->context, 9007199254740993ULL, &sentinel) == sco::Result::Unavailable &&
            sentinel.id == 77 && std::strcmp(sentinel.name, "unchanged") == 0, "cached table refusal leaves output unchanged");
    int64_t value = -9; uint64_t ticks = 55;
    Require(table->snapshot(table->context, &value, &ticks) == sco::Result::Unavailable && value == -9 && ticks == 55,
            "closed scalar outputs unchanged");
    Require(Call(registry, R"({"id":9,"method":"mock.entity_get","params":{"id":"9007199254740993"}})").find("error") != nullptr,
            "cached registry refuses after close");
    Require(host.RetireUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)) == RetireResult::QuiescedRetained,
            "quiesced retained");
    Require(host.phase() == Phase::QuiescedRetained && !host.Start(), "terminal restart refused");
}
void Abandon() {
    auto& host = ReservedMockHost::Instance();
    Require(!ReservedMockHostTestAccess::Abandon(host), "startup abandoned");
    Require(host.phase() == Phase::FailedRetained && !host.Start(), "abandoned retained and restart refused");
    Require(Call(host.registry(), R"({"id":1,"method":"mock.scalar_snapshot"})").find("error") != nullptr, "unpublished registry refuses");
    Require(host.RetireUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)) == RetireResult::FailedRetained,
            "abandon retire retains failure");
}
void SelfRetire() {
    auto& host = ReservedMockHost::Instance();
    Require(host.Start(), "startup");
    CallPin pin;
    Require(TryEnter(ReservedMockHostTestAccess::Session(host), pin) == sco::Result::Ok, "actual live pin");
    Require(host.Close() == sco::Result::Ok, "close with live pin");
    Require(host.RetireUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)) == RetireResult::FailedRetained,
            "self retirement fails retained");
    Require(host.phase() == Phase::FailedRetained && !host.Start(), "failure retained, restart refused");
    Require(Call(host.registry(), R"({"id":1,"method":"mock.scalar_snapshot"})").find("error") != nullptr, "failed registry refuses");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "normal") Normal();
    else if (mode == "abandon") Abandon();
    else if (mode == "self_retire") SelfRetire();
    else return 2;
    return 0;
}
