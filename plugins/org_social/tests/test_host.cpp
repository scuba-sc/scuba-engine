// SPDX-License-Identifier: GPL-3.0-only
// org_social in the real host kit (sco::app), headless, the way scuba-hostd boots it: plugin root,
// a data root for sco.storage, and a synthetic game.spatial published as the game pack would.
// Three starts over one data root prove the roster, waypoints and settings persist.
//   org_social_host_test <staged plugin root> <scratch data dir>
#include "sco/app.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include "sco/widgets.h"
#include "sc_game_spatial.h"
#include "sco_widgets.h"
#include "sco_net.h"
extern "C" {
#include "os_model.h"
#include "os_wire.h"
}

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_passed = 0, g_failed = 0;
#define CHECK(c) do { if (c) ++g_passed; else { ++g_failed; std::printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

// Synthetic frames: zone z's origin sits at (1000 * z, 0, 0) in the world; zone 999 never resolves.
int FakePoseToZone(uint64_t from, uint64_t to, const double pos[3], const double rot[4], double outPos[3],
                   double outRot[4]) {
    if (from == 999 || to == 999) return 0;
    outPos[0] = pos[0] + 1000.0 * static_cast<double>(from) - 1000.0 * static_cast<double>(to);
    outPos[1] = pos[1];
    outPos[2] = pos[2];
    for (int k = 0; k < 4; ++k) outRot[k] = rot[k];
    return 1;
}

const sc_game_spatial_v1 kSpatial{ sizeof(sc_game_spatial_v1), FakePoseToZone, nullptr, nullptr, nullptr };
bool g_publishSpatial = false;

void SetCaps() {
    if (g_publishSpatial)
        CHECK(sco::host::ProvideGameService(SC_GAME_SPATIAL_NAME, SC_GAME_SPATIAL_VERSION, &kSpatial) == sco::Result::Ok);
}

void Sink(const char* line) { std::printf("%s\n", line); }

struct Channel {
    std::string name;
    uint32_t flags, maxLength;
    sco_net_on_message callback;
    void* context;
};
std::vector<Channel> g_channels;
std::vector<os_msg> g_sent;
bool g_fakeNetwork = false, g_remotePresent = true, g_peerFailure = false;
int FakeActive() { return 1; }
uint64_t FakeSelf() { return 1; }
sco_result FakePeers(sco_net_peer* out, uint32_t* count) {
    if (g_peerFailure) return SCO_UNAVAILABLE;
    const uint32_t needed = g_remotePresent ? 2u : 1u;
    if (*count < needed) { *count = needed; return SCO_TOO_MANY; }
    out[0] = {1, 0};
    if (g_remotePresent) out[1] = {2, 0};
    *count = needed;
    return SCO_OK;
}
sco_result FakeName(uint64_t peer, char* out, uint32_t cap) {
    std::snprintf(out, cap, "%s", peer == 1 ? "Local" : "Bravo");
    return SCO_OK;
}
sco_result FakeSend(sco_plugin*, const char*, const void* data, uint32_t len) {
    os_msg message{};
    if (!os_wire_decode(static_cast<const uint8_t*>(data), len, &message)) return SCO_BAD_ARG;
    g_sent.push_back(message);
    return SCO_OK;
}
sco_result FakeRegister(sco_plugin*, const char* channel, uint32_t flags, uint32_t limit,
                        sco_net_on_message callback, void* context) {
    g_channels.push_back({channel, flags, limit, callback, context});
    return SCO_OK;
}
sco_result FakeUnregister(sco_plugin*, const char*) { return SCO_OK; }
const sco_net_v1 kNetwork{sizeof(sco_net_v1), 0, FakeActive, FakePeers, FakeName, FakeSend,
                         FakeRegister, FakeUnregister, FakeSelf};
const sco_plugin_info kFixtureInfo{sizeof(sco_plugin_info), 1, 1, "netfixture", "1.0", "scubamount"};
const sco_plugin_info* FixtureQuery() { return &kFixtureInfo; }
sco_result FixtureLoad(const sco_api*, sco_plugin*) {
    CHECK(sco::host::WithdrawHostService(SCO_NET_NAME) == sco::Result::Ok);
    return sco::host::ProvideHostService(SCO_NET_NAME, SCO_NET_VERSION_1_0, &kNetwork) == sco::Result::Ok
        ? SCO_OK : SCO_FAILED;
}
void FixtureUnload() {}
const sco::plugins::Builtin kFixture{"netfixture", FixtureQuery, FixtureLoad, FixtureUnload};
void Deliver(const char* channel, const os_msg& message) {
    uint8_t bytes[OS_WIRE_MAX_ROSTER];
    const size_t size = os_wire_encode(&message, bytes, sizeof bytes);
    CHECK(size != 0);
    bool delivered = false;
    for (const auto& registered : g_channels) {
        if (registered.name != channel) continue;
        CHECK(size <= registered.maxLength);
        registered.callback(2, bytes, static_cast<uint32_t>(size), registered.context);
        delivered = true;
    }
    CHECK(delivered);
}

