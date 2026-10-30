// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/designer.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <iomanip>
#include <locale>
#ifdef SCUBA_DESIGNER_IMGUI
#include "imgui.h"
#include <cstring>
#endif
namespace scuba::ui {
namespace {
bool Text(const std::string& s, size_t limit) {
    if (s.size() > limit) return false;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i++]);
        if (!c) return false;
        if (c < 128) continue;
        unsigned count = 0;
        uint32_t value = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31u; minimum = 128; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15u; minimum = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7u; minimum = 65536; }
        else return false;
        while (count--) {
            if (i == s.size()) return false;
            const auto tail = static_cast<unsigned char>(s[i++]);
            if ((tail & 0xc0u) != 0x80u) return false;
            value = (value << 6u) | (tail & 63u);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool Container(DesignerKind k) { return k == DesignerKind::Column || k == DesignerKind::Row; }
bool Nodes(const std::vector<DesignerNode>& nodes, std::set<uint32_t>& keys, unsigned depth) {
    if (!nodes.empty() && depth > 16) return false;
    for (const auto& n : nodes) {
        if (!n.key || !keys.insert(n.key).second || keys.size() > 256 ||
            n.kind < DesignerKind::Column || n.kind > DesignerKind::TextInput ||
            !Text(n.label, 255) || !Text(n.text, 255)) return false;
        if (Container(n.kind) ? !n.label.empty() : !n.children.empty()) return false;
        if (!Container(n.kind) && n.kind != DesignerKind::Label && n.label.empty()) return false;
        if (n.kind == DesignerKind::Slider && (!std::isfinite(n.value) || !std::isfinite(n.minimum) ||
            !std::isfinite(n.maximum) || n.value < n.minimum || n.value > n.maximum || n.minimum > n.maximum)) return false;
        if (n.kind == DesignerKind::Toggle && n.value != 0 && n.value != 1) return false;
        if (!Nodes(n.children, keys, depth + 1)) return false;
    }
    return true;
}
}

