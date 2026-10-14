#include "hostd/modules.h"
#include "hostd/server.h"

namespace scuba::hostd {

namespace {

double UptimeMs(const HostContext& host) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - host.startedAt).count();
}

}  // namespace

void RegisterSystemHandlers(Registry& registry, HostContext& host) {
    registry.register_handler("system.ping", [&host](const Json& params, const CallContext&) {
        Json out = Json::MakeObject();
        out.set("pong", true);
        out.set("uptime_ms", UptimeMs(host));
        if (const Json* echo = params.find("echo")) out.set("echo", *echo);
        return out;
    });

    registry.register_handler("system.status", [&host](const Json&, const CallContext&) {
        Json kernel = Json::MakeObject();
        kernel.set("started", host.kernel && host.kernel->started());
        kernel.set("plugin_root", host.pluginRoot);
        Json plugins = Json::MakeArray();
        if (host.kernel && host.kernel->started()) {
            for (const auto& p : host.kernel->plugins()) {
                Json item = Json::MakeObject();
                item.set("folder", p.folder);
                item.set("id", p.id);
                item.set("name", p.name);
                item.set("version", p.version);
                item.set("state", p.state);
                item.set("reason", p.reason);
                plugins.push_back(std::move(item));
            }
        }
        kernel.set("plugins", std::move(plugins));

        Json out = Json::MakeObject();
        out.set("name", "scuba-hostd");
        out.set("version", SCUBA_VERSION);
        out.set("sdk_version", SCUBA_SDK_VERSION);
        out.set("uptime_ms", UptimeMs(host));
        out.set("clients", host.server ? host.server->client_count() : size_t{ 0 });
        out.set("kernel", std::move(kernel));
        return out;
    });

    registry.register_handler("system.methods", [&registry](const Json&, const CallContext&) {
        Json out = Json::MakeArray();
        for (const auto& m : registry.methods()) out.push_back(m);
        return out;
    });

    if (host.requestShutdown) {
        registry.register_handler("system.shutdown", [&host](const Json&, const CallContext&) {
            host.requestShutdown();
            Json out = Json::MakeObject();
            out.set("stopping", true);
            return out;
        });
    }
}

}  // namespace scuba::hostd
