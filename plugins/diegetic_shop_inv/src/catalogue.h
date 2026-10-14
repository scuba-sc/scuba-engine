// SPDX-License-Identifier: GPL-3.0-only
// diegetic_shop_inv: the synthetic sample catalogue and its paging helpers.
// Every item here is invented sample data for development; none of it is real store data.
#ifndef DIEGETIC_SHOP_INV_CATALOGUE_H
#define DIEGETIC_SHOP_INV_CATALOGUE_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shop {

enum class Category : uint32_t { Armor = 0, Weapons = 1, Components = 2, Prefabs = 3 };
inline constexpr uint32_t kCategoryCount = 4;

struct Item {
    const char* id;          // command argument and stable key: [a-z0-9_]
    const char* name;        // display name
    Category category;
    int64_t price;           // local credits, > 0
    uint32_t slot;           // sc_loadout_slot, 0 when the item is not wearable
    const char* itemClass;   // game.loadout item_class form ([A-Za-z0-9_]), nullptr for prefabs
    const char* prefabPath;  // game.prefabs content path, nullptr for everything else
    const char* record;      // record-shaped sample name shown as the item's DataCore record
};

std::span<const Item> Catalogue() noexcept;
const Item* FindItem(std::string_view id) noexcept;
const char* CategoryName(Category c) noexcept;
// "all", "armor", "weapons", "components", "prefabs" -> -1 (all) or the category index.
bool ParseCategory(std::string_view name, int& out) noexcept;
const char* SlotName(uint32_t slot) noexcept;

// The items of one category (filter = category index) or all of them (filter < 0), in catalogue order.
std::vector<const Item*> Filter(int filter);

struct PageInfo {
    uint32_t page;   // 1-based, clamped into 1..pages
    uint32_t pages;  // at least 1
    uint32_t first;  // index of the first row on the page
    uint32_t count;  // rows on the page
};
PageInfo Paginate(std::size_t total, int64_t page, uint32_t pageSize) noexcept;

// "12,500 cr".
std::string FormatCredits(int64_t amount);

// The documented input rules of game.loadout (item_class) and game.prefabs (content path).
bool ValidItemClass(std::string_view s) noexcept;
bool ValidPrefabPath(std::string_view s) noexcept;

}  // namespace shop

#endif
