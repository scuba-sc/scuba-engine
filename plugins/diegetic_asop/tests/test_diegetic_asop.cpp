// SPDX-License-Identifier: GPL-3.0-only
// diegetic_asop unit test: the terminal against a synthetic sco_api and a synthetic game.hangar
// table (no game, no host). Checks every gate (sco_api size, publication, has(), table size), the
// calls that reach the table, the last_error size handshake, the game.asop section (inert, gated
// and wired) and the panel's widget event path.
#include "terminal.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

struct Fake {
    std::set<std::string> caps;
    bool hangarPublished = true;
    bool asopPublished = false;
    uint32_t asopSize = sizeof(sc_asop_v1);
    sco_result asopResult = SCO_OK;
    int32_t claimSeconds = 30;
    uint32_t persisted = 1;
    uint32_t hangarSize = sizeof(sc_hangar_v1);
    sco_result nextResult = SCO_OK;
    std::string lastError;
    uint64_t liftForArea = 7;
    int queries = 0;
    std::vector<std::string> calls;
};

Fake g_fake;

int FakeHas(const char* capability) { return g_fake.caps.count(capability) ? 1 : 0; }

sco_result FakeFindLift(sco_plugin*, uint64_t area, uint64_t* out) {
    g_fake.calls.push_back("find " + std::to_string(area));
    if (g_fake.nextResult != SCO_OK) return g_fake.nextResult;
    *out = g_fake.liftForArea;
    return SCO_OK;
}

sco_result FakeLiftRequest(sco_plugin*, uint64_t lift, uint32_t op) {
    g_fake.calls.push_back("lift " + std::to_string(lift) + " op " + std::to_string(op));
    return g_fake.nextResult;
}

sco_result FakeAtcCancel(sco_plugin*, uint64_t atc) {
    g_fake.calls.push_back("atc " + std::to_string(atc));
    return g_fake.nextResult;
}

sco_result FakeLastError(sco_plugin*, char* out, uint32_t* inout) {
    const auto need = static_cast<uint32_t>(g_fake.lastError.size() + 1);
    if (*inout < need) {
        *inout = need;
        return SCO_TOO_MANY;
    }
    std::memcpy(out, g_fake.lastError.c_str(), need);
    *inout = need;
    return SCO_OK;
}

sc_hangar_v1 g_hangar;

sco_result FakeClaimTimeout(sco_plugin*, int32_t* out) {
    g_fake.calls.push_back("claim_timeout");
    if (g_fake.asopResult != SCO_OK) return g_fake.asopResult;
    *out = g_fake.claimSeconds;
    return SCO_OK;
}

sco_result FakeShardPersisted(sco_plugin*, uint32_t* out) {
    g_fake.calls.push_back("shard_persisted");
    if (g_fake.asopResult != SCO_OK) return g_fake.asopResult;
    *out = g_fake.persisted;
    return SCO_OK;
}

sc_asop_v1 g_asop;

sco_result FakeQuery(const char* name, uint32_t version, const void** out) {
    ++g_fake.queries;
    *out = nullptr;
    if (!std::strcmp(name, SC_HANGAR_SERVICE_NAME) && version == SC_HANGAR_SERVICE_VERSION) {
        if (!g_fake.hangarPublished) return SCO_NOT_FOUND;
        g_hangar.size = g_fake.hangarSize;
        *out = &g_hangar;
        return SCO_OK;
    }
    if (!std::strcmp(name, SC_ASOP_SERVICE_NAME) && version == SC_ASOP_SERVICE_VERSION && g_fake.asopPublished) {
        g_asop.size = g_fake.asopSize;
        *out = &g_asop;
        return SCO_OK;
    }
    return SCO_NOT_FOUND;
}

sco_api MakeApi(uint32_t size) {
    sco_api api{};
    api.size = size;
    api.major = 1;
    api.minor = size >= sizeof(sco_api) ? 1 : 0;
    api.has = FakeHas;
    api.query_service = FakeQuery;
    return api;
}

