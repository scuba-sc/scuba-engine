// SPDX-License-Identifier: GPL-3.0-only
#include "shop.h"

#include <cinttypes>
#include <cstdio>

namespace shop {

bool ParseId(std::string_view text, uint64_t& out) noexcept {
    out = 0;
    unsigned base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 20) return false;
    uint64_t v = 0;
    for (char c : text) {
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        if (v > (UINT64_MAX - d) / base) return false;
        v = v * base + d;
    }
    if (v == 0) return false;
    out = v;
    return true;
}

std::string Shop::ListPage(int64_t page) const {
    const std::vector<const Item*> rows = Visible();
    const PageInfo p = Paginate(rows.size(), page, kCommandPageSize);
    std::string out = "p" + std::to_string(p.page) + "/" + std::to_string(p.pages) + " " +
                      (filter_ < 0 ? "all" : CategoryName(static_cast<Category>(filter_))) + ":";
    if (p.count == 0) return out + " (empty)";
    for (uint32_t i = 0; i < p.count; ++i) {
        const Item& item = *rows[p.first + i];
        out += std::string(i ? "; " : " ") + item.id + " " + FormatCredits(item.price);
    }
    return out;
}

std::string Shop::Detail(const Item& item) const {
    std::string out = std::string(item.name) + " [" + item.id + "] " + CategoryName(item.category) + ", " +
                      FormatCredits(item.price);
    if (item.slot) out += ", slot " + std::string(SlotName(item.slot));
    if (item.prefabPath) out += ", prefab " + std::string(item.prefabPath);
    out += ", record " + std::string(item.record) + " (sample); datacore: " + services_.DataCoreState();
    return out;
}

Outcome Shop::Balance() const {
    int64_t balance = 0;
    std::string why;
    const sco_result r = services_.Balance(balance, why);
    if (r != SCO_OK) return {r, why};
    return {SCO_OK, "local balance " + FormatCredits(balance)};
}

Outcome Shop::Buy(const Item& item) {
    if (nextKey_ == 0) ++nextKey_;
    const uint64_t key = nextKey_++;
    sc_economy_receipt receipt{};
    std::string why;
    const sco_result r = services_.Pay(key, item.price, receipt, why);
    if (r != SCO_OK) return {r, "buy " + std::string(item.id) + ": " + why};
    lastKey_ = key;
    char keyText[32];
    std::snprintf(keyText, sizeof keyText, "0x%" PRIx64, key);
    return {SCO_OK, "bought " + std::string(item.name) + " for " + FormatCredits(item.price) + ": " +
                        FormatCredits(receipt.before) + " -> " + FormatCredits(receipt.after) + " (key " + keyText + ")"};
}

Outcome Shop::Equip(const Item& item) const {
    sc_loadout_report report{};
    std::string why;
    const sco_result r = services_.Equip(item, report, why);
    return {r, "equip " + std::string(item.id) + ": " + why};
}

Outcome Shop::Spawn(const Item& item, uint64_t zone, const double pos[3]) const {
    uint64_t entity = 0;
    std::string why;
    const sco_result r = services_.Spawn(item, zone, pos, entity, why);
    if (r != SCO_OK) return {r, "spawn " + std::string(item.id) + ": " + why};
    char idText[32];
    std::snprintf(idText, sizeof idText, "0x%" PRIx64, entity);
    return {SCO_OK, "spawn " + std::string(item.id) + ": started, entity " + idText};
}

}  // namespace shop
