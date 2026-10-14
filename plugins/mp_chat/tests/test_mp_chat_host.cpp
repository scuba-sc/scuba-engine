// SPDX-License-Identifier: GPL-3.0-only
// mp_chat on the real host kit (sco::app), headless, the way scuba-hostd's kernel boots it.
//
//   mp_chat_host_test single <plugin root> <work dir>
//       commands, the sco.widgets 1.1 panel (TEXT_INPUT, paged LIST + SCO_WIDGET_EVENT_PAGE,
//       severity), room switching and the sco.storage key-value state across a restart.
//   mp_chat_host_test pair <plugin root> <work dir> <path of this executable>
//       two peers over sco.net on 127.0.0.1: this process hosts, a child process ("join") joins;
//       each sends one message and waits for the other's, then the host restarts and reads the
//       exchange back from sco.storage.
// Waits are bounded by deadlines and driven by sco::net::WaitForDelivery, never fixed sleeps.
#include "sco/app.h"
#include "sco/log.h"
#include "sco/net/session.h"
#include "sco/runtime.h"
#include "sco/widgets.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define MP_POPEN _popen
#define MP_PCLOSE _pclose
#else
#define MP_POPEN popen
#define MP_PCLOSE pclose
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
uint32_t g_now = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

constexpr const char* kPanel = "mp_chat.main";
constexpr const char* kPassphrase = "scuba loopback chat passphrase";
constexpr uint32_t kStatus = 2, kRoom = 4, kClear = 5, kHistory = 6, kInput = 8, kSend = 9;

void Sink(const char* line) {
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
}

struct Reply {
    bool called = false;
    sco::Result r = sco::Result::Failed;
    std::string text;
};

void OnDone(sco::Result r, const char* text, void* ctx) {
    auto* reply = static_cast<Reply*>(ctx);
    reply->called = true;
    reply->r = r;
    reply->text = text ? text : "";
}

Reply Run(const char* name, const char* arg = nullptr) {
    Reply reply;
    sco::Arg a{};
    a.type = sco::ArgType::String;
    a.v.s = arg;
    sco::Invoke(name, arg ? &a : nullptr, arg ? 1u : 0u, OnDone, &reply);
    return reply;
}

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void TickOnce() {
    g_now += 16;
    sco::app::Tick(g_now);
}

template <class Pred>
bool Pump(Pred done, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        TickOnce();
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        (void)sco::net::WaitForDelivery(50);
    }
}

bool StartHost(const fs::path& root, const fs::path& data) {
    sco::app::Platform pf;
    pf.hostVersion = "mp_chat_host_test";
    pf.pluginRoot = root;
    pf.pluginsEnabled = true;
    pf.dataRoot = data;
    if (!sco::app::Start(pf)) return false;
    bool loaded = false;
    for (const auto& p : sco::app::Plugins()) {
        if (p.folder != "mp_chat") continue;
        loaded = p.state == sco::plugins::State::Loaded;
        if (!loaded) std::fprintf(stderr, "mp_chat not loaded: %s\n", sco::plugins::Describe(p).c_str());
    }
    TickOnce();
    return loaded;
}

class Scripted final : public sco::widgets::Renderer {
public:
    std::vector<sco::widgets::Event> queue;
    void Render(const sco::widgets::Panel&, std::vector<sco::widgets::Event>& out) override {
        out.insert(out.end(), queue.begin(), queue.end());
        queue.clear();
    }
};

bool Panel(sco::widgets::Panel& out) {
    for (auto& p : sco::widgets::Panels())
        if (p.id == kPanel) {
            out = p;
            return true;
        }
    return false;
}