void Reset() {
    g_fake = Fake{};
    g_hangar = sc_hangar_v1{};
    g_hangar.lift_of_landing_area = FakeFindLift;
    g_hangar.lift_request = FakeLiftRequest;
    g_hangar.atc_cancel_request = FakeAtcCancel;
    g_hangar.last_error = FakeLastError;
    g_asop = sc_asop_v1{};
    g_asop.claim_timeout = FakeClaimTimeout;
    g_asop.shard_persisted = FakeShardPersisted;
    g_asop.last_error = FakeLastError;
}

bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

const sco_widget_node* NodeByLabel(scosdk::PanelTree& tree, const char* label, uint32_t kind) {
    for (uint32_t id = 1; id <= tree.Count(); ++id) {
        const sco_widget_node* n = tree.Find(id);
        if (n && n->kind == kind && n->label && !std::strcmp(n->label, label)) return n;
    }
    return nullptr;
}

bool AnyLabelContains(scosdk::PanelTree& tree, const char* part) {
    for (uint32_t id = 1; id <= tree.Count(); ++id) {
        const sco_widget_node* n = tree.Find(id);
        if (n && n->label && Contains(n->label, part)) return true;
    }
    return false;
}

bool Click(diegetic_asop::Terminal& t, const char* label) {
    const sco_widget_node* n = NodeByLabel(t.Tree(), label, SCO_WIDGET_BUTTON);
    if (!n) return false;
    sco_widget_event e{};
    e.size = sizeof e;
    e.node_id = n->id;
    e.kind = SCO_WIDGET_BUTTON;
    return t.Tree().Dispatch(e);
}

bool Type(diegetic_asop::Terminal& t, const char* label, const char* text) {
    const sco_widget_node* n = NodeByLabel(t.Tree(), label, SCO_WIDGET_TEXT_INPUT);
    if (!n) return false;
    sco_widget_event e{};
    e.size = sizeof e;
    e.node_id = n->id;
    e.kind = SCO_WIDGET_TEXT_INPUT;
    e.text = text;
    return t.Tree().Dispatch(e);
}

void TestParse() {
    uint64_t v = 0;
    CHECK(diegetic_asop::ParseEntityId("42", v) && v == 42);
    CHECK(diegetic_asop::ParseEntityId(" 0x2A ", v) && v == 42);
    CHECK(diegetic_asop::ParseEntityId("18446744073709551615", v) && v == UINT64_MAX);
    CHECK(!diegetic_asop::ParseEntityId("18446744073709551616", v));
    CHECK(!diegetic_asop::ParseEntityId("0", v));
    CHECK(!diegetic_asop::ParseEntityId("0x", v));
    CHECK(!diegetic_asop::ParseEntityId("-5", v));
    CHECK(!diegetic_asop::ParseEntityId("12abc", v));
    CHECK(!diegetic_asop::ParseEntityId("headless input", v));
}

void TestOneZeroHost() {
    Reset();
    g_fake.caps = {diegetic_asop::kCapLift};
    sco_api api = MakeApi(static_cast<uint32_t>(offsetof(sco_api, provide_service)));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.Refresh() == SCO_OK);
    const diegetic_asop::Outcome o = t.FindLift("42");
    CHECK(o.result == SCO_UNAVAILABLE);
    CHECK(Contains(o.message, "no services"));
    CHECK(g_fake.queries == 0);
    CHECK(g_fake.calls.empty());
    CHECK(!t.Asop().active);
}

void TestNotPublished() {
    Reset();
    g_fake.hangarPublished = false;
    g_fake.caps = {diegetic_asop::kCapLift, diegetic_asop::kCapAtcCancel};
    sco_api api = MakeApi(sizeof(sco_api));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.Refresh() == SCO_OK);
    CHECK(t.FindLift("1").result == SCO_UNAVAILABLE);
    CHECK(t.CancelAtc("1").result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "not published"));
    CHECK(g_fake.calls.empty());
    CHECK(Contains(t.Summary(), "game.hangar: not published; lift: off; atc_cancel: off; asop: inert"));
}

