// SPDX-License-Identifier: GPL-3.0-only
// diegetic_shop_inv: shop, loadout and prefab browser over a synthetic sample catalogue.
// Commands and the sco.widgets panel share the actions in shop.cpp; every game call is gated in
// services.cpp, so on a host without the services the plugin stays a read-only browser.
#include "catalogue.h"
#include "panel.h"
#include "services.h"
#include "shop.h"

#include "scosdk/scosdk.hpp"

#include <chrono>
#include <string>

namespace {

using sco::sdk::Args;
using sco::sdk::CommandBuilder;
using sco::sdk::Reply;

uint32_t SeverityOf(sco_result r) noexcept {
    if (r == SCO_OK) return SCO_WIDGET_SEVERITY_INFO;
    return r == SCO_UNAVAILABLE ? SCO_WIDGET_SEVERITY_WARNING : SCO_WIDGET_SEVERITY_ERROR;
}

class ShopPlugin : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        services_.Bind(Api(), Self());
        shop_.SeedKeys(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u);
        if (const sco_result r = RegisterCommands(); r != SCO_OK) return r;

        actions_.filter = [this] { OnFilter(); };
        actions_.select = [this] { OnSelect(); };
        actions_.refresh = [this] { Refresh(); Show(); };
        actions_.buy = [this] { Report(WithSelection([this](const shop::Item& i) { return shop_.Buy(i); })); };
        actions_.equip = [this] { Report(WithSelection([this](const shop::Item& i) { return shop_.Equip(i); })); };
        actions_.spawn = [this] { Report(WithSelection([this](const shop::Item& i) { return SpawnFromPanel(i); })); };

        if (widgets_.Open(*this) == SCO_OK) {
            Refresh();
            Show();
        } else {
            Info("sco.widgets is not available: commands only");
        }
        ready_ = Subscribe("game.ready", [this](const void*) {
            Refresh();
            Show();
        });
        if (!ready_) return ready_.Result();
        Status("%u sample items; %s", static_cast<unsigned>(shop::Catalogue().size()), services_.Describe().c_str());
        return SCO_OK;
    }

    void OnUnload() override {
        if (shown_) (void)widgets_.RemovePanel(shop::kPanelId);
    }

