// SPDX-License-Identifier: GPL-3.0-only
#include "panel.h"

namespace shop {

scosdk::PanelTree BuildShopPanel(PanelModel& model, const std::vector<const Item*>& visible,
                                 const PanelActions& actions, uint32_t& listId) {
    std::vector<std::string> names;
    names.reserve(visible.size());
    for (const Item* item : visible) names.push_back(std::string(item->name) + " - " + FormatCredits(item->price));

    const uint32_t rows = static_cast<uint32_t>(names.size());
    if (model.selected >= static_cast<int>(rows)) model.selected = -1;
    if (model.firstRow >= rows) model.firstRow = 0;

    std::vector<std::string> cells;
    if (model.selected >= 0) {
        const Item& item = *visible[static_cast<std::size_t>(model.selected)];
        cells = {"Name", item.name,
                 "Category", CategoryName(item.category),
                 "Price", FormatCredits(item.price),
                 "Slot", SlotName(item.slot),
                 "Prefab", item.prefabPath ? item.prefabPath : "-",
                 "Record", std::string(item.record) + " (sample)",
                 "DataCore", model.datacore};
    }

    scosdk::PanelBuilder b;
    b.Column([&] {
        b.Label("Synthetic sample catalogue: not real store data")
            .Severity(SCO_WIDGET_SEVERITY_INFO)
            .Tooltip("Every item, price and record here is invented sample data shipped with the plugin.");
        b.Label(model.status);
        b.Row([&] {
            b.Combo("Category", &model.category, {"All", "Armor", "Weapons", "Components", "Prefabs"},
                    [&actions](int) { if (actions.filter) actions.filter(); });
            b.Button("Refresh", actions.refresh);
            b.Label(model.balance);
        });
        listId = b.List("Items", &model.selected, names, [&actions](int) { if (actions.select) actions.select(); })
                     .Paging(model.firstRow, kVisibleRows)
                     .Tooltip("Paged: 8 rows at a time")
                     .Id();
        if (cells.empty())
            b.Label("Nothing selected");
        else
            b.Table("Details", 2, cells);
        b.Row([&] {
            b.TextInput("Zone id", &model.zone);
            b.Button("Buy", actions.buy).Tooltip("game.economy: pay the price from the local balance");
            b.Button("Equip", actions.equip).Tooltip("game.loadout: equip a wearable item");
            b.Button("Spawn", actions.spawn).Tooltip("game.prefabs: spawn a prefab item in the zone");
        });
        b.Label(model.result).Severity(model.severity);
    });
    return b.Build();
}

uint32_t FirstRowOf(const scosdk::PanelTree& tree, uint32_t listId) noexcept {
    const sco_widget_node* node = tree.Find(listId);
    if (!node || node->kind != SCO_WIDGET_LIST || node->size < sizeof(sco_widget_node_1_1)) return 0;
    return reinterpret_cast<const sco_widget_node_1_1*>(node)->first_row;
}

}  // namespace shop
