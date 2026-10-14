// End to end for the sco.widgets bridge: starts the real scuba-hostd (argv[1]) headless with the
// kit's sdk/examples/widgets and widgets_lua plugins (argv[2], built by build_widget_plugins.cmake),
// receives both panels over the WebSocket, sends BUTTON and SLIDER events and checks that each
// plugin saw them (its own log lines) and that the host snapshot changed (widgets.snapshot).
#include "check.h"
#include "hostd/json.h"
#include "hostd/ws.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#define SCUBA_POPEN _popen
#define SCUBA_PCLOSE _pclose
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#define SCUBA_POPEN popen
#define SCUBA_PCLOSE pclose
#endif

using namespace scuba::hostd;

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
const SocketHandle kBad = INVALID_SOCKET;
void CloseSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
const SocketHandle kBad = -1;
void CloseSocket(SocketHandle s) { close(s); }
#endif

const uint8_t kMask[4] = { 0x5a, 0x17, 0xc3, 0x09 };

// One WebSocket client. Every receive is bounded by a 10 s socket timeout, so a missing answer or
// event fails the test instead of hanging it.
class Session {
public:
    explicit Session(uint16_t port) : reader_(false, 1 << 22) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == kBad) return;
#ifdef _WIN32
        DWORD timeoutMs = 10000;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
#else
        timeval tv{};
        tv.tv_sec = 10;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        auto* p = reinterpret_cast<uint8_t*>(&sin.sin_port);
        p[0] = static_cast<uint8_t>(port >> 8);
        p[1] = static_cast<uint8_t>(port & 0xFFu);
        inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
        if (connect(sock_, reinterpret_cast<const sockaddr*>(&sin), sizeof sin) != 0) {
            CloseSocket(sock_);
            sock_ = kBad;
            return;
        }
        Send("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
             "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        for (;;) {
            const size_t end = buffer_.find("\r\n\r\n");
            if (end != std::string::npos) {
                upgraded_ = buffer_.rfind("HTTP/1.1 101", 0) == 0;
                buffer_.erase(0, end + 4);
                break;
            }
            if (!ReadMore()) break;
        }
    }
    ~Session() {
        if (sock_ != kBad) CloseSocket(sock_);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool ok() const { return sock_ != kBad && upgraded_; }

    Json Call(const std::string& method, Json params) {
        Json request = Json::MakeObject();
        const double id = ++nextId_;
        request.set("id", id);
        request.set("method", method);
        request.set("params", std::move(params));
        if (!Send(WsEncodeFrame(WsOpcode::Text, request.dump(), true, kMask))) return Json();
        for (;;) {
            Json m;
            if (!NextMessage(m)) return Json();
            if (m.find("event")) {
                backlog_.push_back(std::move(m));
                continue;
            }
            const Json* got = m.find("id");
            if (got && got->as_number() == id) return m;
        }
    }

    // The first event (backlog first, then the wire) for which pred holds; the events before it
    // are consumed. Null when none arrives within the receive timeout.
    Json WaitEvent(const std::function<bool(const std::string&, const Json&)>& pred) {
        for (;;) {
            while (!backlog_.empty()) {
                Json e = std::move(backlog_.front());
                backlog_.erase(backlog_.begin());
                const Json* name = e.find("event");
                const Json* data = e.find("data");
                if (name && data && pred(name->as_string(), *data)) return e;
            }
            Json m;
            if (!NextMessage(m)) return Json();
            if (m.find("event")) backlog_.push_back(std::move(m));
        }
    }

private:
    bool Send(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
#ifdef _WIN32
            const int r = send(sock_, data.data() + off, static_cast<int>(data.size() - off), 0);
#else
            const ssize_t r = send(sock_, data.data() + off, data.size() - off, 0);
#endif
            if (r <= 0) return false;
            off += static_cast<size_t>(r);
        }
        return true;
    }

    bool ReadMore() {
        char buf[16384];
#ifdef _WIN32
        const int r = recv(sock_, buf, static_cast<int>(sizeof buf), 0);
#else
        const ssize_t r = recv(sock_, buf, sizeof buf, 0);
#endif
        if (r <= 0) return false;
        buffer_.append(buf, static_cast<size_t>(r));
        return true;
    }

    bool NextMessage(Json& out) {
        WsMessage m;
        for (;;) {
            if (!buffer_.empty()) {
                reader_.Feed(buffer_);
                buffer_.clear();
            }
            const WsReader::Result r = reader_.Next(m);
            if (r == WsReader::Result::Error) return false;
            if (r == WsReader::Result::Message) {
                if (m.opcode != WsOpcode::Text) continue;
                auto parsed = Json::parse(m.payload);
                if (!parsed) return false;
                out = std::move(*parsed);
                return true;
            }
            if (!ReadMore()) return false;
        }
    }

    SocketHandle sock_ = kBad;
    bool upgraded_ = false;
    std::string buffer_;
    WsReader reader_;
    std::vector<Json> backlog_;
    double nextId_ = 0;
};

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

