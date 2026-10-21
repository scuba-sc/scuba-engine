// End to end: starts the real scuba-hostd (argv[1]) on a free port with a synthetic studio
// directory, reads its "listening port=" line, fetches / over HTTP, checks the Host/Origin
// refusals, then connects over WebSocket and calls system.ping, system.status and system.shutdown.
#include "check.h"
#include "hostd/json.h"
#include "hostd/ws.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

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

class Client {
public:
    explicit Client(uint16_t port) {
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
        }
    }
    ~Client() {
        if (sock_ != kBad) CloseSocket(sock_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool ok() const { return sock_ != kBad; }

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

    // Reads more bytes into buffer_; false on close, error or the 10 s receive timeout.
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

    std::string ReadToClose() {
        while (ReadMore()) {
        }
        return std::move(buffer_);
    }

    // Reads up to and including the blank line ending an HTTP head; the rest stays buffered.
    std::string ReadHead() {
        for (;;) {
            const size_t end = buffer_.find("\r\n\r\n");
            if (end != std::string::npos) {
                std::string head = buffer_.substr(0, end + 4);
                buffer_.erase(0, end + 4);
                return head;
            }
            if (!ReadMore()) return {};
        }
    }

    std::string& buffer() { return buffer_; }

private:
    SocketHandle sock_ = kBad;
    std::string buffer_;
};

const uint8_t kMask[4] = { 0x11, 0x22, 0x33, 0x44 };

std::string Get(uint16_t port, const std::string& path, const std::string& host) {
    Client c(port);
    CHECK(c.ok());
    c.Send("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
    return c.ReadToClose();
}

std::string Handshake(Client& c, uint16_t port, const std::string& extraHeader) {
    c.Send("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
           "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" + extraHeader + "\r\n");
    return c.ReadHead();
}

Json Rpc(Client& c, WsReader& reader, const std::string& request) {
    if (!c.Send(WsEncodeFrame(WsOpcode::Text, request, true, kMask))) return Json();
    WsMessage m;
    for (;;) {
        if (!c.buffer().empty()) {
            reader.Feed(c.buffer());
            c.buffer().clear();
        }
        const WsReader::Result r = reader.Next(m);
        if (r == WsReader::Result::Error) return Json();
        if (r == WsReader::Result::Message) {
            if (m.opcode != WsOpcode::Text) continue;
            auto parsed = Json::parse(m.payload);
            if (parsed && parsed->find("event")) continue;
            return parsed ? *parsed : Json();
        }
        if (!c.ReadMore()) return Json();
    }
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_e2e <path to scuba-hostd>\n");
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("scuba-e2e-" + std::to_string(rd()));
    std::filesystem::create_directories(dir / "studio");
    std::filesystem::create_directories(dir / "plugins");
    {
        std::ofstream indexFile(dir / "studio" / "index.html", std::ios::binary);
        indexFile << "<!doctype html><title>scuba e2e studio</title><p>e2e-marker</p>\n";
    }

    std::string cmd = Quote(argv[1]) + " --port 0 --test-shutdown --studio " + Quote((dir / "studio").string()) +
                      " --plugins " + Quote((dir / "plugins").string());
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    std::FILE* child = SCUBA_POPEN(cmd.c_str(), "r");
    CHECK(child != nullptr);
    if (!child) return CheckExit("test_e2e");

    uint16_t port = 0;
    char line[4096];
    while (std::fgets(line, sizeof line, child)) {
        std::fputs(line, stdout);
        const std::string s(line);
        const size_t at = s.find("scuba-hostd: listening port=");
        if (at != std::string::npos) {
            port = static_cast<uint16_t>(std::strtoul(s.c_str() + at + 28, nullptr, 10));
            break;
        }
    }
    CHECK(port != 0);

    if (port != 0) {
        const std::string host = "127.0.0.1:" + std::to_string(port);
        const std::string indexPage = Get(port, "/", host);
        CHECK(indexPage.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
        CHECK(indexPage.find("Content-Type: text/html; charset=utf-8") != std::string::npos);
        CHECK(indexPage.find("e2e-marker") != std::string::npos);
        CHECK(Get(port, "/missing.js", host).rfind("HTTP/1.1 404", 0) == 0);
        CHECK(Get(port, "/../secret.txt", host).rfind("HTTP/1.1 404", 0) == 0);
        CHECK(Get(port, "/", "rebind.example:" + std::to_string(port)).rfind("HTTP/1.1 403", 0) == 0);

        {
            Client evil(port);
            CHECK(evil.ok());
            CHECK(Handshake(evil, port, "Origin: http://evil.example\r\n").rfind("HTTP/1.1 403", 0) == 0);
        }

        Client ws(port);
        CHECK(ws.ok());
        const std::string head = Handshake(ws, port, "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n");
        CHECK(head.rfind("HTTP/1.1 101 Switching Protocols\r\n", 0) == 0);
        CHECK(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        WsReader reader(false, 1 << 20);

        const Json pong = Rpc(ws, reader, R"({"id":1,"method":"system.ping","params":{"echo":"hi"}})");
        std::printf("ping -> %s\n", pong.dump().c_str());
        CHECK(pong.find("id") && pong.find("id")->as_number() == 1);
        CHECK(pong.find("result") && pong.find("result")->find("pong") && pong.find("result")->find("pong")->as_bool());
        CHECK(pong.find("result") && pong.find("result")->find("echo") && pong.find("result")->find("echo")->as_string() == "hi");

        const Json status = Rpc(ws, reader, R"({"id":"s","method":"system.status"})");
        std::printf("status -> %s\n", status.dump().c_str());
        const Json* result = status.find("result");
        CHECK(result && result->find("kernel") && result->find("kernel")->find("started")->as_bool());
        CHECK(result && result->find("clients") && result->find("clients")->as_number() == 1);

        const Json scene = Rpc(ws, reader, R"({"id":10,"method":"mock_game.state"})");
        CHECK(scene.find("result") && scene.find("result")->find("player")->find("spawned")->as_bool());
        const Json caps = Rpc(ws, reader, R"({"id":11,"method":"mock_game.caps.set","params":{"name":"game.actors.state","ready":false}})");
        CHECK(caps.find("result") && caps.find("result")->size() == SCUBA_EXPECT_CAPS);
        const Json moved = Rpc(ws, reader, R"({"id":12,"method":"mock_game.player.move","params":{"zone":4,"pos":[7,8,9],"camera":true}})");
        CHECK(moved.find("result"));
        const Json changed = Rpc(ws, reader, R"({"id":13,"method":"mock_game.state"})");
        CHECK(changed.find("result") && changed.find("result")->find("player")->find("pos")->at(0).as_number() == 7);
        const Json died = Rpc(ws, reader, R"({"id":14,"method":"mock_game.event.player_died"})");
        CHECK(died.find("result") && died.find("result")->find("fired")->as_bool());
        const Json spawned = Rpc(ws, reader, R"({"id":15,"method":"mock_game.event.player_spawned"})");
        CHECK(spawned.find("result") && spawned.find("result")->find("fired")->as_bool());

        const Json missing = Rpc(ws, reader, R"({"id":2,"method":"nope.nothing"})");
        CHECK(missing.find("error") && missing.find("error")->find("code")->as_number() == -32601);

        const Json stop = Rpc(ws, reader, R"({"id":3,"method":"system.shutdown"})");
        CHECK(stop.find("result") && stop.find("result")->find("stopping")->as_bool());
    }

    while (std::fgets(line, sizeof line, child)) std::fputs(line, stdout);
    const int status = SCUBA_PCLOSE(child);
#ifdef _WIN32
    CHECK(status == 0);
#else
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return CheckExit("test_e2e");
}
