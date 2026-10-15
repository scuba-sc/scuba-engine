#include "check.h"
#include "hostd/base64.h"
#include "hostd/server.h"
#include "hostd/sha1.h"
#include "hostd/ws.h"

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
using SocketHandle = SOCKET;
const SocketHandle kInvalidSocket = INVALID_SOCKET;
inline void CloseSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
const SocketHandle kInvalidSocket = -1;
inline void CloseSocket(SocketHandle s) { ::close(s); }
#endif

using namespace scuba::hostd;

namespace {

std::string Hex(const std::array<uint8_t, 20>& d) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : d) {
        out += kHex[b >> 4];
        out += kHex[b & 15u];
    }
    return out;
}

std::string B64(std::string_view s) { return Base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

void TestSha1AndBase64() {
    CHECK(Hex(Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Hex(Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(Hex(Sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    CHECK(B64("") == "");
    CHECK(B64("f") == "Zg==");
    CHECK(B64("fo") == "Zm8=");
    CHECK(B64("foo") == "Zm9v");
    CHECK(B64("foobar") == "Zm9vYmFy");
}

HttpRequest Upgrade(const char* key = "dGhlIHNhbXBsZSBub25jZQ==") {
    const std::string text = std::string("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:7878\r\nUpgrade: websocket\r\n"
                                         "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                         "Sec-WebSocket-Key: ") + key + "\r\n\r\n";
    HttpRequest req;
    size_t consumed = 0;
    CHECK(ParseHttpRequest(text, req, consumed) == HttpParse::Done);
    return req;
}

void TestHandshake() {
    CHECK(WsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(IsWsUpgradeRequest(Upgrade()));
    CHECK(!IsWsUpgradeRequest(Upgrade("short")));
    HttpRequest noVersion = Upgrade();
    for (auto& h : noVersion.headers)
        if (h.first == "sec-websocket-version") h.second = "8";
    CHECK(!IsWsUpgradeRequest(noVersion));
    HttpRequest post = Upgrade();
    post.method = "POST";
    CHECK(!IsWsUpgradeRequest(post));
    const std::string resp = WsHandshakeResponse("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(resp.rfind("HTTP/1.1 101 Switching Protocols\r\n", 0) == 0);
    CHECK(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
}

const uint8_t kMask[4] = { 0x37, 0xfa, 0x21, 0x3d };

void TestRfcExamples() {
    const std::string unmasked = WsEncodeFrame(WsOpcode::Text, "Hello");
    CHECK(unmasked == std::string("\x81\x05Hello", 7));
    const std::string masked = WsEncodeFrame(WsOpcode::Text, "Hello", true, kMask);
    CHECK(masked == std::string("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));

    WsReader server(true, 1 << 20);
    server.Feed(masked);
    WsMessage m;
    CHECK(server.Next(m) == WsReader::Result::Message);
    CHECK(m.opcode == WsOpcode::Text && m.payload == "Hello");
    CHECK(server.Next(m) == WsReader::Result::None);

    WsReader client(false, 1 << 20);
    client.Feed(unmasked);
    CHECK(client.Next(m) == WsReader::Result::Message);
    CHECK(m.payload == "Hello");
}

void TestLengthsAndPartialInput() {
    for (const size_t n : { size_t{ 0 }, size_t{ 125 }, size_t{ 126 }, size_t{ 65535 }, size_t{ 65536 }, size_t{ 200000 } }) {
        const std::string payload(n, 'x');
        const std::string frame = WsEncodeFrame(WsOpcode::Text, payload, true, kMask);
        const size_t header = n < 126 ? 2 : (n <= 65535 ? 4 : 10);
        CHECK(frame.size() == header + 4 + n);
        WsReader r(true, 1 << 20);
        WsMessage m;
        for (size_t i = 0; i < frame.size(); i += 7000) {
            CHECK(r.Next(m) == WsReader::Result::None);
            r.Feed(std::string_view(frame).substr(i, 7000));
        }
        CHECK(r.Next(m) == WsReader::Result::Message);
        CHECK(m.payload == payload);
    }
    const std::string frame = WsEncodeFrame(WsOpcode::Text, "byte by byte", true, kMask);
    WsReader r(true, 1024);
    WsMessage m;
    size_t messages = 0;
    for (const char c : frame) {
        r.Feed(std::string_view(&c, 1));
        while (r.Next(m) == WsReader::Result::Message) ++messages;
    }
    CHECK(messages == 1 && m.payload == "byte by byte");
}

void TestFragmentsAndControl() {
    std::string bytes = WsEncodeFrame(WsOpcode::Text, "Hel", false, kMask);
    bytes += WsEncodeFrame(WsOpcode::Ping, "p", true, kMask);
    bytes += WsEncodeFrame(WsOpcode::Continuation, "lo", true, kMask);
    WsReader r(true, 1024);
    r.Feed(bytes);
    WsMessage m;
    CHECK(r.Next(m) == WsReader::Result::Message);
    CHECK(m.opcode == WsOpcode::Ping && m.payload == "p");
    CHECK(r.Next(m) == WsReader::Result::Message);
    CHECK(m.opcode == WsOpcode::Text && m.payload == "Hello");

    WsReader c(true, 1024);
    c.Feed(WsEncodeClose(ws_close::kNormal, "bye", kMask));
    CHECK(c.Next(m) == WsReader::Result::Message);
    CHECK(m.opcode == WsOpcode::Close && m.payload == std::string("\x03\xe8" "bye", 5));
}

void TestProtocolErrors() {
    WsMessage m;
    {
        WsReader r(true, 1024);
        r.Feed(WsEncodeFrame(WsOpcode::Text, "x"));
        CHECK(r.Next(m) == WsReader::Result::Error);
        CHECK(r.error_code() == ws_close::kProtocolError);
    }
    {
        WsReader r(true, 1024);
        r.Feed(WsEncodeFrame(WsOpcode::Continuation, "x", true, kMask));
        CHECK(r.Next(m) == WsReader::Result::Error);
    }
    {
        WsReader r(true, 1024);
        r.Feed(WsEncodeFrame(WsOpcode::Ping, "x", false, kMask));
        CHECK(r.Next(m) == WsReader::Result::Error);
    }
    {
        WsReader r(true, 1024);
        r.Feed(WsEncodeFrame(WsOpcode::Ping, std::string(126, 'x'), true, kMask));
        CHECK(r.Next(m) == WsReader::Result::Error);
    }
    {
        WsReader r(true, 16);
        r.Feed(WsEncodeFrame(WsOpcode::Text, std::string(17, 'x'), true, kMask));
        CHECK(r.Next(m) == WsReader::Result::Error);
        CHECK(r.error_code() == ws_close::kTooBig);
    }
    {
        WsReader r(true, 1024);
        r.Feed(WsEncodeFrame(WsOpcode::Text, std::string("\xc3\x28", 2), true, kMask));
        CHECK(r.Next(m) == WsReader::Result::Error);
        CHECK(r.error_code() == ws_close::kInvalidPayload);
    }
    {
        WsReader r(true, 1024);
        std::string f = WsEncodeFrame(WsOpcode::Text, "x", true, kMask);
        f[0] = static_cast<char>(static_cast<uint8_t>(f[0]) | 0x40u);
        r.Feed(f);
        CHECK(r.Next(m) == WsReader::Result::Error);
    }
    CHECK(IsValidUtf8("plain ascii"));
    CHECK(IsValidUtf8("\xe2\x82\xac"));
    CHECK(!IsValidUtf8("\xed\xa0\x80"));
    CHECK(!IsValidUtf8("\xc0\xaf"));
}

SocketHandle ConnectClient(uint16_t port) {
    SocketHandle s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return kInvalidSocket;
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    auto* bytes = reinterpret_cast<uint8_t*>(&sin.sin_port);
    bytes[0] = static_cast<uint8_t>(port >> 8);
    bytes[1] = static_cast<uint8_t>(port & 0xFFu);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    if (connect(s, reinterpret_cast<const sockaddr*>(&sin), sizeof sin) != 0) {
        CloseSocket(s);
        return kInvalidSocket;
    }
    return s;
}

bool SendAll(SocketHandle s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef _WIN32
        const int r = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#else
        const ssize_t r = send(s, data.data() + off, data.size() - off, 0);
#endif
        if (r <= 0) return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

void TestServerClientCount() {
    Registry registry;
    Broadcaster broadcaster;
    ServerOptions options;
    options.bindAddress = "127.0.0.1";
    options.port = 0;

    Server server(options, registry, broadcaster);
    std::string err;
    CHECK(server.Start(&err));
    CHECK(err.empty());
    const uint16_t port = server.port();
    CHECK(port != 0);

    // Initial state: no connected clients
    CHECK(server.client_count() == 0);

    // 1. Non-WebSocket HTTP connection: should NOT increment client_count
    SocketHandle httpSock = ConnectClient(port);
    CHECK(httpSock != kInvalidSocket);
    const std::string httpReq = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
    CHECK(SendAll(httpSock, httpReq));
    server.Poll(20);
    CHECK(server.client_count() == 0);
    CloseSocket(httpSock);
    server.Poll(20);

    auto pollForCount = [&](size_t expectedCount) {
        for (int i = 0; i < 20; ++i) {
            server.Poll(5);
            if (server.client_count() == expectedCount) return true;
        }
        return false;
    };

    // 2. First WebSocket client connection: should increment client_count to 1
    SocketHandle ws1 = ConnectClient(port);
    CHECK(ws1 != kInvalidSocket);
    const std::string wsReq = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    CHECK(SendAll(ws1, wsReq));
    CHECK(pollForCount(1));

    // 3. Second WebSocket client connection: should increment client_count to 2
    SocketHandle ws2 = ConnectClient(port);
    CHECK(ws2 != kInvalidSocket);
    CHECK(SendAll(ws2, wsReq));
    CHECK(pollForCount(2));

    // 4. Disconnect first WebSocket client: client_count should drop to 1
    CloseSocket(ws1);
    CHECK(pollForCount(1));

    // 5. Disconnect second WebSocket client: client_count should drop to 0
    CloseSocket(ws2);
    CHECK(pollForCount(0));

    // 6. Stop server: client_count remains 0
    server.Stop();
    CHECK(server.client_count() == 0);
}

}  // namespace

int main() {
    TestSha1AndBase64();
    TestHandshake();
    TestRfcExamples();
    TestLengthsAndPartialInput();
    TestFragmentsAndControl();
    TestProtocolErrors();
    TestServerClientCount();
    return CheckExit("test_websocket");
}
