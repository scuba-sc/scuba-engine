#pragma once
// The sco.widgets bridge: a sco::widgets::Renderer that publishes every plugin panel to the studio
// as JSON and feeds the studio's interaction events back through sco::widgets::RenderPanel, so the
// host validates them and calls the owning plugin exactly as an in-game renderer would.
// Wire format: docs/protocol.md, "widgets".
//
// Game thread only (the hostd main loop and its handlers), except Broadcaster::broadcast.
#include "hostd/handlers.h"
#include "hostd/json.h"

#include "sco/widgets.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scuba::hostd {

namespace widgets_error {
constexpr int kNotFound = -32010;   // no such panel, or no such node in it
}  // namespace widgets_error

// Wire names of node kinds ("label" .. "column"); "" for an unknown kind.
const char* WidgetKindName(uint32_t kind);
// Node kind for a wire name, or SCO_WIDGET_EVENT_PAGE for "page"; 0 when unknown.
uint32_t WidgetKindFromName(const std::string& name);

// One panel snapshot as sent to the studio. TEXT_INPUT nodes flagged PASSWORD carry no text.
Json WidgetPanelToJson(const sco::widgets::Panel& panel, uint64_t revision);
// The sco.widgets version, feature names and limits this host advertises.
Json WidgetCapabilitiesJson();

struct WidgetEventRequest {
    std::string panel;
    sco::widgets::Event event;
};
// Parses widgets.event params. Throws HandlerError(kInvalidParams) when malformed.
WidgetEventRequest ParseWidgetEvent(const Json& params);
// Checks an event against a published panel snapshot (WidgetPanelToJson shape). Throws
// HandlerError: kNotFound for an absent node, kInvalidParams for a kind/range mismatch or a
// disabled node. The host validates again before dispatch; this only gives the studio an answer.
void CheckWidgetEvent(const Json& panel, const sco::widgets::Event& event);

class StudioWidgetRenderer final : public sco::widgets::Renderer {
public:
    explicit StudioWidgetRenderer(Broadcaster* broadcaster) : broadcaster_(broadcaster) {}

    // Renders every live panel: publishes "widgets.snapshot" for each panel whose snapshot was
    // rebuilt (sco::widgets::SnapshotBuilds advanced) and "widgets.removed" for panels that are gone.
    void Pump();
    // Pump at most every intervalMs of nowMs (the hostd tick hook).
    void Tick(uint32_t nowMs, uint32_t intervalMs = 50);

    // Queues a studio event for its panel and renders that panel at once, so the plugin's callback
    // runs before this returns, then renders again to publish the result. Returns the sco::Result
    // of the dispatching render. Throws HandlerError as CheckWidgetEvent, or kUnavailable.
    sco::Result Submit(const WidgetEventRequest& request);

    // The latest published snapshots, in panel id order.
    Json PanelsJson() const;
    const Json* FindPanel(const std::string& id) const;
    uint64_t revision() const { return revision_; }

    void Render(const sco::widgets::Panel& panel, std::vector<sco::widgets::Event>& events) override;

private:
    sco::Result RenderOne(const std::string& id);

    Broadcaster* broadcaster_;
    std::map<std::string, Json> published_;
    std::map<std::string, std::vector<sco::widgets::Event>> pending_;
    uint64_t buildsBefore_ = 0;
    uint64_t revision_ = 0;
    uint32_t lastPumpMs_ = 0;
    bool pumped_ = false;
};

}  // namespace scuba::hostd
