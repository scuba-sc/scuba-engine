#pragma once
// The bridge's method registry and event broadcast (docs/protocol.md).
//
// A module adds its methods in its own file and is wired in with one line in modules.cpp:
//
//   void RegisterWidgetHandlers(Registry& r, HostContext& host) {
//       r.register_handler("widgets.list", [&host](const Json& params, const CallContext&) {
//           return Json::MakeArray();
//       });
//   }
//
// Handlers run on the host's main thread (the sco-core game thread), one at a time. They return
// the result value or throw HandlerError for an error response; any other exception becomes an
// internal error. Broadcaster::broadcast is safe from any thread.
#include "hostd/json.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace scuba::hostd {

namespace rpc_error {
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kHandlerFailed = -32000;
constexpr int kUnavailable = -32001;
}  // namespace rpc_error

class HandlerError : public std::runtime_error {
public:
    HandlerError(int code, const std::string& message, Json data = Json())
        : std::runtime_error(message), code_(code), data_(std::move(data)) {}
    int code() const { return code_; }
    const Json& data() const { return data_; }

private:
    int code_;
    Json data_;
};

struct CallContext {
    uint64_t client = 0;   // connection id; 0 for in-process calls
};

using Handler = std::function<Json(const Json& params, const CallContext& ctx)>;

class Registry {
public:
    // Method names are "<domain>.<method>[.<more>]", each part [a-z][a-z0-9_]*. Throws
    // std::invalid_argument for a bad name or one registered already.
    void register_handler(std::string method, Handler fn);
    bool has(std::string_view method) const;
    std::vector<std::string> methods() const;

    // One request object -> one response object. Never throws.
    Json dispatch(const Json& request, const CallContext& ctx) const;
    // Request text -> response text (parse errors answered with id null).
    std::string dispatch_text(std::string_view requestText, const CallContext& ctx) const;

private:
    std::map<std::string, Handler, std::less<>> handlers_;
};

bool IsValidMethodName(std::string_view method);

Json MakeEvent(std::string_view event, Json data);

// Server events, queued from any thread and sent to every WebSocket client by the server loop.
class Broadcaster {
public:
    void broadcast(std::string_view event, Json data);
    // The queued event texts, oldest first; the queue is left empty.
    std::vector<std::string> drain();

private:
    std::mutex mutex_;
    std::vector<std::string> queue_;
};

}  // namespace scuba::hostd