bool DesignerModel::Apply(const DesignerDocument& draft) {
    if (draft.id.empty() || draft.id.size() > 31 ||
        draft.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
        draft.id == "sco" || draft.id == "host" || draft.id == "menu" || draft.id == "game") {
        status_ = "Invalid or reserved plugin id";
        return false;
    }
    std::set<uint32_t> keys;
    if (!Text(draft.title, 63) || draft.title.find_first_not_of(" \t\r\n") == std::string::npos ||
        draft.nodes.empty() || !Nodes(draft.nodes, keys, 1)) {
        status_ = "Invalid design: UTF-8/title, unique keys, 1..256 nodes, depth <=16, labels or values";
        return false;
    }
    undo_.push_back(document_);
    if (undo_.size() > 50) undo_.erase(undo_.begin());
    redo_.clear();
    document_ = draft;
    status_ = "Design updated";
    return true;
}
namespace {
std::vector<DesignerNode>* Siblings(std::vector<DesignerNode>& nodes, uint32_t key) {
    for (auto& n : nodes) {
        if (n.key == key) return &nodes;
        if (auto* found = Siblings(n.children, key)) return found;
    }
    return nullptr;
}
DesignerNode* Find(std::vector<DesignerNode>& nodes, uint32_t key) {
    auto* siblings = Siblings(nodes, key);
    if (!siblings) return nullptr;
    for (auto& n : *siblings) if (n.key == key) return &n;
    return nullptr;
}
void Keys(const std::vector<DesignerNode>& nodes, std::set<uint32_t>& keys) {
    for (const auto& n : nodes) { keys.insert(n.key); Keys(n.children, keys); }
}
}
bool DesignerModel::Insert(uint32_t parent, DesignerKind kind) {
    auto draft = document_;
    auto* owner = Find(draft.nodes, parent);
    if (parent && (!owner || !Container(owner->kind))) { status_ = "Select a Row or Column parent"; return false; }
    std::set<uint32_t> keys;
    Keys(draft.nodes, keys);
    DesignerNode n;
    n.key = 1;
    while (keys.count(n.key)) ++n.key;
    n.kind = kind;
    static const char* names[] = {"", "", "Label", "Button", "Toggle", "Slider", "TextInput"};
    if (kind >= DesignerKind::Column && kind <= DesignerKind::TextInput) n.label = names[static_cast<size_t>(kind)];
    (owner ? owner->children : draft.nodes).push_back(n);
    return Apply(draft);
}
bool DesignerModel::Erase(uint32_t key) {
    auto draft = document_;
    auto* siblings = Siblings(draft.nodes, key);
    if (!siblings) return false;
    siblings->erase(std::remove_if(siblings->begin(), siblings->end(), [key](const auto& n) { return n.key == key; }), siblings->end());
    return Apply(draft);
}
bool DesignerModel::Move(uint32_t key, int offset) {
    if (offset != -1 && offset != 1) return false;
    auto draft = document_;
    auto* siblings = Siblings(draft.nodes, key);
    if (!siblings) return false;
    for (size_t i = 0; i < siblings->size(); ++i) {
        if ((*siblings)[i].key != key) continue;
        if ((offset < 0 && i == 0) || (offset > 0 && i + 1 == siblings->size())) return false;
        std::swap((*siblings)[i], (*siblings)[offset < 0 ? i - 1 : i + 1]);
        return Apply(draft);
    }
    return false;
}
bool DesignerModel::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(document_); document_ = std::move(undo_.back()); undo_.pop_back();
    status_ = "Undo"; return true;
}
bool DesignerModel::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(document_); document_ = std::move(redo_.back()); redo_.pop_back();
    status_ = "Redo"; return true;
}
namespace {
std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char raw : s) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32 || c == 127) {
            out += '\\'; out += static_cast<char>('0' + ((c >> 6) & 7));
            out += static_cast<char>('0' + ((c >> 3) & 7)); out += static_cast<char>('0' + (c & 7));
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}
std::string Number(double value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(17) << value;
    return s.str();
}
void Generate(const std::vector<DesignerNode>& nodes, std::string& bindings, std::string& body, unsigned depth) {
    static const char* names[] = {"Column", "Row", "Label", "Button", "Toggle", "Slider", "TextInput"};
    for (const auto& n : nodes) {
        const auto variable = "v" + std::to_string(n.key);
        const std::string indent(depth * 4u, ' ');
        std::string args = Quote(n.label);
        if (n.kind == DesignerKind::Toggle) {
            bindings += "static bool " + variable + " = " + (n.value == 1 ? "true" : "false") + ";\n";
            args += ", &" + variable;
        } else if (n.kind == DesignerKind::Slider) {
            bindings += "static double " + variable + " = " + Number(n.value) + ";\n";
            args += ", &" + variable + ", " + Number(n.minimum) + ", " + Number(n.maximum);
        } else if (n.kind == DesignerKind::TextInput) {
            bindings += "static std::string " + variable + " = " + Quote(n.text) + ";\n";
            args += ", &" + variable;
        }
        body += indent + "panel." + names[static_cast<size_t>(n.kind)] + "(";
        if (Container(n.kind)) {
            body += "[&] {\n";
            Generate(n.children, bindings, body, depth + 1);
            body += indent + "}";
        } else body += args;
        body += n.disabled ? ").Disabled();\n" : ");\n";
    }
}
}
std::string DesignerModel::GenerateCpp() const {
    std::string bindings, body;
    Generate(document_.nodes, bindings, body, 1);
    const auto panel = Quote(document_.id + ".main");
    return "// Generated native Designer subset; synthetic panel only.\n"
        "#include \"scosdk/scosdk.hpp\"\n#include \"scosdk/menu_builder.hpp\"\n#include <string>\n" + bindings +
        "static scosdk::PanelTree BuildDesignedPanel() {\n    scosdk::PanelBuilder panel;\n" + body +
        "    return panel.Build();\n}\nnamespace {\nclass DesignedPlugin final : public sco::sdk::Plugin {\npublic:\n"
        "    sco_result OnLoad() override {\n        if (widgets_.Open(*this) != SCO_OK) return SCO_OK;\n"
        "        auto next = BuildDesignedPanel();\n        if (!next) return next.Result();\n"
        "        const auto result = next.Submit(widgets_, " + panel + ", " + Quote(document_.title) + ");\n"
        "        if (result == SCO_OK) tree_ = std::move(next);\n        return result;\n    }\n"
        "    void OnUnload() override { if (widgets_) (void)widgets_.RemovePanel(" + panel + "); }\n"
        "private:\n    sco::sdk::Widgets widgets_;\n    scosdk::PanelTree tree_;\n};\n}\n"
        "SCO_PLUGIN(DesignedPlugin, " + Quote(document_.id) + ", \"0.1.0\", \"the maintainer\");\n";
}
bool DesignerModel::Export(const DesignerExporter& exporter) {
    if (!exporter) { status_ = "Export unavailable: no hostd exporter configured"; return false; }
    try {
        const auto receipt = exporter(document_.id, "cpp", GenerateCpp());
        if (receipt.empty()) { status_ = "Export failed: empty folder receipt"; return false; }
        status_ = "Exported: " + receipt;
        return true;
    } catch (const std::exception& error) { status_ = std::string("Export failed: ") + error.what(); return false; }
}
#ifdef SCUBA_DESIGNER_IMGUI
namespace {
constexpr const char* kNames[] = {"Column", "Row", "Label", "Button", "Toggle", "Slider", "TextInput"};
bool Input(const char* label, std::string& value, size_t limit = 255) {
    char buffer[256]{};
    std::memcpy(buffer, value.data(), std::min(value.size(), limit));
    if (!ImGui::InputText(label, buffer, limit + 1)) return false;
    value = buffer; return true;
}
void Outline(const std::vector<DesignerNode>& nodes, uint32_t& selected) {
    for (const auto& n : nodes) {
        ImGui::PushID(static_cast<int>(n.key));
        const std::string label = std::string(kNames[static_cast<size_t>(n.kind)]) + " / " + n.label;
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
        if (n.key == selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (n.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        const bool expanded = ImGui::TreeNodeEx("node", flags, "%s", label.c_str());
        if (ImGui::IsItemClicked()) selected = n.key;
        if (expanded) { Outline(n.children, selected); ImGui::TreePop(); }
        ImGui::PopID();
    }
}
bool Preview(std::vector<DesignerNode>& nodes, bool row = false) {
    bool changed = false;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& n = nodes[i];
        if (row && i) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(n.key));
        ImGui::BeginDisabled(n.disabled);
        const std::string label = n.label + "###control";
        if (Container(n.kind)) {
            ImGui::BeginGroup();
            changed |= Preview(n.children, n.kind == DesignerKind::Row);
            ImGui::EndGroup();
        } else if (n.kind == DesignerKind::Label) ImGui::TextUnformatted(n.label.c_str());
        else if (n.kind == DesignerKind::Button) {
            if (ImGui::Button(label.c_str())) ImGui::SetTooltip("Synthetic activation; no game command");
        } else if (n.kind == DesignerKind::Toggle) {
            bool value = n.value == 1;
            if (ImGui::Checkbox(label.c_str(), &value)) { n.value = value ? 1 : 0; changed = true; }
        } else if (n.kind == DesignerKind::Slider) {
            ImGui::SetNextItemWidth(180);
            changed |= ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &n.value, &n.minimum, &n.maximum);
        } else if (n.kind == DesignerKind::TextInput) changed |= Input(label.c_str(), n.text);
        ImGui::EndDisabled(); ImGui::PopID();
    }
    return changed;
}
}
void DesignerDock::Draw() {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(600, 800), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Designer", &open)) { ImGui::End(); return; }
    ImGui::TextWrapped("Native C++ subset / synthetic preview. No game commands or plugin installation.");
    if (ImGui::Button("Undo")) model.Undo();
    ImGui::SameLine(); if (ImGui::Button("Redo")) model.Redo();
    if (ImGui::CollapsingHeader("Structure", ImGuiTreeNodeFlags_DefaultOpen)) {
        Outline(model.document().nodes, selected_);
        ImGui::Combo("Widget", &palette_, kNames, 7);
        if (ImGui::Button("Insert child")) model.Insert(selected_, static_cast<DesignerKind>(palette_));
        ImGui::SameLine(); if (ImGui::Button("Insert root")) model.Insert(0, static_cast<DesignerKind>(palette_));
        if (ImGui::Button("Move up")) model.Move(selected_, -1);
        ImGui::SameLine(); if (ImGui::Button("Move down")) model.Move(selected_, 1);
        ImGui::SameLine(); if (ImGui::Button("Delete")) {
            if (model.Erase(selected_)) selected_ = model.document().nodes[0].key;
        }
    }
    if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto draft = model.document();
        bool changed = Input("Plugin id", draft.id, 31);
        changed |= Input("Panel title", draft.title, 63);
        if (auto* n = Find(draft.nodes, selected_)) {
            if (!Container(n->kind)) changed |= Input("Label", n->label);
            changed |= ImGui::Checkbox("Disabled", &n->disabled);
            if (n->kind == DesignerKind::Toggle) {
                bool value = n->value == 1;
                if (ImGui::Checkbox("Value", &value)) { n->value = value ? 1 : 0; changed = true; }
            }
            if (n->kind == DesignerKind::Slider) {
                changed |= ImGui::InputDouble("Minimum", &n->minimum);
                changed |= ImGui::InputDouble("Maximum", &n->maximum);
                changed |= ImGui::InputDouble("Value", &n->value);
            }
            if (n->kind == DesignerKind::TextInput) changed |= Input("Text", n->text);
        }
        if (changed) model.Apply(draft);
    }
    if (ImGui::CollapsingHeader("Live preview", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto draft = model.document();
        ImGui::PushID("preview");
        if (Preview(draft.nodes)) model.Apply(draft);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("C++ source", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto source = model.GenerateCpp();
        if (ImGui::Button("Copy source")) ImGui::SetClipboardText(source.c_str());
        ImGui::BeginDisabled(!exporter);
        if (ImGui::Button("Export plugin folder")) model.Export(exporter);
        ImGui::EndDisabled();
        if (!exporter) ImGui::TextWrapped("Folder export unavailable: configure the hostd Designer exporter.");
        ImGui::BeginChild("source", ImVec2(0, 180), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(source.c_str()); ImGui::EndChild();
    }
    ImGui::TextWrapped("%s", model.status().c_str());
    ImGui::End();
}
#endif
} // namespace scuba::ui
