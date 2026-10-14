// SPDX-License-Identifier: GPL-3.0-only
#include "catalogue.h"
#include "panel.h"
#include "services.h"
#include "shop.h"
#include "sco_datacore.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

struct sco_plugin { int id; };

namespace {
void Check(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "check failed at line %d\n", line);
        std::exit(1);
    }
}
#define CHECK(condition) Check((condition), __LINE__)

struct Fake {
    std::set<std::string> caps;
    bool published = true;
    uint32_t queries = 0, reads = 0, payments = 0, equips = 0, spawns = 0;
    int64_t balance = 20000;
    sco_result paymentResult = SCO_OK, equipResult = SCO_OK, spawnResult = SCO_OK;
    sc_economy_request payment{};
    sc_loadout_item equipped{};
    std::string path;
    uint64_t zone = 0;
    std::array<double, 3> position{};
    std::array<double, 4> rotation{};
};
Fake fake;

int Has(const char* name) { return fake.caps.count(name) ? 1 : 0; }
sco_result Balance(sco_plugin*, sc_economy_balance* out) {
    ++fake.reads;
    CHECK(out->size == sizeof(*out));
    out->local_balance = fake.balance;
    return SCO_OK;
}
sco_result Pay(sco_plugin*, const sc_economy_request* request, sc_economy_receipt* out) {
    ++fake.payments;
    CHECK(request->size == sizeof(*request) && request->flags == 0);
    CHECK(out->size == sizeof(*out));
    fake.payment = *request;
    if (fake.paymentResult != SCO_OK) return fake.paymentResult;
    CHECK(request->expected_balance == fake.balance);
    out->key = request->key;
    out->before = fake.balance;
    fake.balance += request->delta;
    out->after = fake.balance;
    out->delta = request->delta;
    return SCO_OK;
}
sco_result Equip(sco_plugin*, const sc_loadout_request* request, sc_loadout_report* out) {
    ++fake.equips;
    CHECK(request->size == sizeof(*request));
    CHECK(request->count == 1 && request->item_stride == sizeof(sc_loadout_item));
    CHECK(out->size == sizeof(*out));
    fake.equipped = request->items[0];
    out->count = 1;
    out->applied = fake.equipResult == SCO_OK ? 1u : 0u;
    out->slots[0] = {fake.equipped.slot, fake.equipResult == SCO_OK ? SC_LOADOUT_APPLIED : SC_LOADOUT_ITEM_MISSING};
    return fake.equipResult;
}
sco_result Spawn(sco_plugin*, const char* path, uint64_t zone, const double pos[3],
                 const double rot[4], uint64_t* id) {
    ++fake.spawns;
    fake.path = path;
    fake.zone = zone;
    for (std::size_t i = 0; i < 3; ++i) fake.position[i] = pos[i];
    for (std::size_t i = 0; i < 4; ++i) fake.rotation[i] = rot[i];
    *id = fake.spawnResult == SCO_OK ? 9001u : 0u;
    return fake.spawnResult;
}
uint32_t State() { return SCO_DC_LOADED; }
sc_economy_v1 economy{sizeof(sc_economy_v1), 0, Balance, Pay, nullptr};
sc_loadout_v1 loadout{sizeof(sc_loadout_v1), 0, Equip};
sc_prefabs_v1 prefabs{sizeof(sc_prefabs_v1), 0, Spawn};
sco_datacore_v1 datacore{};
sco_result Query(const char* name, uint32_t version, const void** out) {
    ++fake.queries;
    CHECK(version == 0x00010000u);
    *out = nullptr;
    if (!fake.published) return SCO_NOT_FOUND;
    if (std::strcmp(name, SC_ECONOMY_SERVICE_NAME) == 0) *out = &economy;
    if (std::strcmp(name, SC_LOADOUT_SERVICE_NAME) == 0) *out = &loadout;
    if (std::strcmp(name, SC_PREFABS_NAME) == 0) *out = &prefabs;
    if (std::strcmp(name, SCO_DATACORE_NAME) == 0) *out = &datacore;
    return *out ? SCO_OK : SCO_NOT_FOUND;
}

