#include "hostd/widgets_bridge.h"

#include "sco/runtime.h"
#include "sco_widgets.h"

#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace scuba::hostd {

namespace {

struct KindName {
    uint32_t kind;
    const char* name;
};

constexpr KindName kKinds[] = {
    { SCO_WIDGET_LABEL, "label" },   { SCO_WIDGET_BUTTON, "button" }, { SCO_WIDGET_TOGGLE, "toggle" },
    { SCO_WIDGET_SLIDER, "slider" }, { SCO_WIDGET_COMBO, "combo" },   { SCO_WIDGET_LIST, "list" },
    { SCO_WIDGET_TEXT_INPUT, "text_input" }, { SCO_WIDGET_TABLE, "table" }, { SCO_WIDGET_ROW, "row" },
    { SCO_WIDGET_COLUMN, "column" },
};

const char* SeverityName(uint32_t severity) {
    switch (severity) {
        case SCO_WIDGET_SEVERITY_INFO: return "info";
        case SCO_WIDGET_SEVERITY_WARNING: return "warning";
        case SCO_WIDGET_SEVERITY_ERROR: return "error";
        default: return "none";
    }
}

[[noreturn]] void BadParams(const std::string& message) {
    throw HandlerError(rpc_error::kInvalidParams, message);
}

int64_t IntegerField(const Json& v, const char* field, int64_t lo, int64_t hi) {
    if (!v.is_number()) BadParams(std::string(field) + " must be a number");
    const double d = v.as_number();
    if (!std::isfinite(d) || std::floor(d) != d || d < static_cast<double>(lo) || d > static_cast<double>(hi))
        BadParams(std::string(field) + " is out of range");
    return static_cast<int64_t>(d);
}

double NumberOf(const Json& node, const char* key) {
    const Json* v = node.find(key);
    return v ? v->as_number() : 0.0;
}

}  // namespace

const char* WidgetKindName(uint32_t kind) {
    for (const auto& k : kKinds)
        if (k.kind == kind) return k.name;
    return "";
}

uint32_t WidgetKindFromName(const std::string& name) {
    if (name == "page") return SCO_WIDGET_EVENT_PAGE;
    for (const auto& k : kKinds)
        if (name == k.name) return k.kind;
    return 0;
}

Json WidgetPanelToJson(const sco::widgets::Panel& panel, uint64_t revision) {
    Json nodes = Json::MakeArray();
    bool extended = false;
    for (const auto& n : panel.nodes) {
        const bool password = (n.flags & SCO_WIDGET_PASSWORD) != 0;
        Json node = Json::MakeObject();
        node.set("id", n.id);
        node.set("parent", n.parent);
        node.set("kind", WidgetKindName(n.kind));
        node.set("kind_code", n.kind);
        node.set("disabled", (n.flags & SCO_WIDGET_DISABLED) != 0);
        node.set("password", password);
        node.set("label", n.label);
        node.set("text", password ? std::string() : n.text);
        node.set("value", n.value);
        node.set("minimum", n.minimum);
        node.set("maximum", n.maximum);
        node.set("index", n.index);
        Json items = Json::MakeArray();
        for (const auto& item : n.items) items.push_back(item);
        node.set("items", std::move(items));
        node.set("columns", n.columns);
        node.set("extended", n.extended);
        node.set("severity", SeverityName(n.severity));
        node.set("tooltip", n.tooltip);
        node.set("first_row", n.first_row);
        node.set("visible_rows", n.visible_rows);
        if (!n.tag.empty()) node.set("tag", n.tag);
        extended = extended || n.extended;
        nodes.push_back(std::move(node));
    }
    Json out = Json::MakeObject();
    out.set("id", panel.id);
    out.set("title", panel.title);
    out.set("owner", panel.owner);
    out.set("revision", revision);
    out.set("extended", extended);
    out.set("nodes", std::move(nodes));
    return out;
}

Json WidgetCapabilitiesJson() {
    Json features = Json::MakeArray();
    for (const char* f : { "update_nodes", "remove_nodes", "severity", "tooltip", "paging", "event_row",
                           "coalesced_render", "stable_tags" })
        features.push_back(f);
    Json limits = Json::MakeObject();
    limits.set("max_panels", SCO_WIDGETS_MAX_PANELS);
    limits.set("max_nodes", SCO_WIDGETS_MAX_NODES);
    limits.set("max_total_nodes", SCO_WIDGETS_MAX_TOTAL_NODES);
    limits.set("max_items", SCO_WIDGETS_MAX_ITEMS);
    limits.set("max_columns", SCO_WIDGETS_MAX_COLUMNS);
    limits.set("max_text", SCO_WIDGETS_MAX_TEXT);
    limits.set("max_events", SCO_WIDGETS_MAX_EVENTS);
    limits.set("max_tag", SCO_WIDGETS_MAX_TAG);
    Json events = Json::MakeArray();
    for (const char* e : { "button", "toggle", "slider", "combo", "list", "text_input", "page" }) events.push_back(e);
    Json out = Json::MakeObject();
    out.set("service", SCO_WIDGETS_NAME);
    out.set("version", "1.2");
    out.set("features", std::move(features));
    out.set("limits", std::move(limits));
    out.set("events", std::move(events));
    return out;
}

