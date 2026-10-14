#include "hostd/server.h"

#include "hostd/http.h"
#include "hostd/ws.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scuba::hostd {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
using PollEntry = WSAPOLLFD;
using SockLen = int;
const SocketHandle kInvalidSocket = INVALID_SOCKET;

bool InitSockets() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
int PollSockets(PollEntry* fds, size_t n, int timeoutMs) { return WSAPoll(fds, static_cast<ULONG>(n), timeoutMs); }
void CloseSocket(SocketHandle s) { closesocket(s); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool SetNonBlocking(SocketHandle s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
std::string LastSocketError() { return "winsock error " + std::to_string(WSAGetLastError()); }
long long SendSome(SocketHandle s, const char* data, size_t n) {
    return send(s, data, static_cast<int>(std::min<size_t>(n, 1u << 30)), 0);
}
long long RecvSome(SocketHandle s, char* buf, size_t n) { return recv(s, buf, static_cast<int>(n), 0); }
void ProtectListener(SocketHandle s) {
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
}
void PrepareClient(SocketHandle) {}
#else
using SocketHandle = int;
using PollEntry = pollfd;
using SockLen = socklen_t;
const SocketHandle kInvalidSocket = -1;

bool InitSockets() { return true; }
int PollSockets(PollEntry* fds, size_t n, int timeoutMs) { return ::poll(fds, static_cast<nfds_t>(n), timeoutMs); }
void CloseSocket(SocketHandle s) { ::close(s); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool SetNonBlocking(SocketHandle s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
std::string LastSocketError() { return std::strerror(errno); }
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
long long SendSome(SocketHandle s, const char* data, size_t n) { return ::send(s, data, n, kSendFlags); }
long long RecvSome(SocketHandle s, char* buf, size_t n) { return ::recv(s, buf, n, 0); }
void ProtectListener(SocketHandle s) {
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
}
void PrepareClient(SocketHandle s) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)s;
#endif
}
#endif

void SetPort(sockaddr_in& sin, uint16_t port) {
    auto* bytes = reinterpret_cast<uint8_t*>(&sin.sin_port);
    bytes[0] = static_cast<uint8_t>(port >> 8);
    bytes[1] = static_cast<uint8_t>(port & 0xFFu);
}

uint16_t GetPort(const sockaddr_in& sin) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&sin.sin_port);
    return static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

const char kNoStudioPage[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>scuba-hostd</title></head><body>"
    "<h1>scuba-hostd is running</h1>"
    "<p>The studio is not built. Run <code>npm ci &amp;&amp; npm run build</code> in <code>studio/</code>, "
    "or start scuba-hostd with <code>--studio &lt;dir&gt;</code>.</p>"
    "<p>The JSON bridge is at <code>/ws</code> (see docs/protocol.md).</p></body></html>";

struct Conn {
    SocketHandle sock = kInvalidSocket;
    uint64_t id = 0;
    bool websocket = false;
    bool closeAfterFlush = false;
    bool dead = false;
    std::string in;
    std::string out;
    std::unique_ptr<WsReader> reader;
};

constexpr size_t kMaxPendingOutput = 64u * 1024u * 1024u;

}  // namespace

struct Server::Impl {
    Impl(ServerOptions o, const Registry& r, Broadcaster& b) : opt(std::move(o)), registry(r), broadcaster(b) {}

    ServerOptions opt;
    const Registry& registry;
    Broadcaster& broadcaster;
    SocketHandle listener = kInvalidSocket;
    uint16_t boundPort = 0;
    uint64_t nextId = 1;
    std::vector<std::unique_ptr<Conn>> conns;
    std::vector<std::string> allowedHosts;
    std::vector<std::string> allowedOrigins;

    void ComputeAllowed() {
        std::vector<std::string> names = { "127.0.0.1", "localhost", "[::1]" };
        if (opt.bindAddress != "0.0.0.0" && std::find(names.begin(), names.end(), opt.bindAddress) == names.end())
            names.push_back(opt.bindAddress);
        const std::string suffix = ":" + std::to_string(boundPort);
        for (const auto& n : names) {
            allowedHosts.push_back(n + suffix);
            allowedOrigins.push_back("http://" + n + suffix);
        }
        for (const auto& o : opt.allowedOrigins) allowedOrigins.push_back(ToLowerAscii(o));
    }

    void Accept() {
        for (;;) {
            const SocketHandle s = accept(listener, nullptr, nullptr);
            if (s == kInvalidSocket) return;
            if (!SetNonBlocking(s)) {
                CloseSocket(s);
                continue;
            }
            PrepareClient(s);
            auto c = std::make_unique<Conn>();
            c->sock = s;
            c->id = nextId++;
            conns.push_back(std::move(c));
        }
    }

    void Read(Conn& c) {
        char buf[64 * 1024];
        for (;;) {
            const long long r = RecvSome(c.sock, buf, sizeof buf);
            if (r > 0) {
                if (!c.closeAfterFlush) c.in.append(buf, static_cast<size_t>(r));
                continue;
            }
            if (r < 0 && WouldBlock()) return;
            c.dead = true;
            return;
        }
    }

    void Write(Conn& c) {
        while (!c.out.empty()) {
            const long long r = SendSome(c.sock, c.out.data(), c.out.size());
            if (r > 0) {
                c.out.erase(0, static_cast<size_t>(r));
                continue;
            }
            if (r < 0 && WouldBlock()) break;
            c.dead = true;
            return;
        }
        if (c.out.empty() && c.closeAfterFlush) c.dead = true;
        if (c.out.size() > kMaxPendingOutput) c.dead = true;
    }

    void Reply(Conn& c, std::string response) {
        c.out += response;
        c.closeAfterFlush = true;
        c.in.clear();
    }

    void Process(Conn& c) {
        if (c.closeAfterFlush || c.dead) return;
        if (c.websocket) {
            ProcessWs(c);
            return;
        }
        HttpRequest req;
        size_t consumed = 0;
        switch (ParseHttpRequest(c.in, req, consumed)) {
            case HttpParse::Incomplete: return;
            case HttpParse::Error:
                Reply(c, HttpResponse(c.in.size() > kMaxHttpHead ? 431 : 400, "text/plain; charset=utf-8", "bad request\n"));
                return;
            case HttpParse::Done: break;
        }
        c.in.erase(0, consumed);
        HandleHttp(c, req);
    }

    void HandleHttp(Conn& c, const HttpRequest& req) {
        const std::string* host = req.header("host");
        if (!host || std::find(allowedHosts.begin(), allowedHosts.end(), ToLowerAscii(*host)) == allowedHosts.end()) {
            Reply(c, HttpResponse(403, "text/plain; charset=utf-8", "forbidden host\n"));
            return;
        }
        if (req.path == "/ws") {
            if (!IsWsUpgradeRequest(req)) {
                Reply(c, HttpResponse(426, "text/plain; charset=utf-8", "WebSocket upgrade required\n",
                                      { { "Upgrade", "websocket" }, { "Sec-WebSocket-Version", "13" } }));
                return;
            }
            if (const std::string* origin = req.header("origin");
                origin && std::find(allowedOrigins.begin(), allowedOrigins.end(), ToLowerAscii(*origin)) == allowedOrigins.end()) {
                Reply(c, HttpResponse(403, "text/plain; charset=utf-8", "forbidden origin\n"));
                return;
            }
            c.out += WsHandshakeResponse(*req.header("sec-websocket-key"));
            c.websocket = true;
            c.reader = std::make_unique<WsReader>(true, opt.maxMessageBytes);
            ProcessWs(c);
            return;
        }
        if (req.method != "GET" && req.method != "HEAD") {
            Reply(c, HttpResponse(405, "text/plain; charset=utf-8", "method not allowed\n", { { "Allow", "GET, HEAD" } }));
            return;
        }
        ServeStatic(c, req);
    }

    static bool ReadFile(const std::filesystem::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !f.bad();
    }

    void ServeStatic(Conn& c, const HttpRequest& req) {
        const bool head = req.method == "HEAD";
        std::error_code ec;
        const bool haveStudio = !opt.staticDir.empty() && std::filesystem::is_regular_file(opt.staticDir / "index.html", ec);
        if (!haveStudio) {
            if (req.path == "/" || req.path == "/index.html")
                Reply(c, HttpResponse(200, "text/html; charset=utf-8", kNoStudioPage, {}, head));
            else
                Reply(c, HttpResponse(404, "text/plain; charset=utf-8", "not found\n", {}, head));
            return;
        }
        auto file = ResolveStaticPath(opt.staticDir, req.path);
        if (!file) {
            Reply(c, HttpResponse(404, "text/plain; charset=utf-8", "not found\n", {}, head));
            return;
        }
        if (!std::filesystem::is_regular_file(*file, ec)) {
            if (file->has_extension()) {
                Reply(c, HttpResponse(404, "text/plain; charset=utf-8", "not found\n", {}, head));
                return;
            }
            file = opt.staticDir / "index.html";
        }
        std::string body;
        if (!ReadFile(*file, body)) {
            Reply(c, HttpResponse(500, "text/plain; charset=utf-8", "cannot read file\n", {}, head));
            return;
        }
        Reply(c, HttpResponse(200, MimeTypeFor(*file), body, {}, head));
    }

    void ProcessWs(Conn& c) {
        if (!c.in.empty()) {
            c.reader->Feed(c.in);
            c.in.clear();
        }
        WsMessage msg;
        for (;;) {
            const WsReader::Result r = c.reader->Next(msg);
            if (r == WsReader::Result::None) return;
            if (r == WsReader::Result::Error) {
                c.out += WsEncodeClose(c.reader->error_code(), c.reader->error_text());
                c.closeAfterFlush = true;
                return;
            }
            switch (msg.opcode) {
                case WsOpcode::Text: {
                    CallContext ctx;
                    ctx.client = c.id;
                    c.out += WsEncodeFrame(WsOpcode::Text, registry.dispatch_text(msg.payload, ctx));
                    break;
                }
                case WsOpcode::Ping: c.out += WsEncodeFrame(WsOpcode::Pong, msg.payload); break;
                case WsOpcode::Pong: break;
                case WsOpcode::Close: {
                    uint16_t code = ws_close::kNormal;
                    if (msg.payload.size() >= 2)
                        code = static_cast<uint16_t>((static_cast<uint8_t>(msg.payload[0]) << 8) | static_cast<uint8_t>(msg.payload[1]));
                    c.out += WsEncodeClose(code);
                    c.closeAfterFlush = true;
                    return;
                }
                case WsOpcode::Binary:
                case WsOpcode::Continuation:
                    c.out += WsEncodeClose(ws_close::kUnsupportedData, "binary messages are not supported");
                    c.closeAfterFlush = true;
                    return;
            }
        }
    }
};

Server::Server(ServerOptions options, const Registry& registry, Broadcaster& broadcaster)
    : impl_(std::make_unique<Impl>(std::move(options), registry, broadcaster)) {}

Server::~Server() { Stop(); }

bool Server::Start(std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!InitSockets()) return fail("socket library initialisation failed");
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    SetPort(sin, impl_->opt.port);
    if (inet_pton(AF_INET, impl_->opt.bindAddress.c_str(), &sin.sin_addr) != 1)
        return fail("bad bind address '" + impl_->opt.bindAddress + "' (an IPv4 literal is required)");

    const SocketHandle s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return fail("socket: " + LastSocketError());
    ProtectListener(s);
    if (bind(s, reinterpret_cast<const sockaddr*>(&sin), sizeof sin) != 0) {
        const std::string why = "bind " + impl_->opt.bindAddress + ":" + std::to_string(impl_->opt.port) + ": " + LastSocketError();
        CloseSocket(s);
        return fail(why);
    }
    if (listen(s, 64) != 0 || !SetNonBlocking(s)) {
        const std::string why = "listen: " + LastSocketError();
        CloseSocket(s);
        return fail(why);
    }
    sockaddr_in bound{};
    SockLen len = sizeof bound;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        const std::string why = "getsockname: " + LastSocketError();
        CloseSocket(s);
        return fail(why);
    }
    impl_->listener = s;
    impl_->boundPort = GetPort(bound);
    impl_->ComputeAllowed();
    return true;
}

