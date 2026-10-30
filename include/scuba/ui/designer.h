// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scuba::ui {
// A bounded native authoring subset; not the complete browser Designer schema.
enum class DesignerKind { Column, Row, Label, Button, Toggle, Slider, TextInput };
struct DesignerNode {
    uint32_t key = 1;
    DesignerKind kind = DesignerKind::Column;
    std::string label;
    bool disabled = false;
    double value = 0;
    double minimum = 0;
    double maximum = 1;
    std::string text;
    std::vector<DesignerNode> children;
};
struct DesignerDocument {
    std::string id = "instrument";
    std::string title = "Instrument control";
    std::vector<DesignerNode> nodes = {DesignerNode{}};
};
using DesignerExporter = std::function<std::string(const std::string& id, const std::string& language,
                                                  const std::string& source)>;
class DesignerModel {
public:
    const DesignerDocument& document() const noexcept { return document_; }
    const std::string& status() const noexcept { return status_; }
    bool Apply(const DesignerDocument& draft);
    bool Insert(uint32_t parent, DesignerKind kind); // parent 0 inserts a root
    bool Erase(uint32_t key);
    bool Move(uint32_t key, int offset); // adjacent siblings only
    bool Undo();
    bool Redo();
    std::string GenerateCpp() const;
    // Callback must return a nonempty verified folder receipt or throw.
    bool Export(const DesignerExporter& exporter);
private:
    std::vector<DesignerDocument> undo_, redo_;
    DesignerDocument document_;
    std::string status_;
};
// Main-thread ImGui view. Build designer.cpp with SCUBA_DESIGNER_IMGUI to define Draw.
class DesignerDock {
public:
    DesignerModel model;
    DesignerExporter exporter;
    bool open = true;
    void Draw();
private:
    uint32_t selected_ = 1;
    int palette_ = 3;
};
} // namespace scuba::ui
