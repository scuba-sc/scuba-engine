#include "hostd/modules.h"
#include "hostd/widgets_bridge.h"

#include <memory>

namespace scuba::hostd {

void RegisterWidgetsHandlers(Registry& registry, HostContext& host) {
    auto renderer = std::make_shared<StudioWidgetRenderer>(host.broadcaster);

    host.tickHooks.push_back([renderer](uint32_t nowMs) { renderer->Tick(nowMs); });

    registry.register_handler("widgets.list", [renderer](const Json&, const CallContext&) {
        renderer->Pump();
        Json out = Json::MakeObject();
        out.set("started", sco::widgets::Started());
        out.set("capabilities", WidgetCapabilitiesJson());
        out.set("panels", renderer->PanelsJson());
        return out;
    });

    registry.register_handler("widgets.get", [renderer](const Json& params, const CallContext&) {
        const Json* id = params.find("id");
        if (!id || !id->is_string()) throw HandlerError(rpc_error::kInvalidParams, "id must be a panel id");
        renderer->Pump();
        const Json* panel = renderer->FindPanel(id->as_string());
        if (!panel) throw HandlerError(widgets_error::kNotFound, "no panel " + id->as_string());
        return *panel;
    });

    registry.register_handler("widgets.event", [renderer](const Json& params, const CallContext&) {
        const sco::Result result = renderer->Submit(ParseWidgetEvent(params));
        Json out = Json::MakeObject();
        out.set("result", sco::ResultName(result));
        out.set("revision", renderer->revision());
        return out;
    });
}

}  // namespace scuba::hostd