sco::Arg S(const char* s) { sco::Arg a{}; a.type = sco::ArgType::String; a.v.s = s; return a; }
sco::Arg I(int64_t i) { sco::Arg a{}; a.type = sco::ArgType::Int; a.v.i = i; return a; }
sco::Arg F(double f) { sco::Arg a{}; a.type = sco::ArgType::Float; a.v.f = f; return a; }
sco::Arg B(bool b) { sco::Arg a{}; a.type = sco::ArgType::Bool; a.v.i = b ? 1 : 0; return a; }

struct Reply { bool called = false; sco::Result r = sco::Result::Ok; std::string text; };
void Done(sco::Result r, const char* reply, void* ctx) {
    auto* d = static_cast<Reply*>(ctx);
    d->called = true;
    d->r = r;
    d->text = reply ? reply : "";
}

Reply Call(const char* name, std::vector<sco::Arg> args = {}) {
    Reply rep;
    (void)sco::Invoke(name, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()), Done, &rep);
    std::printf("  %s -> %s \"%s\"\n", name, sco::ResultName(rep.r), rep.text.c_str());
    return rep;
}

bool Ok(const Reply& r, const char* needle) { return r.called && r.r == sco::Result::Ok && r.text.find(needle) != std::string::npos; }

sco::Result Raw(const char* name, const std::string& in, std::string& out, uint32_t cap = 65536) {
    std::vector<char> buf(cap ? cap : 1);
    uint32_t size = cap;
    const sco::Result r = sco::InvokeRaw(nullptr, name, in.empty() ? nullptr : in.data(),
                                         static_cast<uint32_t>(in.size()), buf.data(), &size);
    out.assign(buf.data(), r == sco::Result::Ok ? size : 0);
    if (r == sco::Result::TooMany) out = std::to_string(size);
    return r;
}

bool Has(const std::string& doc, const char* needle) {
    const bool found = doc.find(needle) != std::string::npos;
    if (!found) std::printf("  missing %s\n  in %s\n", needle, doc.c_str());
    return found;
}

struct Headless final : sco::widgets::Renderer {
    std::vector<sco::widgets::Event> next;
    void Render(const sco::widgets::Panel&, std::vector<sco::widgets::Event>& events) override {
        events.insert(events.end(), next.begin(), next.end());
        next.clear();
    }
};

const sco::widgets::Node* PanelNode(uint32_t id) {
    static std::vector<sco::widgets::Panel> panels;
    panels = sco::widgets::Panels();
    for (const auto& p : panels)
        if (p.id == "org_social.roster")
            for (const auto& n : p.nodes)
                if (n.id == id) return &n;
    return nullptr;
}

bool TableHas(const char* cell) {
    const sco::widgets::Node* t = PanelNode(3);
    if (!t || t->kind != SCO_WIDGET_TABLE) return false;
    for (const auto& item : t->items)
        if (item == cell) return true;
    return false;
}

void RenderOne(Headless& r, uint32_t node, uint32_t kind, int32_t index, double value, const char* text) {
    sco::widgets::Event e;
    e.node_id = node; e.kind = kind; e.index = index; e.value = value; e.text = text ? text : "";
    r.next.push_back(e);
    CHECK(sco::widgets::RenderPanel("org_social.roster", r) == sco::Result::Ok);
}

bool Start(const std::filesystem::path& root, const std::filesystem::path& data, bool spatial) {
    g_publishSpatial = spatial;
    sco::app::Platform pf;
    pf.hostVersion = "org_social host test";
    pf.pluginRoot = root;
    pf.pluginsEnabled = true;
    pf.dataRoot = data;
    pf.setCapabilities = SetCaps;
    if (g_fakeNetwork) { pf.builtins = &kFixture; pf.nBuiltins = 1; }
    if (!sco::app::Start(pf)) return false;
    bool loaded = false;
    for (const auto& p : sco::app::Plugins()) {
        std::printf("  plugin %s\n", sco::plugins::Describe(p).c_str());
        if (p.manifestOk && p.manifest.id == "org_social") loaded = p.state == sco::plugins::State::Loaded;
    }
    return loaded;
}