uint16_t Server::port() const { return impl_->boundPort; }

size_t Server::client_count() const {
    size_t n = 0;
    for (const auto& c : impl_->conns)
        if (c->websocket && !c->dead) ++n;
    return n;
}

void Server::Poll(int timeoutMs) {
    Impl& im = *impl_;
    if (im.listener == kInvalidSocket) return;

    for (const std::string& event : im.broadcaster.drain())
        for (auto& c : im.conns)
            if (c->websocket && !c->closeAfterFlush && !c->dead) c->out += WsEncodeFrame(WsOpcode::Text, event);

    std::vector<PollEntry> fds;
    fds.reserve(im.conns.size() + 1);
    PollEntry lp{};
    lp.fd = im.listener;
    lp.events = static_cast<short>(POLLIN);
    fds.push_back(lp);
    for (const auto& c : im.conns) {
        PollEntry p{};
        p.fd = c->sock;
        p.events = static_cast<short>(c->out.empty() ? POLLIN : (POLLIN | POLLOUT));
        fds.push_back(p);
    }
    if (PollSockets(fds.data(), fds.size(), timeoutMs) < 0) return;

    const size_t existing = im.conns.size();
    for (size_t i = 0; i < existing; ++i) {
        Conn& c = *im.conns[i];
        if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
            im.Read(c);
            im.Process(c);
        }
        if (!c.dead) im.Write(c);
    }
    if (fds[0].revents & POLLIN) im.Accept();

    for (auto& c : im.conns)
        if (c->dead) CloseSocket(c->sock);
    im.conns.erase(std::remove_if(im.conns.begin(), im.conns.end(), [](const std::unique_ptr<Conn>& c) { return c->dead; }),
                   im.conns.end());
}

void Server::Stop() {
    Impl& im = *impl_;
    for (auto& c : im.conns) {
        if (c->websocket && !c->dead) {
            c->out += WsEncodeClose(ws_close::kGoingAway, "server shutting down");
            im.Write(*c);
        }
        CloseSocket(c->sock);
    }
    im.conns.clear();
    if (im.listener != kInvalidSocket) {
        CloseSocket(im.listener);
        im.listener = kInvalidSocket;
    }
}

}  // namespace scuba::hostd
