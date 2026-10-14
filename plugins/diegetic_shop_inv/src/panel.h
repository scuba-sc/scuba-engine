// SPDX-License-Identifier: GPL-3.0-only
// diegetic_shop_inv: the sco.widgets panel, built with scosdk::PanelBuilder.
#ifndef DIEGETIC_SHOP_INV_PANEL_H
#define DIEGETIC_SHOP_INV_PANEL_H

#include "catalogue.h"

#include "scosdk/menu_builder.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace shop {

inline constexpr char kPanelId[] = "diegetic_shop_inv.shop";
inline constexpr char kPanelTitle[] = "Shop & Loadout";
inline constexpr uint32_t kVisibleRows = 8;

struct PanelModel {
    int category = 0;        // combo index: 0 = all, n = category n - 1
    int selected = -1;       // index into the visible items
    uint32_t firstRow = 0;   // first visible row of the paged item list
    std::string zone;        // zone id typed for Spawn
    std::string status;
    std::string balance;
    std::string datacore;
    std::string result = "Pick an item.";
    uint32_t severity = SCO_WIDGET_SEVERITY_NONE;
};

struct PanelActions {
    std::function<void()> filter, select, refresh, buy, equip, spawn;
};

// listId receives the item list's node id, for FirstRowOf on the next rebuild.
scosdk::PanelTree BuildShopPanel(PanelModel& model, const std::vector<const Item*>& visible,
                                 const PanelActions& actions, uint32_t& listId);

// The list's current first visible row (host paging events update it inside the tree).
uint32_t FirstRowOf(const scosdk::PanelTree& tree, uint32_t listId) noexcept;

}  // namespace shop

#endif
