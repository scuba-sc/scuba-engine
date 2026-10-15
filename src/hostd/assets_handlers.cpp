#include "hostd/modules.h"
#include "hostd/base64.h"
#include "assets/datacore.h"
#include "assets/formats.h"
#include <cmath>
#include <memory>

namespace scuba::hostd {
namespace {
std::string String(const Json& params, std::string_view key) {
    const auto* value = params.find(key);
    if (!value || !value->is_string() || value->as_string().find('\0') != std::string::npos)
        throw HandlerError(rpc_error::kInvalidParams, "expected string parameter: " + std::string(key));
    return value->as_string();
}
size_t Page(const Json& params, std::string_view key, size_t fallback, size_t maximum) {
    const auto* value = params.find(key);
    if (!value) return fallback;
    const auto number = value->as_number(-1);
    if (!std::isfinite(number) || number < 0 || number > static_cast<double>(maximum) || std::floor(number) != number)
        throw HandlerError(rpc_error::kInvalidParams, "invalid page parameter: " + std::string(key));
    return static_cast<size_t>(number);
}
std::filesystem::path Path(const Json& params, std::string_view key) {
    const auto value = String(params, key);
    if (value.empty()) throw HandlerError(rpc_error::kInvalidParams, "path must not be empty");
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
}
void RegisterAssetsHandlers(Registry& registry, HostContext& host) {
    (void)host;
    auto data = std::make_shared<assets::DataCore>();
    auto add = [&](const std::string& name, Handler handler) {
        registry.register_handler(name, [handler = std::move(handler)](const Json& params, const CallContext& context) {
            try { return handler(params, context); }
            catch (const assets::Error& error) { throw HandlerError(-32060, error.what()); }
            catch (const std::filesystem::filesystem_error&) { throw HandlerError(-32060, "asset filesystem operation failed"); }
        });
    };
    add("assets.open", [data](const Json& params, const CallContext&) {
        return data->Open(Path(params, "path"));
    });
    add("assets.records", [data](const Json& params, const CallContext&) {
        const auto* filter = params.find("filter");
        if (filter && !filter->is_string()) throw HandlerError(rpc_error::kInvalidParams, "filter must be a string");
        return data->Records(Page(params, "offset", 0, 100000000), Page(params, "limit", 100, 500), filter ? filter->as_string() : "");
    });
    add("assets.structs", [data](const Json&, const CallContext&) { return data->Structs(); });
    add("assets.fields", [data](const Json& params, const CallContext&) { return data->Fields(String(params, "record")); });
    add("assets.pack", [data](const Json& params, const CallContext&) {
        data->SetPack(String(params, "text"));
        return Json::MakeObject().set("text", data->Export());
    });
    add("assets.export", [data](const Json&, const CallContext&) {
        return Json::MakeObject().set("text", data->Export());
    });
    add("assets.convert", [](const Json& params, const CallContext&) {
        const auto output = assets::Convert(Path(params, "path"));
        if (output.size() > 24U * 1024U * 1024U) throw assets::Error("studio preview GLB exceeds 24 MiB; use a smaller model");
        return Json::MakeObject().set("glb", Base64Encode(output.data(), output.size())).set("bytes", output.size());
    });
    add("assets.import", [data](const Json& params, const CallContext&) {
        assets::ImportPack(Path(params, "path"), Path(params, "destination"),
            String(params, "virtualPath"), data->Export());
        return Json::MakeObject().set("created", true);
    });
}
} // namespace scuba::hostd