WidgetEventRequest ParseWidgetEvent(const Json& params) {
    if (!params.is_object()) BadParams("params must be an object");
    WidgetEventRequest req;
    const Json* panel = params.find("panel");
    if (!panel || !panel->is_string() || panel->as_string().empty()) BadParams("panel must be a panel id");
    req.panel = panel->as_string();

    const Json* nodeId = params.find("node_id");
    if (!nodeId) BadParams("node_id is required");
    req.event.node_id = static_cast<uint32_t>(IntegerField(*nodeId, "node_id", 1, std::numeric_limits<uint32_t>::max()));

    const Json* kind = params.find("kind");
    if (!kind || !kind->is_string()) BadParams("kind must be a string");
    req.event.kind = WidgetKindFromName(kind->as_string());
    switch (req.event.kind) {
        case SCO_WIDGET_BUTTON: case SCO_WIDGET_TOGGLE: case SCO_WIDGET_SLIDER: case SCO_WIDGET_COMBO:
        case SCO_WIDGET_LIST: case SCO_WIDGET_TEXT_INPUT: case SCO_WIDGET_EVENT_PAGE: break;
        default: BadParams("kind must be button, toggle, slider, combo, list, text_input or page");
    }

    if (const Json* value = params.find("value")) {
        if (!value->is_number() || !std::isfinite(value->as_number())) BadParams("value must be a finite number");
        req.event.value = value->as_number();
    }
    if (const Json* index = params.find("index"))
        req.event.index = static_cast<int32_t>(IntegerField(*index, "index", -1, std::numeric_limits<int32_t>::max()));
    if (const Json* text = params.find("text")) {
        if (!text->is_string()) BadParams("text must be a string");
        req.event.text = text->as_string();
    }
    if (const Json* row = params.find("row"))
        req.event.row = static_cast<int32_t>(IntegerField(*row, "row", 0, std::numeric_limits<int32_t>::max()));

    const uint32_t k = req.event.kind;
    const bool numeric = k == SCO_WIDGET_TOGGLE || k == SCO_WIDGET_SLIDER;
    const bool selection = k == SCO_WIDGET_COMBO || k == SCO_WIDGET_LIST;
    if (!numeric && params.find("value")) BadParams("value only applies to toggle and slider events");
    if (numeric && !params.find("value")) BadParams("value is required for toggle and slider events");
    if (!selection && params.find("index")) BadParams("index only applies to combo and list events");
    if (selection && !params.find("index")) BadParams("index is required for combo and list events");
    if (k != SCO_WIDGET_TEXT_INPUT && params.find("text")) BadParams("text only applies to text_input events");
    if (k == SCO_WIDGET_TEXT_INPUT && !params.find("text")) BadParams("text is required for text_input events");
    if (k != SCO_WIDGET_EVENT_PAGE && params.find("row")) BadParams("row only applies to page events");
    if (k == SCO_WIDGET_EVENT_PAGE && !params.find("row")) BadParams("row is required for page events");
    return req;
}

