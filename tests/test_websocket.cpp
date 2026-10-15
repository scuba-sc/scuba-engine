#include "check.h"
#include "hostd/base64.h"
#include "hostd/sha1.h"
#include "hostd/ws.h"

#include <string>

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
    CHECK(Hex(Sha1(std::string(1, 'a'))) == "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8");
    CHECK(Hex(Sha1(std::string(55, 'a'))) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(Hex(Sha1(std::string(56, 'a'))) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK(Hex(Sha1(std::string(57, 'a'))) == "f08f24908d682555111be7ff6f004e78283d989a");
    CHECK(Hex(Sha1(std::string(63, 'a'))) == "03f09f5b158a7a8cdad920bddc29b81c18a551f5");
    CHECK(Hex(Sha1(std::string(64, 'a'))) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    CHECK(Hex(Sha1(std::string(65, 'a'))) == "11655326c708d70319be2610e8a57d9a5b959d3b");
    CHECK(Hex(Sha1(std::string(119, 'a'))) == "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56");
    CHECK(Hex(Sha1(std::string(120, 'a'))) == "f34c1488385346a55709ba056ddd08280dd4c6d6");
    CHECK(Hex(Sha1(std::string(128, 'a'))) == "ad5b3fdbcb526778c2839d2f151ea753995e26a0");
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

}  // namespace

int main() {
    TestSha1AndBase64();
    TestHandshake();
    TestRfcExamples();
    TestLengthsAndPartialInput();
    TestFragmentsAndControl();
    TestProtocolErrors();
    return CheckExit("test_websocket");
}
