#include "hostd/ws.h"

#include "hostd/base64.h"
#include "hostd/sha1.h"

namespace scuba::hostd {

std::string WsAcceptKey(std::string_view clientKey) {
    std::string input(clientKey);
    input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = Sha1(input);
    return Base64Encode(digest.data(), digest.size());
}

namespace {

bool IsBase64Char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
}

bool IsKey16(std::string_view key) {
    if (key.size() != 24 || key[22] != '=' || key[23] != '=') return false;
    for (size_t i = 0; i < 22; ++i)
        if (!IsBase64Char(key[i])) return false;
    return true;
}

}  // namespace

bool IsWsUpgradeRequest(const HttpRequest& request) {
    if (request.method != "GET" || request.version != "HTTP/1.1") return false;
    if (!request.header_has_token("upgrade", "websocket")) return false;
    if (!request.header_has_token("connection", "upgrade")) return false;
    const std::string* version = request.header("sec-websocket-version");
    if (!version || *version != "13") return false;
    const std::string* key = request.header("sec-websocket-key");
    return key && IsKey16(*key);
}

std::string WsHandshakeResponse(std::string_view clientKey) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + WsAcceptKey(clientKey) + "\r\n\r\n";
}

std::string WsEncodeFrame(WsOpcode opcode, std::string_view payload, bool fin, const uint8_t* mask) {
    std::string out;
    out.reserve(payload.size() + 14);
    out += static_cast<char>((fin ? 0x80u : 0u) | static_cast<uint8_t>(opcode));
    const auto maskBit = static_cast<uint8_t>(mask ? 0x80u : 0u);
    const uint64_t n = payload.size();
    if (n < 126) {
        out += static_cast<char>(maskBit | static_cast<uint8_t>(n));
    } else if (n <= 0xFFFFu) {
        out += static_cast<char>(maskBit | 126u);
        out += static_cast<char>((n >> 8) & 0xFFu);
        out += static_cast<char>(n & 0xFFu);
    } else {
        out += static_cast<char>(maskBit | 127u);
        for (int i = 7; i >= 0; --i) out += static_cast<char>((n >> (i * 8)) & 0xFFu);
    }
    if (mask) {
        for (int i = 0; i < 4; ++i) out += static_cast<char>(mask[i]);
        for (size_t i = 0; i < payload.size(); ++i)
            out += static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
    } else {
        out.append(payload);
    }
    return out;
}

std::string WsEncodeClose(uint16_t code, std::string_view reason, const uint8_t* mask) {
    std::string payload;
    payload += static_cast<char>((code >> 8) & 0xFFu);
    payload += static_cast<char>(code & 0xFFu);
    payload.append(reason.substr(0, 123));
    return WsEncodeFrame(WsOpcode::Close, payload, true, mask);
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<uint8_t>(s[i]);
        size_t len;
        uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0u) == 0xC0u) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0u) == 0xE0u) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8u) == 0xF0u) { len = 4; cp = c & 0x07u; }
        else return false;
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

WsReader::WsReader(bool expectMasked, size_t maxMessageBytes) : expectMasked_(expectMasked), maxMessage_(maxMessageBytes) {}

void WsReader::Feed(std::string_view bytes) {
    if (failed_) return;
    if (readPos_ > 0 && readPos_ >= buffer_.size() / 2) {
        buffer_.erase(0, readPos_);
        readPos_ = 0;
    }
    buffer_.append(bytes);
}

WsReader::Result WsReader::Fail(uint16_t code, const char* why) {
    failed_ = true;
    errorCode_ = code;
    errorText_ = why;
    return Result::Error;
}

WsReader::Result WsReader::Next(WsMessage& out) {
    if (failed_) return Result::Error;
    for (;;) {
        const std::string_view buf = std::string_view(buffer_).substr(readPos_);
        if (buf.size() < 2) return Result::None;
        const auto b0 = static_cast<uint8_t>(buf[0]);
        const auto b1 = static_cast<uint8_t>(buf[1]);
        const bool fin = (b0 & 0x80u) != 0;
        if (b0 & 0x70u) return Fail(ws_close::kProtocolError, "reserved bits set");
        const auto op = static_cast<uint8_t>(b0 & 0x0Fu);
        const bool masked = (b1 & 0x80u) != 0;
        if (masked != expectMasked_) return Fail(ws_close::kProtocolError, expectMasked_ ? "unmasked client frame" : "masked server frame");

        size_t pos = 2;
        uint64_t len = b1 & 0x7Fu;
        if (len == 126) {
            if (buf.size() < 4) return Result::None;
            len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[2])) << 8) | static_cast<uint8_t>(buf[3]);
            pos = 4;
            if (len < 126) return Fail(ws_close::kProtocolError, "non-minimal length");
        } else if (len == 127) {
            if (buf.size() < 10) return Result::None;
            len = 0;
            for (size_t i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(buf[2 + i]);
            pos = 10;
            if (len >> 63) return Fail(ws_close::kProtocolError, "length high bit set");
            if (len <= 0xFFFFu) return Fail(ws_close::kProtocolError, "non-minimal length");
        }

        const bool control = (op & 0x8u) != 0;
        if (control) {
            if (!fin) return Fail(ws_close::kProtocolError, "fragmented control frame");
            if (len > 125) return Fail(ws_close::kProtocolError, "control frame too long");
            if (op != 0x8 && op != 0x9 && op != 0xA) return Fail(ws_close::kProtocolError, "unknown control opcode");
        } else if (op != 0x0 && op != 0x1 && op != 0x2) {
            return Fail(ws_close::kProtocolError, "unknown data opcode");
        }
        if (len > maxMessage_ || (!control && fragments_.size() + len > maxMessage_)) return Fail(ws_close::kTooBig, "message too big");

        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (masked) {
            if (buf.size() < pos + 4) return Result::None;
            for (size_t i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(buf[pos + i]);
            pos += 4;
        }
        const auto n = static_cast<size_t>(len);
        if (buf.size() < pos + n) return Result::None;

        std::string payload(buf.substr(pos, n));
        if (masked)
            for (size_t i = 0; i < n; ++i) payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
        readPos_ += pos + n;

        if (control) {
            if (op == 0x8 && payload.size() == 1) return Fail(ws_close::kProtocolError, "bad close payload");
            out.opcode = static_cast<WsOpcode>(op);
            out.payload = std::move(payload);
            return Result::Message;
        }
        if (op == 0x0) {
            if (!inFragmented_) return Fail(ws_close::kProtocolError, "continuation without a message");
            fragments_ += payload;
        } else {
            if (inFragmented_) return Fail(ws_close::kProtocolError, "new message inside a fragmented one");
            fragmentOpcode_ = static_cast<WsOpcode>(op);
            fragments_ = std::move(payload);
            inFragmented_ = true;
        }
        if (!fin) continue;
        inFragmented_ = false;
        if (fragmentOpcode_ == WsOpcode::Text && !IsValidUtf8(fragments_)) return Fail(ws_close::kInvalidPayload, "invalid UTF-8");
        out.opcode = fragmentOpcode_;
        out.payload = std::move(fragments_);
        fragments_.clear();
        return Result::Message;
    }
}

}  // namespace scuba::hostd