const sco::widgets::Node* Find(const sco::widgets::Panel& p, uint32_t id) {
    for (const auto& n : p.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

// Delivers one event through sco::widgets::RenderPanel, then ticks so the plugin rebuilds.
void Deliver(sco::widgets::Event ev) {
    Scripted r;
    r.queue.push_back(std::move(ev));
    CHECK(sco::widgets::RenderPanel(kPanel, r) == sco::Result::Ok);
    TickOnce();
}

sco::widgets::Event MakeEvent(uint32_t node, uint32_t kind) {
    sco::widgets::Event ev;
    ev.node_id = node;
    ev.kind = kind;
    return ev;
}

int Single(const fs::path& root, const fs::path& work) {
    CHECK(StartHost(root, work / "data"));
    Reply st = Run("mp_chat.status");
    CHECK(st.r == sco::Result::Ok);
    CHECK(Has(st.text, "room=general") && Has(st.text, "session=off") && Has(st.text, "net=yes") &&
          Has(st.text, "storage=yes") && Has(st.text, "widgets=1.1"));

    sco::widgets::Panel p;
    CHECK(Panel(p));
    CHECK(p.nodes.size() == 9u);
    const auto* list = Find(p, kHistory);
    CHECK(list && list->kind == SCO_WIDGET_LIST && list->extended && list->visible_rows == 8u);
    const auto* input = Find(p, kInput);
    CHECK(input && input->kind == SCO_WIDGET_TEXT_INPUT);
    const auto* send = Find(p, kSend);
    CHECK(send && send->kind == SCO_WIDGET_BUTTON && (send->flags & SCO_WIDGET_DISABLED));

    sco::widgets::Event typed = MakeEvent(kInput, SCO_WIDGET_TEXT_INPUT);
    typed.text = "draft one";
    Deliver(typed);

    Reply sent = Run("mp_chat.send", "hello");
    CHECK(sent.r == sco::Result::Unavailable && Has(sent.text, "not in a session"));
    CHECK(Run("mp_chat.send", "bad\nline").r == sco::Result::BadArg);
    TickOnce();
    CHECK(Panel(p));
    const auto* status = Find(p, kStatus);
    CHECK(status && status->severity == SCO_WIDGET_SEVERITY_WARNING && Has(status->label, "not in a multiplayer session"));
    input = Find(p, kInput);
    CHECK(input && input->text == "draft one");

    for (int i = 0; i < 10; ++i) (void)Run("mp_chat.send", "more");
    TickOnce();
    CHECK(Has(Run("mp_chat.status").text, "lines=11"));
    CHECK(Panel(p));
    list = Find(p, kHistory);
    CHECK(list && list->items.size() == 11u && list->first_row == 3u && list->severity == SCO_WIDGET_SEVERITY_WARNING);
    CHECK(list && Has(list->items[0], "[warn] "));

    sco::widgets::Event page = MakeEvent(kHistory, SCO_WIDGET_EVENT_PAGE);
    page.row = 0;
    Deliver(page);
    (void)Run("mp_chat.send", "more");
    TickOnce();
    CHECK(Panel(p));
    list = Find(p, kHistory);
    CHECK(list && list->first_row == 0u && list->items.size() == 12u);

    page.row = 4;
    Deliver(page);
    (void)Run("mp_chat.send", "more");
    TickOnce();
    CHECK(Panel(p));
    list = Find(p, kHistory);
    CHECK(list && list->first_row == 5u);

    page.row = 99;
    Deliver(page);
    CHECK(Panel(p));
    list = Find(p, kHistory);
    CHECK(list && list->first_row == 5u);

    Reply joined = Run("mp_chat.join", "lobby");
    CHECK(joined.r == sco::Result::Ok && Has(joined.text, "#lobby"));
    CHECK(Run("mp_chat.join", "Not A Room").r == sco::Result::BadArg);
    TickOnce();
    CHECK(Panel(p));
    const auto* combo = Find(p, kRoom);
    CHECK(combo && combo->items.size() == 2u && combo->index == 1);

    sco::widgets::Event pick = MakeEvent(kRoom, SCO_WIDGET_COMBO);
    pick.index = 0;
    Deliver(pick);
    CHECK(Has(Run("mp_chat.status").text, "room=general"));

    Deliver(MakeEvent(kClear, SCO_WIDGET_BUTTON));
    CHECK(Has(Run("mp_chat.status").text, "lines=0"));

    CHECK(Run("mp_chat.join", "lobby").r == sco::Result::Ok);
    sco::app::Stop();
    CHECK(StartHost(root, work / "data"));
    CHECK(Has(Run("mp_chat.status").text, "room=lobby"));
    sco::app::Stop();
    return 0;
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

int Join(const fs::path& root, const fs::path& work, uint16_t port) {
    CHECK(StartHost(root, work / "data"));
    sco::net::JoinOptions j;
    j.address = "127.0.0.1";
    j.port = port;
    j.passphrase = kPassphrase;
    j.playerName = "peer-b";
    CHECK(sco::net::Join(j) == sco::Result::Ok);
    CHECK(Pump([] {
        const std::string s = Run("mp_chat.status").text;
        return Has(s, "session=on") && Has(s, "peers=2");
    }, 60000));
    CHECK(Run("mp_chat.send", "hello from b").r == sco::Result::Ok);
    CHECK(Pump([] { return Run("mp_chat.last").text == "peer-a: hello from a"; }, 60000));
    sco::net::Leave("test done");
    CHECK(Pump([] { return sco::net::GetState() == sco::net::State::Idle; }, 10000));
    sco::app::Stop();
    if (g_failures) return 1;
    std::printf("CHILD OK\n");
    std::fflush(stdout);
    return 0;
}

int Pair(const fs::path& root, const fs::path& work, const std::string& self) {
    CHECK(StartHost(root, work / "host"));
    sco::net::HostOptions h;
    h.port = 0;
    h.passphrase = kPassphrase;
    h.playerName = "peer-a";
    h.maxPeers = 2;
    CHECK(sco::net::Host(h) == sco::Result::Ok);
    CHECK(Pump([] { return Has(Run("mp_chat.status").text, "session=on"); }, 30000));
    const uint16_t port = sco::net::BoundPort();
    CHECK(port != 0);
    if (g_failures) {
        sco::app::Stop();
        return 1;
    }

    std::string cmd = Quote(self) + " join " + Quote(root.string()) + " " + Quote((work / "join").string()) + " " +
                      std::to_string(port);
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    std::FILE* child = MP_POPEN(cmd.c_str(), "r");
    CHECK(child != nullptr);
    if (child) {
        CHECK(Pump([] { return Run("mp_chat.last").text == "peer-b: hello from b"; }, 90000));
        CHECK(Run("mp_chat.send", "hello from a").r == sco::Result::Ok);
        CHECK(Pump([] { return Has(Run("mp_chat.status").text, "peers=1"); }, 90000));
        std::string out;
        char line[512];
        while (std::fgets(line, sizeof line, child)) out += line;
        CHECK(MP_PCLOSE(child) == 0);
        CHECK(Has(out, "CHILD OK"));
    }
    sco::net::Leave("test done");
    sco::app::Stop();

    CHECK(StartHost(root, work / "host"));
    CHECK(Run("mp_chat.last").text == "peer-a: hello from a");
    CHECK(Has(Run("mp_chat.status").text, "lines=2"));
    sco::app::Stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: mp_chat_host_test single|pair|join <plugin root> <work dir> [exe|port]\n");
        return 2;
    }
    sco::SetLogSink(Sink);
    const std::string mode = argv[1];
    const fs::path root = argv[2];
    const fs::path work = argv[3];
    if (mode != "join") {
        std::error_code ec;
        fs::remove_all(work, ec);
    }
    fs::create_directories(work);
    if (mode == "single") {
        (void)Single(root, work);
    } else if (mode == "pair" && argc >= 5) {
        (void)Pair(root, work, argv[4]);
    } else if (mode == "join" && argc >= 5) {
        return Join(root, work, static_cast<uint16_t>(std::strtoul(argv[4], nullptr, 10)));
    } else {
        return 2;
    }
    if (g_failures) {
        std::fprintf(stderr, "mp_chat_host_test %s: %d check(s) failed\n", mode.c_str(), g_failures);
        return 1;
    }
    std::printf("mp_chat_host_test %s: all checks passed\n", mode.c_str());
    return 0;
}
