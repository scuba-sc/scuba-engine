#include "hostd/handlers.h"

#include <exception>

namespace scuba::hostd {

bool IsValidMethodName(std::string_view method) {
    size_t parts = 0;
    size_t i = 0;
    while (i <= method.size()) {
        const size_t dot = method.find('.', i);
        const std::string_view part = method.substr(i, dot == std::string_view::npos ? std::string_view::npos : dot - i);
        if (part.empty() || part.size() > 64) return false;
        if (part[0] < 'a' || part[0] > 'z') return false;
        for (const char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        ++parts;
        if (dot == std::string_view::npos) break;
        i = dot + 1;
    }
    return parts >= 2;
}

void Registry::register_handler(std::string method, Handler fn) {
    if (!IsValidMethodName(method)) throw std::invalid_argument("bad method name: " + method);
    if (!fn) throw std::invalid_argument("empty handler for " + method);
    if (handlers_.count(method)) throw std::invalid_argument("method registered twice: " + method);
    handlers_.emplace(std::move(method), std::move(fn));
}

bool Registry::has(std::string_view method) const { return handlers_.find(method) != handlers_.end(); }

std::vector<std::string> Registry::methods() const {
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (const auto& entry : handlers_) out.push_back(entry.first);
    return out;
}

namespace {

Json ErrorResponse(Json id, int code, const std::string& message, Json data = Json()) {
    Json error = Json::MakeObject();
    error.set("code", code);
    error.set("message", message);
    if (!data.is_null()) error.set("data", std::move(data));
    Json out = Json::MakeObject();
    out.set("id", std::move(id));
    out.set("error", std::move(error));
    return out;
}

}  // namespace

Json Registry::dispatch(const Json& request, const CallContext& ctx) const {
    if (!request.is_object()) return ErrorResponse(Json(), rpc_error::kInvalidRequest, "request must be an object");
    const Json* id = request.find("id");
    if (!id || !(id->is_string() || id->is_number()))
        return ErrorResponse(Json(), rpc_error::kInvalidRequest, "request needs an id (string or number)");
    const Json* method = request.find("method");
    if (!method || !method->is_string()) return ErrorResponse(*id, rpc_error::kInvalidRequest, "request needs a method (string)");
    Json params = Json::MakeObject();
    if (const Json* p = request.find("params"); p && !p->is_null()) {
        if (!p->is_object() && !p->is_array()) return ErrorResponse(*id, rpc_error::kInvalidParams, "params must be an object or array");
        params = *p;
    }
    const auto it = handlers_.find(method->as_string());
    if (it == handlers_.end()) return ErrorResponse(*id, rpc_error::kMethodNotFound, "no such method: " + method->as_string());
    try {
        Json result = it->second(params, ctx);
        Json out = Json::MakeObject();
        out.set("id", *id);
        out.set("result", std::move(result));
        return out;
    } catch (const HandlerError& e) {
        return ErrorResponse(*id, e.code(), e.what(), e.data());
    } catch (const std::exception& e) {
        return ErrorResponse(*id, rpc_error::kInternalError, std::string("internal error: ") + e.what());
    } catch (...) {
        return ErrorResponse(*id, rpc_error::kInternalError, "internal error");
    }
}

std::string Registry::dispatch_text(std::string_view requestText, const CallContext& ctx) const {
    std::string error;
    const auto request = Json::parse(requestText, &error);
    if (!request) return ErrorResponse(Json(), rpc_error::kParseError, "parse error: " + error).dump();
    return dispatch(*request, ctx).dump();
}

Json MakeEvent(std::string_view event, Json data) {
    Json out = Json::MakeObject();
    out.set("event", event);
    out.set("data", std::move(data));
    return out;
}

void Broadcaster::broadcast(std::string_view event, Json data) {
    std::string text = MakeEvent(event, std::move(data)).dump();
    const std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(text));
}

std::vector<std::string> Broadcaster::drain() {
    std::vector<std::string> out;
    const std::lock_guard<std::mutex> lock(mutex_);
    out.swap(queue_);
    return out;
}

}  // namespace scuba::hostd