void CatalogueTests() {
    const auto items = shop::Catalogue();
    CHECK(items.size() == 32);
    std::set<std::string> ids;
    for (const shop::Item& item : items) {
        CHECK(ids.insert(item.id).second);
        CHECK(shop::FindItem(item.id) == &item);
        CHECK(item.price > 0 && item.price < SC_ECONOMY_BALANCE_LIMIT);
        CHECK(std::string(item.record).starts_with("ScubaSample."));
        if (item.itemClass) CHECK(shop::ValidItemClass(item.itemClass));
        if (item.prefabPath) CHECK(shop::ValidPrefabPath(item.prefabPath));
    }
    CHECK(shop::FindItem("unknown") == nullptr);
    CHECK(!shop::ValidItemClass("bad class") && !shop::ValidItemClass(""));
    CHECK(!shop::ValidPrefabPath("ObjectContainers/../bad.socpak"));
    CHECK(!shop::ValidPrefabPath("ObjectContainers/a\\b.socpak"));
    CHECK(!shop::ValidPrefabPath("ObjectContainers//b.socpak"));
    CHECK(shop::FormatCredits(-12500) == "-12,500 cr");
    CHECK(shop::FormatCredits(INT64_MIN) == "-9,223,372,036,854,775,808 cr");
    int category = 99;
    CHECK(shop::ParseCategory("prefabs", category) && category == 3);
    CHECK(shop::Filter(category).size() == 6);
    CHECK(!shop::ParseCategory("unknown", category));
    auto page = shop::Paginate(32, 2, 4);
    CHECK(page.page == 2 && page.pages == 8 && page.first == 4 && page.count == 4);
    page = shop::Paginate(6, INT64_MAX, 4);
    CHECK(page.page == 2 && page.first == 4 && page.count == 2);
    page = shop::Paginate(0, -1, 4);
    CHECK(page.page == 1 && page.pages == 1 && page.count == 0);
    uint64_t id = 0;
    CHECK(shop::ParseId("18446744073709551615", id) && id == UINT64_MAX);
    CHECK(shop::ParseId("0xffffffffffffffff", id) && id == UINT64_MAX);
    CHECK(!shop::ParseId("18446744073709551616", id));
    CHECK(!shop::ParseId("0", id) && !shop::ParseId("-1", id) && !shop::ParseId("1 ", id));
}

void ServiceTests() {
    sco_api api{};
    api.size = sizeof(api);
    api.has = Has;
    api.query_service = Query;
    sco_plugin self{1};
    shop::Services services;
    services.Bind(&api, &self);
    shop::Shop browser(services);
    const shop::Item& helmet = *shop::FindItem("armor_helmet_tern");
    const shop::Item& prefab = *shop::FindItem("prefab_crate_stack");
    const double position[3] = {1.0, -2.0, 3.0};
    std::string why;
    int64_t balance = 0;
    sc_economy_receipt receipt{};
    sc_loadout_report report{};
    uint64_t entity = 0;

    api.size = static_cast<uint32_t>(offsetof(sco_api, provide_service));
    CHECK(services.Balance(balance, why) == SCO_UNAVAILABLE && fake.queries == 0);
    CHECK(browser.Equip(helmet).result == SCO_UNAVAILABLE && fake.queries == 0);
    CHECK(browser.Spawn(prefab, 42, position).result == SCO_UNAVAILABLE && fake.queries == 0);
    api.size = sizeof(api);
    fake.published = false;
    CHECK(services.Balance(balance, why) == SCO_UNAVAILABLE && fake.reads == 0);
    fake.published = true;
    CHECK(services.Balance(balance, why) == SCO_UNAVAILABLE && fake.reads == 0);
    CHECK(services.Equip(helmet, report, why) == SCO_UNAVAILABLE && fake.equips == 0);
    CHECK(services.Spawn(prefab, 42, position, entity, why) == SCO_UNAVAILABLE && fake.spawns == 0);

    fake.caps.insert(shop::kCapEconomyRead);
    CHECK(services.Balance(balance, why) == SCO_OK && balance == 20000);
    CHECK(services.Pay(1, 1450, receipt, why) == SCO_UNAVAILABLE && fake.payments == 0);
    fake.caps.insert(shop::kCapEconomyApply);
    fake.caps.insert(shop::kCapLoadoutApply);
    fake.caps.insert(shop::kCapPrefabsSpawn);
    economy.size = static_cast<uint32_t>(offsetof(sc_economy_v1, balance));
    CHECK(services.Balance(balance, why) == SCO_UNAVAILABLE);
    economy.size = static_cast<uint32_t>(offsetof(sc_economy_v1, apply));
    CHECK(services.Pay(1, 1450, receipt, why) == SCO_UNAVAILABLE && fake.payments == 0);
    economy.size = sizeof(economy);
    loadout.size = static_cast<uint32_t>(offsetof(sc_loadout_v1, apply));
    CHECK(services.Equip(helmet, report, why) == SCO_UNAVAILABLE && fake.equips == 0);
    loadout.size = sizeof(loadout);
    prefabs.size = static_cast<uint32_t>(offsetof(sc_prefabs_v1, spawn));
    CHECK(services.Spawn(prefab, 42, position, entity, why) == SCO_UNAVAILABLE && fake.spawns == 0);
    prefabs.size = sizeof(prefabs);
    const auto originalApply = economy.apply;
    economy.apply = nullptr;
    CHECK(services.Pay(1, 1450, receipt, why) == SCO_UNAVAILABLE && fake.payments == 0);
    economy.apply = originalApply;

    browser.SeedKeys(0);
    CHECK(browser.Buy(helmet).result == SCO_OK && fake.payments == 1);
    CHECK(fake.payment.key == 1 && fake.payment.delta == -1450 && fake.payment.expected_balance == 20000);
    CHECK(browser.LastKey() == 1 && fake.balance == 18550);
    CHECK(browser.Buy(helmet).result == SCO_OK && fake.payment.key == 2);
    fake.paymentResult = SCO_FAILED;
    CHECK(browser.Buy(helmet).result == SCO_FAILED && browser.LastKey() == 2);
    fake.paymentResult = SCO_OK;
    fake.balance = 1;
    const uint32_t payments = fake.payments;
    CHECK(browser.Buy(helmet).result == SCO_FAILED && fake.payments == payments);
    CHECK(services.Pay(0, 10, receipt, why) == SCO_BAD_ARG);
    CHECK(browser.Equip(helmet).result == SCO_OK && fake.equips == 1);
    CHECK(fake.equipped.slot == SC_LOADOUT_SLOT_HEAD);
    CHECK(std::strcmp(fake.equipped.item_class, helmet.itemClass) == 0);
    fake.equipResult = SCO_FAILED;
    CHECK(browser.Equip(helmet).result == SCO_FAILED);
    CHECK(browser.Equip(prefab).result == SCO_BAD_ARG);
    CHECK(browser.Spawn(prefab, 0, position).result == SCO_BAD_ARG);
    CHECK(browser.Spawn(helmet, 42, position).result == SCO_BAD_ARG);
    CHECK(browser.Spawn(prefab, 42, position).result == SCO_OK && fake.spawns == 1);
    CHECK(fake.path == prefab.prefabPath && fake.zone == 42 && fake.position[1] == -2.0);
    CHECK(fake.rotation[0] == 0.0 && fake.rotation[3] == 1.0);
    fake.spawnResult = SCO_NOT_FOUND;
    CHECK(browser.Spawn(prefab, 42, position).result == SCO_NOT_FOUND);
    datacore.size = sizeof(datacore);
    datacore.state = State;
    CHECK(services.DataCoreState() == "loaded");
    datacore.size = static_cast<uint32_t>(offsetof(sco_datacore_v1, state));
    CHECK(services.DataCoreState() == "no state()");
    for (int filter = -1; filter < 4; ++filter) {
        browser.SetFilterIndex(filter);
        for (int64_t page = 1; page <= 8; ++page) CHECK(browser.ListPage(page).size() <= 255);
    }
}