Json Obj(std::initializer_list<std::pair<const char*, Json>> fields) {
    Json o = Json::MakeObject();
    for (const auto& [k, v] : fields) o.set(k, v);
    return o;
}

const Json* FindPanel(const Json& panels, const std::string& id) {
    for (size_t i = 0; i < panels.size(); ++i) {
        const Json* pid = panels.at(i).find("id");
        if (pid && pid->as_string() == id) return &panels.at(i);
    }
    return nullptr;
}

const Json* FindNode(const Json& panel, double nodeId) {
    const Json* nodes = panel.find("nodes");
    for (size_t i = 0; nodes && i < nodes->size(); ++i) {
        const Json* nid = nodes->at(i).find("id");
        if (nid && nid->as_number() == nodeId) return &nodes->at(i);
    }
    return nullptr;
}

double NodeValue(const Json& panel, double nodeId) {
    const Json* node = FindNode(panel, nodeId);
    const Json* value = node ? node->find("value") : nullptr;
    return value ? value->as_number() : -1.0;
}

std::string ResultName(const Json& response) {
    const Json* result = response.find("result");
    const Json* name = result ? result->find("result") : nullptr;
    return name && name->is_string() ? name->as_string() : std::string("(none: ") + response.dump() + ")";
}

double ErrorCode(const Json& response) {
    const Json* error = response.find("error");
    const Json* code = error ? error->find("code") : nullptr;
    return code ? code->as_number() : 0.0;
}

bool WaitLog(Session& s, const std::string& needle) {
    const Json e = s.WaitEvent([&](const std::string& name, const Json& data) {
        const Json* line = data.find("line");
        return name == "kernel.log" && line && line->as_string().find(needle) != std::string::npos;
    });
    std::printf("log %-48s -> %s\n", needle.c_str(), e.is_null() ? "MISSING" : "seen");
    return !e.is_null();
}

bool WaitSliderSnapshot(Session& s, const std::string& panelId, double nodeId, double value) {
    const Json e = s.WaitEvent([&](const std::string& name, const Json& data) {
        const Json* id = data.find("id");
        return name == "widgets.snapshot" && id && id->as_string() == panelId && NodeValue(data, nodeId) == value;
    });
    std::printf("snapshot %s node %g value %g -> %s\n", panelId.c_str(), nodeId, value, e.is_null() ? "MISSING" : "seen");
    return !e.is_null();
}

