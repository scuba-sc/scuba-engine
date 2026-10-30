// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"
#include <stdexcept>
#include "scuba/ui/designer.h"
#ifdef SCUBA_DESIGNER_EXPORT_TEST
#include "hostd/designer_export.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#endif
#ifdef SCUBA_DESIGNER_IMGUI
#include "imgui.h"
#endif
using namespace scuba::ui;
int main() {
    DesignerModel model;
    CHECK(model.document().id == "instrument");
    CHECK(model.document().nodes.size() == 1);
    CHECK(model.document().nodes[0].kind == DesignerKind::Column);
    auto draft = model.document();
    draft.title = "My panel";
    CHECK(model.Apply(draft));
    CHECK(model.document().title == "My panel");
    draft.id = "../escape";
    CHECK(!model.Apply(draft));
    CHECK(model.document().id == "instrument");
    draft = model.document();
    draft.title = " "; CHECK(!model.Apply(draft));
    draft = model.document(); draft.nodes.clear(); CHECK(!model.Apply(draft));
    draft = model.document(); draft.nodes[0].children.push_back(draft.nodes[0]); CHECK(!model.Apply(draft));
    draft = model.document(); draft.nodes[0].kind = DesignerKind::Button;
    draft.nodes[0].label = "Run"; draft.nodes[0].children.push_back(DesignerNode{}); CHECK(!model.Apply(draft));
    draft.nodes[0].children.clear(); draft.nodes[0].kind = DesignerKind::Slider;
    draft.nodes[0].minimum = 2; CHECK(!model.Apply(draft));
    draft = model.document(); draft.nodes[0].label = "container"; CHECK(!model.Apply(draft));
    draft = model.document(); draft.title = std::string("bad\0title", 9); CHECK(!model.Apply(draft));
    draft = model.document(); draft.title = std::string(1, static_cast<char>(0xff)); CHECK(!model.Apply(draft));
    draft = model.document(); draft.nodes[0].kind = static_cast<DesignerKind>(99); CHECK(!model.Apply(draft));
    draft = model.document(); draft.nodes[0].key = 0; CHECK(!model.Apply(draft));
    draft = model.document();
    for (uint32_t i = 2; i <= 257; ++i) { DesignerNode n; n.key = i; draft.nodes.push_back(n); }
    CHECK(!model.Apply(draft));
    DesignerModel edit;
    CHECK(edit.Insert(1, DesignerKind::Button));
    CHECK(edit.document().nodes[0].children[0].label == "Button");
    const auto button = edit.document().nodes[0].children[0].key;
    CHECK(edit.Insert(1, DesignerKind::Toggle));
    CHECK(edit.Move(button, 1));
    CHECK(edit.document().nodes[0].children[1].key == button);
    CHECK(!edit.Move(button, 1));
    CHECK(!edit.Insert(button, DesignerKind::Label));
    CHECK(edit.Erase(button)); CHECK(edit.Undo());
    CHECK(edit.document().nodes[0].children[1].key == button);
    CHECK(edit.Redo()); CHECK(!edit.Erase(1));
    CHECK(edit.Undo()); CHECK(edit.Insert(0, DesignerKind::Label)); CHECK(!edit.Redo());
    CHECK(edit.Erase(1)); CHECK(edit.document().nodes.size() == 1);
    CHECK(!edit.Erase(999));
    auto generated = edit.GenerateCpp();
    CHECK(generated.find("SCO_PLUGIN(DesignedPlugin, \"instrument\"") != std::string::npos);
    CHECK(generated.find("BuildDesignedPanel") != std::string::npos);
    CHECK(generated.find("RemovePanel(\"instrument.main\")") != std::string::npos);
    draft = edit.document(); draft.title = "A\"\\\n";
    draft.nodes[0].label = "hello\"\\\n";
    CHECK(edit.Apply(draft));
    generated = edit.GenerateCpp();
    CHECK(generated.find("hello\\\"\\\\\\012") != std::string::npos);
    int exports = 0;
    CHECK(edit.Export([&](const std::string& id, const std::string& language, const std::string& source) {
        ++exports; CHECK(id == "instrument"); CHECK(language == "cpp"); CHECK(source == generated);
        return std::string("exports/instrument");
    }));
    CHECK(exports == 1); CHECK(edit.status().find("exports/instrument") != std::string::npos);
    CHECK(!edit.Export({}));
    CHECK(!edit.Export([](const auto&, const auto&, const auto&) -> std::string { throw std::runtime_error("exists"); }));
    CHECK(edit.status().find("exists") != std::string::npos);
#ifdef SCUBA_DESIGNER_IMGUI
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1000, 900); io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    DesignerDock dock;
    CHECK(dock.model.Insert(1, DesignerKind::Button));
    CHECK(dock.model.Insert(1, DesignerKind::Toggle));
    CHECK(dock.model.Insert(1, DesignerKind::Slider));
    CHECK(dock.model.Insert(1, DesignerKind::TextInput));
    CHECK(dock.model.Insert(1, DesignerKind::Row));
    CHECK(dock.model.Insert(1, DesignerKind::Label));
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame(); dock.Draw(); ImGui::Render();
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
    ImGui::DestroyContext();
#endif
    DesignerModel depthModel;
    DesignerDocument deep;
    auto* cursor = &deep.nodes[0];
    for (uint32_t i = 2; i <= 16; ++i) {
        DesignerNode child; child.key = i; cursor->children.push_back(child); cursor = &cursor->children[0];
    }
    CHECK(depthModel.Apply(deep));
    DesignerNode tooDeep; tooDeep.key = 17; cursor->children.push_back(tooDeep);
    CHECK(!depthModel.Apply(deep));
#ifdef SCUBA_DESIGNER_EXPORT_TEST
    const scuba::hostd::DesignerExportOptions options{TEST_DESIGNER_SDK_ROOT, TEST_DESIGNER_OUTPUT, TEST_DESIGNER_PYTHON};
    DesignerModel exported;
    for (const auto kind : {DesignerKind::Row, DesignerKind::Label, DesignerKind::Button, DesignerKind::Toggle,
                            DesignerKind::Slider, DesignerKind::TextInput}) CHECK(exported.Insert(1, kind));
    CHECK(exported.Export([&](const auto& id, const auto& language, const auto& source) {
        auto params = scuba::hostd::Json::MakeObject();
        params.set("id", id); params.set("language", language); params.set("source", source);
        const auto result = scuba::hostd::ExportDesignerPlugin(params, options);
        const auto folder = options.outputRoot / result.find("folder")->as_string();
        std::ifstream file(folder / (id + ".cpp"), std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (actual != source || !std::filesystem::is_regular_file(folder / "plugin.ini"))
            throw std::runtime_error("Export readback mismatch");
        return folder.string();
    }));
#endif
    return CheckExit("test_native_designer");
}