void Stop() {
    sco::app::Stop();
    (void)sco::host::WithdrawGameServices();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: org_social_host_test <plugin root> <data dir>\n"); return 2; }
    const std::filesystem::path root = argv[1], data = argv[2];
    std::error_code ec;
    std::filesystem::remove_all(data, ec);
    std::filesystem::create_directories(data);
    sco::SetLogSink(Sink);
    std::string doc;

    std::printf("-- run 1: storage + synthetic game.spatial\n");
    CHECK(Start(root, data, true));
    for (uint32_t t = 1; t <= 3; ++t) sco::app::Tick(t * 100);
    CHECK(Ok(Call("org_social.member_add", { S("Alpha"), S("officer") }), "Alpha added as officer"));
    CHECK(Call("org_social.member_add", { S(""), S("member") }).r == sco::Result::BadArg);
    CHECK(Call("org_social.member_add", { S("Beta"), S("admiral") }).r == sco::Result::BadArg);
    CHECK(Ok(Call("org_social.waypoint_add", { S("Rally"), I(0), F(10), F(20), F(30) }), "waypoint 1 Rally"));
    CHECK(Ok(Call("org_social.waypoint_add", { S("Outpost"), I(2), F(5), F(0), F(0) }), "waypoint 2 Outpost"));
    CHECK(Ok(Call("org_social.waypoint_add", { S("Lost"), I(999), F(1), F(1), F(1) }), "waypoint 3 Lost"));
    CHECK(Call("org_social.waypoint_add", { S("Neg"), I(-1), F(0), F(0), F(0) }).r == sco::Result::BadArg);
    CHECK(Ok(Call("org_social.self_pose", { I(0), F(0), F(0), F(0) }), "pose set"));
    CHECK(Ok(Call("org_social.status", { S("busy") }), "status busy"));
    CHECK(Ok(Call("org_social.roster"), "org 1, online 0, party 0, waypoints 3 (no session)"));
    CHECK(Ok(Call("org_social.billboards"), "{\"schema\":\"org_social.billboards/1\""));

    CHECK(Raw("org_social.roster_json", "", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"schema\":\"org_social.roster/1\""));
    CHECK(Has(doc, "\"handle\":\"Alpha\",\"rank\":\"officer\",\"in_org\":true,\"online\":false"));
    CHECK(Has(doc, "\"status\":\"busy\""));
    CHECK(Raw("org_social.billboards_json", "0 0 0 -10", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"spatial\":\"game.spatial\""));
    CHECK(Has(doc, "\"label\":\"Rally\",\"color\":\"#4fc3f7\",\"zone\":0,\"resolved\":true,\"method\":\"same_frame\",\"pos\":[10,20,30]"));
    CHECK(Has(doc, "\"label\":\"Outpost\",\"color\":\"#4fc3f7\",\"zone\":2,\"resolved\":true,\"method\":\"pose_to_zone\",\"pos\":[2005,0,0]"));
    CHECK(Has(doc, "\"label\":\"Lost\",\"color\":\"#4fc3f7\",\"zone\":999,\"resolved\":false"));
    CHECK(Has(doc, "\"kind\":\"self\",\"id\":\"self\",\"label\":\"You\",\"color\":\"#81c784\",\"zone\":0,\"resolved\":true,\"method\":\"same_frame\",\"pos\":[0,0,0],\"distance_m\":10,"));
    CHECK(Raw("org_social.billboards_json", "zone nine", doc) == sco::Result::BadArg);
    CHECK(Raw("org_social.billboards_json", "", doc, 16) == sco::Result::TooMany && std::stoul(doc) > 16);

    Headless renderer;
    CHECK(PanelNode(3) && TableHas("Alpha") && TableHas("officer"));
    RenderOne(renderer, 6, SCO_WIDGET_TEXT_INPUT, 0, 0, "Bravo");
    RenderOne(renderer, 7, SCO_WIDGET_COMBO, 3, 0, nullptr);
    RenderOne(renderer, 8, SCO_WIDGET_BUTTON, 0, 0, nullptr);
    CHECK(TableHas("Bravo") && TableHas("director"));
    RenderOne(renderer, 11, SCO_WIDGET_TOGGLE, 0, 0.0, nullptr);
    CHECK(PanelNode(11) && PanelNode(11)->value == 0.0);
    CHECK(PanelNode(4) && PanelNode(4)->kind == SCO_WIDGET_LIST && PanelNode(4)->items.size() == 3);
    Stop();

    std::printf("-- run 2: same data root, no game.spatial\n");
    CHECK(Start(root, data, false));
    CHECK(Ok(Call("org_social.roster"), "org 2, online 0, party 0, waypoints 3"));
    CHECK(Raw("org_social.roster_json", "", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"share_waypoints\":false"));
    CHECK(Has(doc, "\"handle\":\"Bravo\",\"rank\":\"director\""));
    CHECK(Raw("org_social.billboards_json", "2", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"spatial\":\"unavailable\""));
    CHECK(Has(doc, "\"label\":\"Outpost\",\"color\":\"#4fc3f7\",\"zone\":2,\"resolved\":true,\"method\":\"same_frame\",\"pos\":[5,0,0]"));
    CHECK(Has(doc, "\"label\":\"Rally\",\"color\":\"#4fc3f7\",\"zone\":0,\"resolved\":false,\"method\":\"unresolved\""));
    CHECK(Ok(Call("org_social.member_remove", { S("Alpha") }), "Alpha removed"));
    CHECK(Call("org_social.member_remove", { S("Nobody") }).r == sco::Result::Failed);
    RenderOne(renderer, 4, SCO_WIDGET_LIST, 0, 0, nullptr);
    RenderOne(renderer, 12, SCO_WIDGET_BUTTON, 0, 0, nullptr);
    CHECK(PanelNode(4) && PanelNode(4)->items.size() == 2);
    CHECK(Call("org_social.waypoint_remove", { I(1) }).r == sco::Result::Failed);
    CHECK(Ok(Call("org_social.share", { B(true) }), "sharing on"));
    Stop();

    std::printf("-- run 3: removals persisted\n");
    CHECK(Start(root, data, false));
    CHECK(Ok(Call("org_social.roster"), "org 1, online 0, party 0, waypoints 2"));
    CHECK(Raw("org_social.roster_json", "", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"share_waypoints\":true") && doc.find("\"Alpha\"") == std::string::npos);
    Stop();

    std::printf("-- run 4: deterministic session callbacks over the real plugin loader\n");
    g_fakeNetwork = true;
    CHECK(Start(root, data, false));
    CHECK(g_channels.size() == 2 && g_channels[0].flags == SCO_NET_RELIABLE && g_channels[1].flags == 0);
    CHECK(Ok(Call("org_social.roster"), "org 1, online 1, party 1, waypoints 2"));
    sco::app::Tick(100);
    CHECK(!g_sent.empty() && g_sent[0].type == OS_MSG_HELLO);
    os_msg announcement{};
    announcement.type = OS_MSG_HELLO;
    announcement.status = OS_STATUS_AWAY;
    announcement.rank = OS_RANK_FOUNDER;
    std::snprintf(announcement.text, sizeof announcement.text, "%s", "Bravo");
    Deliver("org_social.roster", announcement);
    CHECK(Raw("org_social.roster_json", "", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"handle\":\"Bravo\",\"rank\":\"director\",\"in_org\":true,\"online\":true"));
    CHECK(Has(doc, "\"state\":\"away\",\"declared_rank\":\"founder\""));
    announcement.type = OS_MSG_WAYPOINT_SET;
    announcement.id = 7;
    announcement.zone = 0;
    announcement.pos[0] = 12;
    std::snprintf(announcement.text, sizeof announcement.text, "%s", "Shared");
    announcement.type = OS_MSG_WAYPOINT_DEL;
    Deliver("org_social.pose", announcement);
    CHECK(Ok(Call("org_social.roster"), "waypoints 2"));
    announcement.type = OS_MSG_WAYPOINT_SET;
    Deliver("org_social.roster", announcement);
    CHECK(Ok(Call("org_social.roster"), "waypoints 3"));
    announcement.type = OS_MSG_POSE;
    Deliver("org_social.pose", announcement);
    CHECK(Raw("org_social.billboards_json", "0", doc) == sco::Result::Ok);
    CHECK(Has(doc, "\"kind\":\"member\",\"id\":\"peer:2\""));
    CHECK(Has(doc, "\"label\":\"Shared\""));
    announcement.type = OS_MSG_WAYPOINT_DEL;
    Deliver("org_social.roster", announcement);
    CHECK(Ok(Call("org_social.roster"), "waypoints 2"));
    announcement.type = OS_MSG_WAYPOINT_SET;
    Deliver("org_social.roster", announcement);
    CHECK(Ok(Call("org_social.share", {B(false)}), "sharing off"));
    CHECK(!g_sent.empty() && g_sent.back().type == OS_MSG_WAYPOINT_DEL);
    g_peerFailure = true;
    CHECK(sco::Dispatch(SCO_NET_EVENT_PEER, nullptr) == sco::Result::Ok);
    CHECK(Ok(Call("org_social.roster"), "online 1, party 1"));
    g_peerFailure = false;
    g_remotePresent = false;
    CHECK(sco::Dispatch(SCO_NET_EVENT_PEER, nullptr) == sco::Result::Ok);
    CHECK(Ok(Call("org_social.roster"), "online 0, party 0, waypoints 2"));
    CHECK(Raw("org_social.billboards_json", "0", doc) == sco::Result::Ok);
    CHECK(doc.find("\"kind\":\"member\"") == std::string::npos);
    Stop();

    std::printf("org_social host: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
