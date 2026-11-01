// Exercise the bridge against the released host's real widget validation and callbacks.
#include "check.h"
#include "hostd/widgets_bridge.h"
#include "sco/host.h"

#include <string>

using namespace scuba::hostd;

namespace {
template<class Node>
constexpr bool SnapshotHasPresentation() {
#if defined(SCO_WIDGETS_VERSION_1_3) && defined(SCO_WIDGETS_FEATURE_NUMERIC_PRESENTATION)
    return requires(const Node& node) { node.presentation; };
#else
    return false;
#endif
}

int calls = 0;
double lastValue = 0;
void OnEvent(const char*, const sco_widget_event* event, void*) {
    ++calls;
    lastValue = event->value;
}

bool HasFeature(const Json& caps, const std::string& name) {
    const auto* features = caps.find("features");
    for (size_t i = 0; features && i < features->size(); ++i)
        if (features->at(i).as_string() == name) return true;
    return false;
}

WidgetEventRequest Slide(const char* panel, double value) {
    WidgetEventRequest event;
    event.panel = panel;
    event.event.node_id = 1;
    event.event.kind = SCO_WIDGET_SLIDER;
    event.event.value = value;
    return event;
}

void Refuses(StudioWidgetRenderer& renderer, const WidgetEventRequest& event) {
    const int before = calls;
    bool refused = false;
    try { (void)renderer.Submit(event); }
    catch (const HandlerError& error) { refused = error.code() == rpc_error::kInvalidParams; }
    CHECK(refused);
    CHECK(calls == before);
}
}

int main() {
    sco::SetGameThread();
    CHECK(sco::host::BuildApi({"widget bridge test"}) != nullptr);
    auto* owner = sco::host::NewPlugin("numeric_probe");
    CHECK(owner != nullptr);
    CHECK(sco::widgets::Start() == sco::Result::Ok);
    const auto* table = sco::widgets::Table();
    const Json caps = WidgetCapabilitiesJson();
    constexpr bool numericPresentation = SnapshotHasPresentation<sco::widgets::Node>();
    CHECK(caps.find("version")->as_string() == (numericPresentation ? "1.3" : "1.2"));
    CHECK(HasFeature(caps, "numeric_presentation") == numericPresentation);

    // A 1.0-sized node retains finite-double semantics on a newer host.
    sco_widget_node legacy{};
    legacy.size = sizeof(legacy);
    legacy.kind = SCO_WIDGET_SLIDER;
    legacy.id = 1;
    legacy.label = "Number";
    legacy.value = 0.5;
    legacy.maximum = 1;
    CHECK(table->set_panel(owner, "numeric_probe.number", "Number", &legacy, 1, sizeof(legacy), OnEvent, nullptr) == SCO_OK);
    StudioWidgetRenderer renderer(nullptr);
    renderer.Pump();
    CHECK(renderer.Submit(Slide("numeric_probe.number", 0.25)) == sco::Result::Ok);
    CHECK(calls == 1 && lastValue == 0.25);
    Refuses(renderer, Slide("numeric_probe.number", 2));

#if defined(SCO_WIDGETS_VERSION_1_3) && defined(SCO_WIDGETS_FEATURE_NUMERIC_PRESENTATION)
    if constexpr (numericPresentation) {
        uint32_t hostCaps[SCO_WIDGETS_CAPS_1_2]{};
        hostCaps[0] = SCO_WIDGETS_CAPS_1_2;
        CHECK(table->capabilities(owner, hostCaps) == SCO_OK);
        CHECK((hostCaps[SCO_WIDGETS_CAP_FEATURES] & SCO_WIDGETS_FEATURE_NUMERIC_PRESENTATION) != 0);
        const Json* number = renderer.FindPanel("numeric_probe.number");
        CHECK(number && number->find("nodes")->at(0).find("presentation") &&
              number->find("nodes")->at(0).find("presentation")->as_number() == SCO_WIDGET_NUMERIC_NUMBER);

        for (const uint32_t mode : {SCO_WIDGET_NUMERIC_INTEGER, SCO_WIDGET_NUMERIC_INTEGER_PERCENT}) {
            sco_widget_node_1_3 integer{};
            integer.base.base.base = legacy;
            integer.base.base.base.size = sizeof(integer);
            integer.base.base.base.label = "Integer";
            integer.base.base.base.value = 50;
            integer.base.base.base.maximum = 100;
            integer.presentation = mode;
            CHECK(table->set_panel(owner, "numeric_probe.integer", "Integer", &integer.base.base.base,
                                   1, sizeof(integer), OnEvent, nullptr) == SCO_OK);
            renderer.Pump();
            const Json* panel = renderer.FindPanel("numeric_probe.integer");
            CHECK(panel && panel->find("nodes")->at(0).find("presentation") &&
                  panel->find("nodes")->at(0).find("presentation")->as_number() == mode);
            Refuses(renderer, Slide("numeric_probe.integer", 50.5));
            Refuses(renderer, Slide("numeric_probe.integer", -1));
            Refuses(renderer, Slide("numeric_probe.integer", 101));
            for (double value : {0.0, 73.0, 100.0}) {
                const int before = calls;
                CHECK(renderer.Submit(Slide("numeric_probe.integer", value)) == sco::Result::Ok);
                CHECK(calls == before + 1 && lastValue == value);
                panel = renderer.FindPanel("numeric_probe.integer");
                CHECK(panel && panel->find("nodes")->at(0).find("value")->as_number() == value);
            }
        }
    }
#endif
    if constexpr (!numericPresentation) {
        const Json* number = renderer.FindPanel("numeric_probe.number");
        CHECK(number && !number->find("nodes")->at(0).find("presentation"));
    }

    // Old snapshots have no presentation metadata and must keep fractional input valid.
    Json oldNode = Json::MakeObject();
    oldNode.set("id", 1).set("kind_code", SCO_WIDGET_SLIDER).set("minimum", 0).set("maximum", 1);
    Json oldNodes = Json::MakeArray();
    oldNodes.push_back(std::move(oldNode));
    Json oldPanel = Json::MakeObject();
    oldPanel.set("nodes", std::move(oldNodes));
    CheckWidgetEvent(oldPanel, Slide("numeric_probe.number", 0.125).event);

    sco::widgets::Stop();
    CHECK(sco::Release(owner) == sco::Result::Ok);
    return CheckExit("test_widgets_bridge");
}
