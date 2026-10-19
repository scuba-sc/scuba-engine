// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/plugin_designer.h"
#include "hostd/designer_export.h"
#include "hostd/net_sim/process.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace scuba::ui {
namespace {
using hostd::Json;
const char* const kinds[]{"Label", "Button", "Toggle", "Slider", "TextInput"};
Json Widget(const char* kind, const std::string& key, const char* label) {
    Json n = Json::MakeObject();
    n.set("key", key); n.set("kind", kind); n.set("label", label);
    n.set("disabled", false); n.set("password", false); n.set("value", 0);
    n.set("minimum", 0); n.set("maximum", 1); n.set("index", -1); n.set("text", "");
    n.set("items", Json::MakeArray()); n.set("columns", 2); n.set("tooltip", "");
    n.set("severity", 0); n.set("firstRow", 0); n.set("visibleRows", 0);
    n.set("children", Json::MakeArray()); return n;
}
}
void PluginDesigner::Draw(bool* open) {
    if (!placed_)
        if (const auto* assets = ImGui::FindWindowByName("Assets"); assets && assets->DockId) {
            const auto* designer = ImGui::FindWindowByName("Designer");
            if (!designer || !designer->DockId) ImGui::SetNextWindowDockID(assets->DockId, ImGuiCond_Always);
            placed_ = true;
        }
    if (!ImGui::Begin("Designer", open)) { ImGui::End(); return; }
    if (exporting_.valid() && exporting_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        status_ = exporting_.get();
    ImGui::TextDisabled("SDK widget plugin / C export");
    ImGui::TextWrapped("Edit a panel, preview it, then export a fresh plugin. No game connection.");
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##plugin-id", "Plugin ID", id_.data(), id_.size());
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##panel-title", "Panel title", title_.data(), title_.size());
    ImGui::SeparatorText("Widget tree");
    ImGui::SetNextItemWidth(-1); ImGui::Combo("##add-kind", &addKind_, kinds, 5);
    ImGui::BeginDisabled(nodes_.size() >= 64);
    if (ImGui::Button("Add widget", {-1, 0})) {
        Node n; n.kind = addKind_; n.value = n.kind == 2 ? 0.0f : 0.5f;
        std::snprintf(n.label.data(), n.label.size(), "%s", kinds[n.kind]);
        nodes_.push_back(n); selected_ = static_cast<int>(nodes_.size()) - 1;
    }
    ImGui::EndDisabled();
    ImGui::BeginChild("designer-nodes", {0, 120}, ImGuiChildFlags_Borders);
    for (size_t i = 0; i < nodes_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(nodes_[i].label.data(), selected_ == static_cast<int>(i))) selected_ = static_cast<int>(i);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (selected_ >= 0 && selected_ < static_cast<int>(nodes_.size())) {
        auto& n = nodes_[static_cast<size_t>(selected_)];
        ImGui::SeparatorText("Selected widget");
        ImGui::TextDisabled("%s", kinds[n.kind]);
        ImGui::SetNextItemWidth(-1); ImGui::InputText("##widget-label", n.label.data(), n.label.size());
        ImGui::Checkbox("Disabled", &n.disabled);
        if (n.kind == 2) { bool value = n.value != 0; if (ImGui::Checkbox("Initial value", &value)) n.value = value ? 1.0f : 0.0f; }
        if (n.kind == 3) {
            ImGui::TextDisabled("Minimum"); ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##minimum", &n.minimum);
            ImGui::TextDisabled("Maximum"); ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##maximum", &n.maximum);
            ImGui::TextDisabled("Initial value"); ImGui::SetNextItemWidth(-1); ImGui::InputFloat("##initial-value", &n.value);
        }
        if (n.kind == 4) { ImGui::TextDisabled("Initial text"); ImGui::SetNextItemWidth(-1); ImGui::InputText("##initial-text", n.text.data(), n.text.size()); }
        if (ImGui::Button("Remove")) { nodes_.erase(nodes_.begin() + selected_); selected_ = -1; }
        ImGui::SameLine(); ImGui::BeginDisabled(selected_ <= 0);
        if (ImGui::Button("Up")) { std::swap(nodes_[static_cast<size_t>(selected_)], nodes_[static_cast<size_t>(selected_ - 1)]); --selected_; }
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(selected_ < 0 || selected_ + 1 >= static_cast<int>(nodes_.size()));
        if (ImGui::Button("Down")) { std::swap(nodes_[static_cast<size_t>(selected_)], nodes_[static_cast<size_t>(selected_ + 1)]); ++selected_; }
        ImGui::EndDisabled();
    }
    ImGui::SeparatorText("Preview");
    for (size_t i = 0; i < nodes_.size(); ++i) {
        auto& n = nodes_[i]; ImGui::PushID(static_cast<int>(i)); ImGui::BeginDisabled(n.disabled);
        switch (n.kind) {
        case 0: ImGui::TextWrapped("%s", n.label.data()); break;
        case 1: ImGui::Button(n.label.data()); break;
        case 2: { bool v = n.value != 0; ImGui::Checkbox(n.label.data(), &v); break; }
        case 3: {
            float v = n.value;
            if (n.minimum <= n.maximum && v >= n.minimum && v <= n.maximum) {
                ImGui::TextUnformatted(n.label.data());
                ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##preview-slider", &v, n.minimum, n.maximum);
            } else ImGui::TextWrapped("Fix the slider range before preview or export.");
            break;
        }
        case 4: { auto text = n.text; ImGui::TextUnformatted(n.label.data()); ImGui::SetNextItemWidth(-1); ImGui::InputText("##preview-text", text.data(), text.size()); break; }
        }
        ImGui::EndDisabled(); ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(exporting_.valid() || nodes_.empty());
    if (ImGui::Button("Export C plugin", {-1, 0})) {
        Json design = Json::MakeObject(), column = Widget("Column", "root", ""), children = Json::MakeArray();
        design.set("id", id_.data()); design.set("title", title_.data());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const auto& n = nodes_[i]; auto j = Widget(kinds[n.kind], std::to_string(i + 1), n.label.data());
            j.set("disabled", n.disabled); j.set("value", n.value); j.set("minimum", n.minimum);
            j.set("maximum", n.maximum); j.set("text", n.text.data()); children.push_back(std::move(j));
        }
        column.set("children", std::move(children)); Json nodes = Json::MakeArray(); nodes.push_back(std::move(column));
        design.set("nodes", std::move(nodes));
        status_ = "Exporting through published SDK scaffold...";
        exporting_ = std::async(std::launch::async, [design] {
            try {
                hostd::net_sim::Process generator(SCUBA_DESIGNER_BUN, {SCUBA_DESIGNER_GENERATOR});
                Json request = Json::MakeObject(); request.set("design", design);
                const auto response = generator.Call(request);
                const auto* source = response.find("source");
                if (!source || !source->is_string()) throw std::runtime_error("Generator returned no source");
                Json params = Json::MakeObject(); params.set("id", *design.find("id"));
                params.set("language", "c"); params.set("source", *source);
                const hostd::DesignerExportOptions options{SCUBA_DESIGNER_SDK_ROOT,
                    std::filesystem::current_path() / "exports", SCUBA_DESIGNER_PYTHON};
                const auto out = hostd::ExportDesignerPlugin(params, options);
                return "Exported exports/" + out.find("folder")->as_string() + "; build target check before loading.";
            } catch (const std::exception& e) { return std::string("Export refused: ") + e.what(); }
        });
    }
    ImGui::EndDisabled();
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();
}
}