void PanelTests() {
    shop::PanelModel model;
    model.status = "synthetic provider";
    model.datacore = "loaded";
    uint32_t listId = 0;
    uint32_t selections = 0, buys = 0;
    shop::PanelActions actions;
    actions.select = [&] { ++selections; };
    actions.buy = [&] { ++buys; };
    const auto visible = shop::Filter(-1);
    auto tree = shop::BuildShopPanel(model, visible, actions, listId);
    CHECK(static_cast<bool>(tree));
    CHECK(scosdk::ValidateWidgetNodes(tree.Nodes(), tree.Count(), tree.Stride()) == SCO_OK);
    CHECK(tree.Find(listId)->item_count == 32);
    sco_widget_event_1_1 page{};
    page.base.size = sizeof(page);
    page.base.node_id = listId;
    page.base.kind = SCO_WIDGET_EVENT_PAGE;
    page.row = 8;
    CHECK(tree.Dispatch(page.base) && shop::FirstRowOf(tree, listId) == 8);
    page.row = 32;
    CHECK(!tree.Dispatch(page.base));
    sco_widget_event selection{};
    selection.size = sizeof(selection);
    selection.node_id = listId;
    selection.kind = SCO_WIDGET_LIST;
    selection.index = 9;
    CHECK(tree.Dispatch(selection) && model.selected == 9 && selections == 1);
    model.firstRow = shop::FirstRowOf(tree, listId);
    tree = shop::BuildShopPanel(model, visible, actions, listId);
    CHECK(static_cast<bool>(tree) && shop::FirstRowOf(tree, listId) == 8);
    CHECK(scosdk::ValidateWidgetNodes(tree.Nodes(), tree.Count(), tree.Stride()) == SCO_OK);
    for (uint32_t i = 1; i <= tree.Count(); ++i) {
        const auto* node = tree.Find(i);
        if (node->kind != SCO_WIDGET_BUTTON || std::strcmp(node->label, "Buy") != 0) continue;
        sco_widget_event click{};
        click.size = sizeof(click);
        click.node_id = node->id;
        click.kind = SCO_WIDGET_BUTTON;
        CHECK(tree.Dispatch(click));
    }
    CHECK(buys == 1);
    model.selected = -1;
    model.firstRow = 0;
    tree = shop::BuildShopPanel(model, {}, actions, listId);
    CHECK(static_cast<bool>(tree));
}
}

int main() {
    CatalogueTests();
    ServiceTests();
    PanelTests();
    std::puts("all checks passed");
}
