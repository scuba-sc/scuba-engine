#pragma once
// WebSocket (RFC 6455): the opening handshake and the framing layer, transport-free so it can be
// unit-tested. Extensions and subprotocols are not negotiated.
#include "hostd/http.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scuba::hostd {

enum class WsOpcode : uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

namespace ws_close {
constexpr uint16_t kNormal = 1000;
constexpr uint16_t kGoingAway = 1001;
constexpr uint16_t kProtocolError = 1002;
constexpr uint16_t kUnsupportedData = 1003;
constexpr uint16_t kInvalidPayload = 1007;
constexpr uint16_t kTooBig = 1009;
}  // namespace ws_close

// base64(SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
std::string WsAcceptKey(std::string_view clientKey);

// A valid client opening handshake: GET, HTTP/1.1, Upgrade: websocket, Connection: upgrade,
// Sec-WebSocket-Version: 13 and a Sec-WebSocket-Key of 16 base64-encoded bytes.
bool IsWsUpgradeRequest(const HttpRequest& request);

// The 101 response for a request IsWsUpgradeRequest accepted.
std::string WsHandshakeResponse(std::string_view clientKey);

// One frame. mask (4 bytes) set: a client frame, payload masked; null: a server frame.
std::string WsEncodeFrame(WsOpcode opcode, std::string_view payload, bool fin = true, const uint8_t* mask = nullptr);
std::string WsEncodeClose(uint16_t code, std::string_view reason = {}, const uint8_t* mask = nullptr);

bool IsValidUtf8(std::string_view s);

struct WsMessage {
    WsOpcode opcode = WsOpcode::Text;   // Text, Binary, Ping, Pong or Close
    std::string payload;                // Close: the raw close payload (code + reason)
};

// Incremental reader: Feed bytes as they arrive, then call Next until it returns None. Fragmented
// data messages are reassembled; control frames may interleave. After Error, close with
// error_code() and stop reading.
class WsReader {
public:
    enum class Result { None, Message, Error };

    // expectMasked: true on the server side (client frames must be masked), false on a client.
    WsReader(bool expectMasked, size_t maxMessageBytes);

    void Feed(std::string_view bytes);
    Result Next(WsMessage& out);
    uint16_t error_code() const { return errorCode_; }
    const std::string& error_text() const { return errorText_; }

private:
    Result Fail(uint16_t code, const char* why);

    bool expectMasked_;
    size_t maxMessage_;
    std::string buffer_;
    size_t readPos_ = 0;
    bool inFragmented_ = false;
    WsOpcode fragmentOpcode_ = WsOpcode::Text;
    std::string fragments_;
    bool failed_ = false;
    uint16_t errorCode_ = 0;
    std::string errorText_;
};

}  // namespace scuba::hostd