void CheckWidgetEvent(const Json& panel, const sco::widgets::Event& event) {
    const Json* nodes = panel.find("nodes");
    const Json* node = nullptr;
    for (size_t i = 0; nodes && i < nodes->size(); ++i)
        if (NumberOf(nodes->at(i), "id") == static_cast<double>(event.node_id)) node = &nodes->at(i);
    if (!node) throw HandlerError(widgets_error::kNotFound, "no node " + std::to_string(event.node_id) + " in the panel");

    const auto kind = static_cast<uint32_t>(NumberOf(*node, "kind_code"));
    const Json* items = node->find("items");
    const size_t itemCount = items ? items->size() : 0;
    const auto columns = static_cast<size_t>(NumberOf(*node, "columns"));
    const size_t rows = kind == SCO_WIDGET_TABLE ? (columns ? itemCount / columns : 0) : (kind == SCO_WIDGET_LIST ? itemCount : 0);

    if (event.kind == SCO_WIDGET_EVENT_PAGE) {
        if (NumberOf(*node, "visible_rows") <= 0) BadParams("page events need a paged list or table");
        if (static_cast<size_t>(event.row) >= rows) BadParams("row is past the last row");
    } else if (event.kind != kind) {
        BadParams(std::string("a ") + WidgetKindName(event.kind) + " event cannot target a " + WidgetKindName(kind) + " node");
    }
    // The host also refuses events below a disabled ROW/COLUMN; that ancestor check stays with it.
    if (const Json* disabled = node->find("disabled"); disabled && disabled->as_bool()) BadParams("the node is disabled");

    switch (event.kind) {
        case SCO_WIDGET_TOGGLE:
            if (event.value != 0 && event.value != 1) BadParams("toggle value must be 0 or 1");
            break;
        case SCO_WIDGET_SLIDER:
            if (event.value < NumberOf(*node, "minimum") || event.value > NumberOf(*node, "maximum"))
                BadParams("slider value is outside [minimum, maximum]");
            break;
        case SCO_WIDGET_COMBO:
        case SCO_WIDGET_LIST:
            if (event.index >= 0 && static_cast<size_t>(event.index) >= itemCount) BadParams("index is past the last item");
            break;
        case SCO_WIDGET_TEXT_INPUT:
            if (event.text.size() > SCO_WIDGETS_MAX_TEXT || event.text.find('\0') != std::string::npos)
                BadParams("text is longer than 255 bytes or holds NUL");
            break;
        default: break;
    }
}

void StudioWidgetRenderer::Render(const sco::widgets::Panel& panel, std::vector<sco::widgets::Event>& events) {
    const bool rebuilt = sco::widgets::SnapshotBuilds() != buildsBefore_;
    if (rebuilt || published_.find(panel.id) == published_.end()) {
        Json snapshot = WidgetPanelToJson(panel, ++revision_);
        if (broadcaster_) broadcaster_->broadcast("widgets.snapshot", snapshot);
        published_[panel.id] = std::move(snapshot);
    }
    auto queued = pending_.find(panel.id);
    if (queued != pending_.end()) {
        events = std::move(queued->second);
        pending_.erase(queued);
    }
}

sco::Result StudioWidgetRenderer::RenderOne(const std::string& id) {
    buildsBefore_ = sco::widgets::SnapshotBuilds();
    return sco::widgets::RenderPanel(id.c_str(), *this);
}

void StudioWidgetRenderer::Pump() {
    std::set<std::string> live;
    for (const auto& panel : sco::widgets::Panels()) {
        live.insert(panel.id);
        (void)RenderOne(panel.id);
    }
    for (auto it = published_.begin(); it != published_.end();) {
        if (live.count(it->first)) {
            ++it;
            continue;
        }
        Json data = Json::MakeObject();
        data.set("id", it->first);
        if (broadcaster_) broadcaster_->broadcast("widgets.removed", std::move(data));
        pending_.erase(it->first);
        it = published_.erase(it);
    }
}

void StudioWidgetRenderer::Tick(uint32_t nowMs, uint32_t intervalMs) {
    if (pumped_ && nowMs - lastPumpMs_ < intervalMs) return;
    pumped_ = true;
    lastPumpMs_ = nowMs;
    Pump();
}

sco::Result StudioWidgetRenderer::Submit(const WidgetEventRequest& request) {
    if (!sco::widgets::Started()) throw HandlerError(rpc_error::kUnavailable, "sco.widgets is not running");
    const Json* panel = FindPanel(request.panel);
    if (!panel) {
        Pump();
        panel = FindPanel(request.panel);
    }
    if (!panel) throw HandlerError(widgets_error::kNotFound, "no panel " + request.panel);
    CheckWidgetEvent(*panel, request.event);

    pending_[request.panel].push_back(request.event);
    const sco::Result dispatched = RenderOne(request.panel);
    pending_.erase(request.panel);
    if (dispatched == sco::Result::Ok) (void)RenderOne(request.panel);
    if (dispatched == sco::Result::NotFound || dispatched == sco::Result::Unavailable) Pump();
    return dispatched;
}

Json StudioWidgetRenderer::PanelsJson() const {
    Json out = Json::MakeArray();
    for (const auto& [id, snapshot] : published_) out.push_back(snapshot);
    return out;
}

const Json* StudioWidgetRenderer::FindPanel(const std::string& id) const {
    const auto it = published_.find(id);
    return it == published_.end() ? nullptr : &it->second;
}

}  // namespace scuba::hostd