void Exercise(uint16_t port) {
    Session s(port);
    CHECK(s.ok());
    if (!s.ok()) return;

    const Json status = s.Call("system.status", Json::MakeObject());
    const Json* plugins = status.find("result") ? status.find("result")->find("kernel")->find("plugins") : nullptr;
    int loaded = 0;
    for (size_t i = 0; plugins && i < plugins->size(); ++i) {
        const Json& p = plugins->at(i);
        std::printf("plugin %s: %s %s\n", p.find("folder")->as_string().c_str(), p.find("state")->as_string().c_str(),
                    p.find("reason")->as_string().c_str());
        const std::string& id = p.find("id")->as_string();
        if ((id == "widgets" || id == "widgets_lua") && p.find("state")->as_string() == "loaded") ++loaded;
    }
    CHECK(loaded == 2);

    const Json list = s.Call("widgets.list", Json::MakeObject());
    const Json* result = list.find("result");
    CHECK(result && result->find("started") && result->find("started")->as_bool());
    CHECK(result && result->find("capabilities") && result->find("capabilities")->find("version")->as_string() == "1.1");
    const Json* panels = result ? result->find("panels") : nullptr;
    CHECK(panels && panels->size() == 2);
    if (!panels) return;
    const Json* native = FindPanel(*panels, "widgets.main");
    const Json* lua = FindPanel(*panels, "widgets_lua.main");
    CHECK(native && lua);
    if (!native || !lua) return;
    std::printf("widgets.main: %s\n", native->dump().c_str());
    CHECK(native->find("nodes")->size() == 10);
    CHECK(lua->find("nodes")->size() == 10);
    CHECK(native->find("owner")->as_string() == "widgets");
    CHECK(FindNode(*native, 4) && FindNode(*native, 4)->find("kind")->as_string() == "button");
    CHECK(NodeValue(*native, 6) == 0.5);
    CHECK(NodeValue(*lua, 6) == 0.5);
    const Json* table = FindNode(*native, 10);
    CHECK(table && table->find("kind")->as_string() == "table" && table->find("columns")->as_number() == 2 &&
          table->find("items")->size() == 4);

    // Native C plugin: BUTTON runs its widgets.ping command; SLIDER changes the host-held value.
    const Json press = s.Call("widgets.event", Obj({ { "panel", "widgets.main" }, { "node_id", 4 }, { "kind", "button" } }));
    CHECK(ResultName(press) == "OK");
    CHECK(WaitLog(s, "widgets: event 1 node 4 kind 2 in widgets.main"));
    CHECK(WaitLog(s, "widget command"));

    const Json slide = s.Call("widgets.event",
                              Obj({ { "panel", "widgets.main" }, { "node_id", 6 }, { "kind", "slider" }, { "value", 0.25 } }));
    CHECK(ResultName(slide) == "OK");
    CHECK(WaitLog(s, "widgets: event 2 node 6 kind 4 in widgets.main"));
    CHECK(WaitSliderSnapshot(s, "widgets.main", 6, 0.25));

    // Lua plugin: its callback logs "<panel> <kind> <node>" and the button invokes widgets_lua.ping.
    const Json luaPress = s.Call("widgets.event", Obj({ { "panel", "widgets_lua.main" }, { "node_id", 4 }, { "kind", "button" } }));
    CHECK(ResultName(luaPress) == "OK");
    CHECK(WaitLog(s, "widgets_lua.main button 4"));

    const Json luaSlide = s.Call("widgets.event",
                                 Obj({ { "panel", "widgets_lua.main" }, { "node_id", 6 }, { "kind", "slider" }, { "value", 0.75 } }));
    CHECK(ResultName(luaSlide) == "OK");
    CHECK(WaitLog(s, "widgets_lua.main slider 6"));
    CHECK(WaitSliderSnapshot(s, "widgets_lua.main", 6, 0.75));

    const Json get = s.Call("widgets.get", Obj({ { "id", "widgets.main" } }));
    CHECK(get.find("result") && NodeValue(*get.find("result"), 6) == 0.25);

    // Refusals answered to the studio before anything reaches the plugin.
    CHECK(ErrorCode(s.Call("widgets.event", Obj({ { "panel", "widgets.main" }, { "node_id", 6 }, { "kind", "slider" }, { "value", 2 } }))) == -32602);
    CHECK(ErrorCode(s.Call("widgets.event", Obj({ { "panel", "widgets.main" }, { "node_id", 4 }, { "kind", "slider" }, { "value", 0 } }))) == -32602);
    CHECK(ErrorCode(s.Call("widgets.event", Obj({ { "panel", "widgets.main" }, { "node_id", 99 }, { "kind", "button" } }))) == -32010);
    CHECK(ErrorCode(s.Call("widgets.event", Obj({ { "panel", "nope.main" }, { "node_id", 1 }, { "kind", "button" } }))) == -32010);
    CHECK(ErrorCode(s.Call("widgets.event", Obj({ { "panel", "widgets.main" }, { "node_id", 8 }, { "kind", "page" }, { "row", 1 } }))) == -32602);
    CHECK(ErrorCode(s.Call("widgets.event", Obj({ { "panel", "widgets.main" }, { "node_id", 4 }, { "kind", "button" }, { "value", 1 } }))) == -32602);
    CHECK(ErrorCode(s.Call("widgets.get", Obj({ { "id", "nope.main" } }))) == -32010);

    const Json stop = s.Call("system.shutdown", Json::MakeObject());
    CHECK(stop.find("result") && stop.find("result")->find("stopping")->as_bool());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_widgets_e2e <path to scuba-hostd> <plugin root>\n");
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("scuba-widgets-e2e-" + std::to_string(rd()));
    std::filesystem::create_directories(dir / "studio");
    {
        std::ofstream indexFile(dir / "studio" / "index.html", std::ios::binary);
        indexFile << "<!doctype html><title>scuba widgets e2e</title>\n";
    }

    std::string cmd = Quote(argv[1]) + " --port 0 --test-shutdown --studio " + Quote((dir / "studio").string()) +
                      " --plugins " + Quote(argv[2]);
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    std::FILE* child = SCUBA_POPEN(cmd.c_str(), "r");
    CHECK(child != nullptr);
    if (!child) return CheckExit("test_widgets_e2e");

    uint16_t port = 0;
    char line[4096];
    while (std::fgets(line, sizeof line, child)) {
        std::fputs(line, stdout);
        const std::string text(line);
        const size_t at = text.find("scuba-hostd: listening port=");
        if (at != std::string::npos) {
            port = static_cast<uint16_t>(std::strtoul(text.c_str() + at + 28, nullptr, 10));
            break;
        }
    }
    CHECK(port != 0);
    if (port != 0) Exercise(port);

    while (std::fgets(line, sizeof line, child)) std::fputs(line, stdout);
    const int exitStatus = SCUBA_PCLOSE(child);
#ifdef _WIN32
    CHECK(exitStatus == 0);
#else
    CHECK(WIFEXITED(exitStatus) && WEXITSTATUS(exitStatus) == 0);
#endif
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return CheckExit("test_widgets_e2e");
}