void TestCapabilityGates() {
    Reset();
    sco_api api = MakeApi(sizeof(sco_api));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.FindLift("1").result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "game.hangar.lift is off"));
    CHECK(t.Lift("1", SC_HANGAR_LIFT_OPEN).result == SCO_UNAVAILABLE);
    CHECK(t.CancelAtc("1").result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "game.hangar.atc_cancel is off"));
    CHECK(g_fake.calls.empty());

    g_fake.caps = {diegetic_asop::kCapAtcCancel};
    CHECK(t.FindLift("1").result == SCO_UNAVAILABLE);
    CHECK(t.CancelAtc("9").ok());
    CHECK(g_fake.calls.size() == 1 && g_fake.calls[0] == "atc 9");
}

void TestShortTable() {
    Reset();
    g_fake.caps = {diegetic_asop::kCapLift, diegetic_asop::kCapAtcCancel};
    g_fake.hangarSize = static_cast<uint32_t>(offsetof(sc_hangar_v1, lift_request));
    sco_api api = MakeApi(sizeof(sco_api));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.FindLift("5").ok());
    CHECK(t.Lift("7", SC_HANGAR_LIFT_OPEN).result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "has no lift_request"));
    CHECK(t.CancelAtc("3").result == SCO_UNAVAILABLE);
    CHECK(t.LastError().result == SCO_UNAVAILABLE);
    CHECK(g_fake.calls.size() == 1 && g_fake.calls[0] == "find 5");
    CHECK(Contains(t.Summary(), "lift: off; atc_cancel: off"));
}

void TestCallsAndErrors() {
    Reset();
    g_fake.caps = {diegetic_asop::kCapLift, diegetic_asop::kCapAtcCancel};
    sco_api api = MakeApi(sizeof(sco_api));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.Lift("0x10", SC_HANGAR_LIFT_CLOSE).ok());
    CHECK(g_fake.calls.back() == "lift 16 op 2");
    CHECK(t.Lift("0", SC_HANGAR_LIFT_OPEN).result == SCO_BAD_ARG);
    CHECK(g_fake.calls.size() == 1);

    g_fake.nextResult = SCO_NOT_FOUND;
    g_fake.lastError = std::string(300, 'x') + "\x01";
    const diegetic_asop::Outcome o = t.FindLift("77");
    CHECK(o.result == SCO_NOT_FOUND);
    CHECK(Contains(o.message, "lift_of_landing_area: not_found: xxxx"));
    CHECK(o.message.size() > 300);
    CHECK(t.Refresh() == SCO_OK);
    CHECK(AnyLabelContains(t.Tree(), "lift_of_landing_area: not_found"));

    g_fake.lastError = "landing area not streamed in";
    CHECK(t.LastError().ok());
    CHECK(t.Last().message == "game.hangar: landing area not streamed in");
}

