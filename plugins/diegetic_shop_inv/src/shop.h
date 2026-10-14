// SPDX-License-Identifier: GPL-3.0-only
// diegetic_shop_inv: the shop actions shared by commands and panel buttons.
#ifndef DIEGETIC_SHOP_INV_SHOP_H
#define DIEGETIC_SHOP_INV_SHOP_H

#include "catalogue.h"
#include "services.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shop {

inline constexpr uint32_t kCommandPageSize = 4;  // keeps a page inside the hosts' 256-byte reply

struct Outcome {
    sco_result result;
    std::string text;
};

// Decimal or 0x hex, non-zero, full uint64 range.
bool ParseId(std::string_view text, uint64_t& out) noexcept;

class Shop {
public:
    explicit Shop(const Services& services) noexcept : services_(services) {}

    // Payment keys are seed, seed + 1, ...; zero is skipped (game.economy needs non-zero keys).
    void SeedKeys(uint64_t seed) noexcept { nextKey_ = seed; }
    uint64_t LastKey() const noexcept { return lastKey_; }

    int FilterIndex() const noexcept { return filter_; }
    void SetFilterIndex(int filter) noexcept { filter_ = filter < 0 || filter >= static_cast<int>(kCategoryCount) ? -1 : filter; }
    std::vector<const Item*> Visible() const { return Filter(filter_); }

    std::string ListPage(int64_t page) const;
    std::string Detail(const Item& item) const;

    Outcome Balance() const;
    Outcome Buy(const Item& item);
    Outcome Equip(const Item& item) const;
    Outcome Spawn(const Item& item, uint64_t zone, const double pos[3]) const;

private:
    const Services& services_;
    int filter_ = -1;
    uint64_t nextKey_ = 1;
    uint64_t lastKey_ = 0;
};

}  // namespace shop

#endif