private:
    sco_result RegisterCommands() {
        sco_result r = CommandBuilder(*this, "diegetic_shop_inv.status")
                           .Title("Shop status")
                           .Help("Which shop services this host publishes and enables")
                           .Handle([this](const Args&, Reply& reply) {
                               reply.Printf("%s", services_.Describe().c_str());
                               return SCO_OK;
                           })
                           .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.categories")
                .Title("Categories")
                .Help("Item counts per category of the sample catalogue")
                .Handle([](const Args&, Reply& reply) {
                    std::string s = "all " + std::to_string(shop::Catalogue().size());
                    for (uint32_t c = 0; c < shop::kCategoryCount; ++c)
                        s += ", " + std::string(shop::CategoryName(static_cast<shop::Category>(c))) + " " +
                             std::to_string(shop::Filter(static_cast<int>(c)).size());
                    reply.Printf("%s", s.c_str());
                    return SCO_OK;
                })
                .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.list")
                .Title("List items")
                .Help("One page (4 items) of the current category")
                .Arg<int64_t>("page", "1-based page number")
                .Handle([this](const Args& args, Reply& reply) {
                    reply.Printf("%s", shop_.ListPage(args.Int(0)).c_str());
                    return SCO_OK;
                })
                .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.filter")
                .Title("Filter")
                .Help("all, armor, weapons, components or prefabs")
                .Arg<const char*>("category", "Category name")
                .Handle([this](const Args& args, Reply& reply) {
                    int filter = -1;
                    if (!shop::ParseCategory(args.String(0), filter)) {
                        reply.Printf("unknown category '%s'", args.String(0));
                        return SCO_BAD_ARG;
                    }
                    shop_.SetFilterIndex(filter);
                    model_.category = filter + 1;
                    model_.selected = -1;
                    model_.firstRow = 0;
                    keepPaging_ = false;
                    Show();
                    reply.Printf("%s", shop_.ListPage(1).c_str());
                    return SCO_OK;
                })
                .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.item")
                .Title("Item details")
                .Help("Details of one sample item")
                .Arg<const char*>("id", "Item id")
                .Handle([this](const Args& args, Reply& reply) {
                    const shop::Item* item = shop::FindItem(args.String(0));
                    if (!item) {
                        reply.Printf("no item '%s'", args.String(0));
                        return SCO_NOT_FOUND;
                    }
                    reply.Printf("%s", shop_.Detail(*item).c_str());
                    return SCO_OK;
                })
                .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.balance")
                .Title("Balance")
                .Help("game.economy local balance (game.economy.read)")
                .Handle([this](const Args&, Reply& reply) { return Answer(shop_.Balance(), reply); })
                .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.buy")
                .Title("Buy")
                .Help("Pay an item's price from the local balance (game.economy.read + apply)")
                .Arg<const char*>("id", "Item id")
                .Handle([this](const Args& args, Reply& reply) {
                    return WithItem(args.String(0), reply, [this](const shop::Item& i) { return shop_.Buy(i); });
                })
                .Register();
        if (r != SCO_OK) return r;
        r = CommandBuilder(*this, "diegetic_shop_inv.equip")
                .Title("Equip")
                .Help("Equip a wearable item on the local player (game.loadout.apply)")
                .Arg<const char*>("id", "Item id")
                .Handle([this](const Args& args, Reply& reply) {
                    return WithItem(args.String(0), reply, [this](const shop::Item& i) { return shop_.Equip(i); });
                })
                .Register();
        if (r != SCO_OK) return r;
        return CommandBuilder(*this, "diegetic_shop_inv.spawn")
            .Title("Spawn prefab")
            .Help("Spawn a prefab item in a zone at local x y z metres (game.entities.prefabs)")
            .Arg<const char*>("id", "Item id")
            .Arg<const char*>("zone", "Zone id, decimal or 0x hex")
            .Arg<double>("x", "Zone-local x (m)")
            .Arg<double>("y", "Zone-local y (m)")
            .Arg<double>("z", "Zone-local z (m)")
            .Handle([this](const Args& args, Reply& reply) {
                uint64_t zone = 0;
                if (!shop::ParseId(args.String(1), zone)) {
                    reply.Printf("zone '%s' is not a non-zero id", args.String(1));
                    return SCO_BAD_ARG;
                }
                const double pos[3] = {args.Float(2), args.Float(3), args.Float(4)};
                return WithItem(args.String(0), reply,
                                [&](const shop::Item& i) { return shop_.Spawn(i, zone, pos); });
            })
            .Register();
    }

    static sco_result Answer(const shop::Outcome& o, Reply& reply) {
        reply.Printf("%s", o.text.c_str());
        return o.result;
    }

    template <class F>
    sco_result WithItem(const char* id, Reply& reply, F&& action) {
        const shop::Item* item = shop::FindItem(id);
        if (!item) {
            reply.Printf("no item '%s'", id);
            return SCO_NOT_FOUND;
        }
        const shop::Outcome o = action(*item);
        Report(o);
        return Answer(o, reply);
    }

    template <class F>
    shop::Outcome WithSelection(F&& action) {
        const std::vector<const shop::Item*> visible = shop_.Visible();
        if (model_.selected < 0 || static_cast<std::size_t>(model_.selected) >= visible.size())
            return {SCO_BAD_ARG, "select an item first"};
        return action(*visible[static_cast<std::size_t>(model_.selected)]);
    }

    shop::Outcome SpawnFromPanel(const shop::Item& item) {
        uint64_t zone = 0;
        if (!shop::ParseId(model_.zone, zone)) return {SCO_BAD_ARG, "type a non-zero zone id first"};
        const double pos[3] = {0.0, 0.0, 0.0};
        return shop_.Spawn(item, zone, pos);
    }

    void OnFilter() {
        shop_.SetFilterIndex(model_.category - 1);
        model_.selected = -1;
        model_.firstRow = 0;
        keepPaging_ = false;
        Show();
    }

    void OnSelect() {
        const std::vector<const shop::Item*> visible = shop_.Visible();
        if (model_.selected >= 0 && static_cast<std::size_t>(model_.selected) < visible.size()) {
            model_.result = std::string("Selected ") + visible[static_cast<std::size_t>(model_.selected)]->name;
            model_.severity = SCO_WIDGET_SEVERITY_NONE;
        }
        Show();
    }

    void Report(const shop::Outcome& o) {
        model_.result = o.text;
        model_.severity = SeverityOf(o.result);
        if (o.result == SCO_OK) Refresh();
        Show();
    }

    void Refresh() {
        model_.status = services_.Describe();
        model_.datacore = services_.DataCoreState();
        const shop::Outcome b = shop_.Balance();
        model_.balance = b.result == SCO_OK ? b.text : "Balance: unavailable";
    }

    void Show() {
        if (!widgets_) return;
        if (keepPaging_ && tree_) model_.firstRow = shop::FirstRowOf(tree_, listId_);
        keepPaging_ = true;
        uint32_t listId = 0;
        scosdk::PanelTree next = shop::BuildShopPanel(model_, shop_.Visible(), actions_, listId);
        if (!next) {
            Warn("panel build failed: %s", next.Error().c_str());
            return;
        }
        const sco_result r = next.Submit(widgets_, shop::kPanelId, shop::kPanelTitle);
        if (r != SCO_OK) {
            Warn("set_panel: %s", shop::ResultName(r));
            return;
        }
        tree_ = std::move(next);
        listId_ = listId;
        shown_ = true;
    }

    shop::Services services_;
    shop::Shop shop_{services_};
    sco::sdk::Widgets widgets_;
    scosdk::PanelTree tree_;
    shop::PanelModel model_;
    shop::PanelActions actions_;
    sco::sdk::Subscription ready_;
    uint32_t listId_ = 0;
    bool keepPaging_ = true;
    bool shown_ = false;
};

}  // namespace

SCO_PLUGIN(ShopPlugin, "diegetic_shop_inv", "0.1.0", "scuba-engine");