void TestAsopSection() {
    Reset();
    sco_api api = MakeApi(sizeof(sco_api));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.Refresh() == SCO_OK);
    CHECK(!t.Asop().active);
    CHECK(t.Asop().query == SCO_NOT_FOUND);
    CHECK(!AnyLabelContains(t.Tree(), "ASOP fleet"));
    CHECK(t.ReadAsop().result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "game.asop 1.0 is not published"));

    // Published, both capabilities off (the game pack default): the section shows, nothing is called.
    g_fake.asopPublished = true;
    CHECK(t.Refresh() == SCO_OK);
    CHECK(t.Asop().active && !t.Asop().claimTimeout && !t.Asop().shardPersisted);
    CHECK(t.Asop().tableSize == sizeof(sc_asop_v1));
    CHECK(AnyLabelContains(t.Tree(), "ASOP fleet"));
    CHECK(Contains(t.Summary(), "asop: active (claim_timeout: off, shard_persisted: off)"));
    CHECK(t.ReadAsop().result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "game.asop.claim_timeout is off"));
    CHECK(g_fake.calls.empty());

    // One capability on: that read reaches the table, the other stays gated.
    g_fake.caps = {diegetic_asop::kCapShardPersisted};
    CHECK(t.ReadAsop().result == SCO_UNAVAILABLE);
    CHECK(g_fake.calls.size() == 1 && g_fake.calls[0] == "shard_persisted");
    CHECK(AnyLabelContains(t.Tree(), "Claim timeout: unavailable; shard persisted: yes"));

    g_fake.caps = {diegetic_asop::kCapClaimTimeout, diegetic_asop::kCapShardPersisted};
    g_fake.calls.clear();
    CHECK(t.Refresh() == SCO_OK);
    CHECK(Contains(t.Summary(), "claim_timeout: ready, shard_persisted: ready"));
    CHECK(Click(t, "Read ASOP"));
    CHECK(t.Last().ok() && t.Last().message == "claim timeout 30 s; shard persisted: yes");
    CHECK(g_fake.calls.size() == 2);
    CHECK(AnyLabelContains(t.Tree(), "Claim timeout: 30 s; shard persisted: yes"));

    // A failing read carries last_error's text.
    g_fake.asopResult = SCO_NOT_FOUND;
    g_fake.lastError = "insurance configuration not loaded";
    CHECK(t.ReadAsop().result == SCO_NOT_FOUND);
    CHECK(t.Last().message == "claim_timeout: not_found: insurance configuration not loaded");

    // A table too short for shard_persisted: gated by size, never called.
    g_fake.asopResult = SCO_OK;
    g_fake.asopSize = static_cast<uint32_t>(offsetof(sc_asop_v1, shard_persisted));
    g_fake.calls.clear();
    CHECK(t.Refresh() == SCO_OK);
    CHECK(t.Asop().claimTimeout && !t.Asop().shardPersisted);
    CHECK(t.ReadAsop().result == SCO_UNAVAILABLE);
    CHECK(Contains(t.Last().message, "has no shard_persisted"));
    CHECK(g_fake.calls.size() == 1 && g_fake.calls[0] == "claim_timeout");
}

void TestPanelEvents() {
    Reset();
    g_fake.caps = {diegetic_asop::kCapLift, diegetic_asop::kCapAtcCancel};
    g_fake.liftForArea = 4242;
    sco_api api = MakeApi(sizeof(sco_api));
    diegetic_asop::Terminal t;
    t.Attach(&api, nullptr, nullptr);
    CHECK(t.Refresh() == SCO_OK);
    CHECK(t.Tree().Count() > 10);
    CHECK(t.Tree().Stride() == sizeof(sco_widget_node_1_1));
    CHECK(scosdk::ValidateWidgetNodes(t.Tree().Nodes(), t.Tree().Count(), t.Tree().Stride()) == SCO_OK);

    CHECK(Type(t, "Landing area id", "0x99"));
    CHECK(Click(t, "Find lift"));
    CHECK(g_fake.calls.size() == 1 && g_fake.calls[0] == "find 153");
    const sco_widget_node* liftInput = NodeByLabel(t.Tree(), "Lift id", SCO_WIDGET_TEXT_INPUT);
    CHECK(liftInput && liftInput->text && !std::strcmp(liftInput->text, "4242"));

    CHECK(Click(t, "Open lift"));
    CHECK(Click(t, "Close lift"));
    CHECK(g_fake.calls.size() == 3 && g_fake.calls[1] == "lift 4242 op 1" && g_fake.calls[2] == "lift 4242 op 2");

    CHECK(Type(t, "ATC id", "headless input"));
    CHECK(Click(t, "Cancel ATC request"));
    CHECK(t.Last().result == SCO_BAD_ARG);
    CHECK(g_fake.calls.size() == 3);
    CHECK(AnyLabelContains(t.Tree(), "ATC 'headless input' is not a non-zero id"));
}

}  // namespace

int main() {
    TestParse();
    TestOneZeroHost();
    TestNotPublished();
    TestCapabilityGates();
    TestShortTable();
    TestCallsAndErrors();
    TestAsopSection();
    TestPanelEvents();
    if (g_failures) {
        std::printf("test_diegetic_asop: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_diegetic_asop: all checks passed\n");
    return 0;
}
