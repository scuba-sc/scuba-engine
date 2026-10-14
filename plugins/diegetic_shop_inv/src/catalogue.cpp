// SPDX-License-Identifier: GPL-3.0-only
// The synthetic sample catalogue. Names, prices, classes, records and container paths are made
// up for development and tests: none of them name or describe a real game item.
#include "catalogue.h"

#include "sc_loadout.h"
#include "sc_prefabs.h"

#include <array>

namespace shop {
namespace {

constexpr Category A = Category::Armor;
constexpr Category W = Category::Weapons;
constexpr Category C = Category::Components;
constexpr Category P = Category::Prefabs;

constexpr uint32_t kHead = SC_LOADOUT_SLOT_HEAD, kTorso = SC_LOADOUT_SLOT_TORSO, kArms = SC_LOADOUT_SLOT_ARMS,
                   kLegs = SC_LOADOUT_SLOT_LEGS, kUnder = SC_LOADOUT_SLOT_UNDERSUIT,
                   kPack = SC_LOADOUT_SLOT_BACKPACK, kVisor = SC_LOADOUT_SLOT_VISOR;

constexpr std::array<Item, 32> kItems{{
    {"armor_helmet_tern", "Tern Helmet", A, 1450, kHead, "SCUBA_SAMPLE_Helmet_Tern", nullptr, "ScubaSample.Helmet_Tern"},
    {"armor_helmet_skua", "Skua Helmet", A, 2100, kHead, "SCUBA_SAMPLE_Helmet_Skua", nullptr, "ScubaSample.Helmet_Skua"},
    {"armor_core_tern", "Tern Core", A, 2600, kTorso, "SCUBA_SAMPLE_Core_Tern", nullptr, "ScubaSample.Core_Tern"},
    {"armor_core_skua", "Skua Core", A, 3900, kTorso, "SCUBA_SAMPLE_Core_Skua", nullptr, "ScubaSample.Core_Skua"},
    {"armor_arms_tern", "Tern Arms", A, 980, kArms, "SCUBA_SAMPLE_Arms_Tern", nullptr, "ScubaSample.Arms_Tern"},
    {"armor_arms_skua", "Skua Arms", A, 1500, kArms, "SCUBA_SAMPLE_Arms_Skua", nullptr, "ScubaSample.Arms_Skua"},
    {"armor_legs_tern", "Tern Legs", A, 1100, kLegs, "SCUBA_SAMPLE_Legs_Tern", nullptr, "ScubaSample.Legs_Tern"},
    {"armor_legs_skua", "Skua Legs", A, 1700, kLegs, "SCUBA_SAMPLE_Legs_Skua", nullptr, "ScubaSample.Legs_Skua"},
    {"undersuit_reef", "Reef Undersuit", A, 800, kUnder, "SCUBA_SAMPLE_Undersuit_Reef", nullptr, "ScubaSample.Undersuit_Reef"},
    {"undersuit_kelp", "Kelp Undersuit", A, 1200, kUnder, "SCUBA_SAMPLE_Undersuit_Kelp", nullptr, "ScubaSample.Undersuit_Kelp"},
    {"backpack_satchel", "Satchel Pack", A, 650, kPack, "SCUBA_SAMPLE_Backpack_Satchel", nullptr, "ScubaSample.Backpack_Satchel"},
    {"backpack_hauler", "Hauler Pack", A, 1400, kPack, "SCUBA_SAMPLE_Backpack_Hauler", nullptr, "ScubaSample.Backpack_Hauler"},
    {"visor_amber", "Amber Visor", A, 300, kVisor, "SCUBA_SAMPLE_Visor_Amber", nullptr, "ScubaSample.Visor_Amber"},
    {"visor_clear", "Clear Visor", A, 250, kVisor, "SCUBA_SAMPLE_Visor_Clear", nullptr, "ScubaSample.Visor_Clear"},
    {"weapon_pistol_minnow", "Minnow Pistol", W, 900, 0, "SCUBA_SAMPLE_Pistol_Minnow", nullptr, "ScubaSample.Pistol_Minnow"},
    {"weapon_rifle_marlin", "Marlin Rifle", W, 3400, 0, "SCUBA_SAMPLE_Rifle_Marlin", nullptr, "ScubaSample.Rifle_Marlin"},
    {"weapon_smg_barracuda", "Barracuda SMG", W, 2700, 0, "SCUBA_SAMPLE_Smg_Barracuda", nullptr, "ScubaSample.Smg_Barracuda"},
    {"weapon_tool_multitool", "Sample Multi-Tool", W, 600, 0, "SCUBA_SAMPLE_Tool_Multi", nullptr, "ScubaSample.Tool_Multi"},
    {"weapon_knife_coral", "Coral Knife", W, 200, 0, "SCUBA_SAMPLE_Knife_Coral", nullptr, "ScubaSample.Knife_Coral"},
    {"weapon_launcher_grouper", "Grouper Launcher", W, 7800, 0, "SCUBA_SAMPLE_Launcher_Grouper", nullptr, "ScubaSample.Launcher_Grouper"},
    {"comp_cooler_tide", "Tide Cooler", C, 5200, 0, "SCUBA_SAMPLE_Cooler_Tide", nullptr, "ScubaSample.Cooler_Tide"},
    {"comp_shield_lagoon", "Lagoon Shield", C, 8400, 0, "SCUBA_SAMPLE_Shield_Lagoon", nullptr, "ScubaSample.Shield_Lagoon"},
    {"comp_power_current", "Current Power Plant", C, 7300, 0, "SCUBA_SAMPLE_Power_Current", nullptr, "ScubaSample.Power_Current"},
    {"comp_qdrive_drift", "Drift Quantum Drive", C, 12500, 0, "SCUBA_SAMPLE_QDrive_Drift", nullptr, "ScubaSample.QDrive_Drift"},
    {"comp_scanner_sonar", "Sonar Scanner", C, 3100, 0, "SCUBA_SAMPLE_Scanner_Sonar", nullptr, "ScubaSample.Scanner_Sonar"},
    {"comp_fuel_pod", "Fuel Pod", C, 1800, 0, "SCUBA_SAMPLE_Fuel_Pod", nullptr, "ScubaSample.Fuel_Pod"},
    {"prefab_crate_stack", "Crate Stack", P, 400, 0, nullptr, "ObjectContainers/scuba_samples/crate_stack.socpak", "ScubaSample.Prefab_CrateStack"},
    {"prefab_kiosk", "Shop Kiosk", P, 2500, 0, nullptr, "ObjectContainers/scuba_samples/shop_kiosk.socpak", "ScubaSample.Prefab_Kiosk"},
    {"prefab_bench", "Bench", P, 300, 0, nullptr, "ObjectContainers/scuba_samples/bench.socpak", "ScubaSample.Prefab_Bench"},
    {"prefab_light_mast", "Light Mast", P, 750, 0, nullptr, "ObjectContainers/scuba_samples/light_mast.socpak", "ScubaSample.Prefab_LightMast"},
    {"prefab_cargo_rack", "Cargo Rack", P, 1200, 0, nullptr, "ObjectContainers/scuba_samples/cargo_rack.socpak", "ScubaSample.Prefab_CargoRack"},
    {"prefab_tent", "Field Tent", P, 900, 0, nullptr, "ObjectContainers/scuba_samples/field_tent.socpak", "ScubaSample.Prefab_FieldTent"},
}};

constexpr const char* kCategoryNames[kCategoryCount] = {"Armor", "Weapons", "Components", "Prefabs"};
constexpr const char* kCategoryKeys[kCategoryCount] = {"armor", "weapons", "components", "prefabs"};

bool AlnumOrUnderscore(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

std::span<const Item> Catalogue() noexcept { return kItems; }

const Item* FindItem(std::string_view id) noexcept {
    for (const Item& item : kItems)
        if (id == item.id) return &item;
    return nullptr;
}

const char* CategoryName(Category c) noexcept {
    const auto i = static_cast<uint32_t>(c);
    return i < kCategoryCount ? kCategoryNames[i] : "?";
}

bool ParseCategory(std::string_view name, int& out) noexcept {
    if (name == "all") {
        out = -1;
        return true;
    }
    for (uint32_t i = 0; i < kCategoryCount; ++i)
        if (name == kCategoryKeys[i]) {
            out = static_cast<int>(i);
            return true;
        }
    return false;
}

const char* SlotName(uint32_t slot) noexcept {
    switch (slot) {
        case SC_LOADOUT_SLOT_HEAD: return "head";
        case SC_LOADOUT_SLOT_TORSO: return "torso";
        case SC_LOADOUT_SLOT_ARMS: return "arms";
        case SC_LOADOUT_SLOT_LEGS: return "legs";
        case SC_LOADOUT_SLOT_UNDERSUIT: return "undersuit";
        case SC_LOADOUT_SLOT_BACKPACK: return "backpack";
        case SC_LOADOUT_SLOT_VISOR: return "visor";
        default: return "-";
    }
}

std::vector<const Item*> Filter(int filter) {
    std::vector<const Item*> out;
    for (const Item& item : kItems)
        if (filter < 0 || static_cast<uint32_t>(filter) == static_cast<uint32_t>(item.category)) out.push_back(&item);
    return out;
}

PageInfo Paginate(std::size_t total, int64_t page, uint32_t pageSize) noexcept {
    PageInfo p{};
    if (pageSize == 0) pageSize = 1;
    const std::size_t pages = total == 0 ? 1 : (total + pageSize - 1) / pageSize;
    p.pages = static_cast<uint32_t>(pages);
    if (page < 1) page = 1;
    if (static_cast<uint64_t>(page) > pages) page = static_cast<int64_t>(pages);
    p.page = static_cast<uint32_t>(page);
    const std::size_t first = static_cast<std::size_t>(p.page - 1) * pageSize;
    p.first = static_cast<uint32_t>(first);
    p.count = static_cast<uint32_t>(first >= total ? 0 : (total - first < pageSize ? total - first : pageSize));
    return p;
}

std::string FormatCredits(int64_t amount) {
    const bool negative = amount < 0;
    uint64_t v = negative ? 0 - static_cast<uint64_t>(amount) : static_cast<uint64_t>(amount);
    std::string digits;
    int n = 0;
    do {
        if (n && n % 3 == 0) digits.insert(digits.begin(), ',');
        digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
        ++n;
    } while (v);
    if (negative) digits.insert(digits.begin(), '-');
    return digits + " cr";
}

bool ValidItemClass(std::string_view s) noexcept {
    if (s.empty() || s.size() >= SC_LOADOUT_ITEM_NAME_BYTES) return false;
    for (char c : s)
        if (!AlnumOrUnderscore(c)) return false;
    return true;
}

bool ValidPrefabPath(std::string_view s) noexcept {
    constexpr std::string_view kRoot = "ObjectContainers/";
    constexpr std::string_view kExt = ".socpak";
    if (s.size() > SC_PREFABS_MAX_PATH || s.size() <= kRoot.size() + kExt.size()) return false;
    if (s.substr(0, kRoot.size()) != kRoot || s.substr(s.size() - kExt.size()) != kExt) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/') {
            const std::string_view part = s.substr(start, i - start);
            if (part.empty() || part == "." || part == "..") return false;
            start = i + 1;
            continue;
        }
        const char c = s[i];
        if (!AlnumOrUnderscore(c) && c != '-' && c != '.' && c != '{' && c != '}') return false;
    }
    return true;
}

}  // namespace shop
