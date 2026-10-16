// SPDX-License-Identifier: GPL-3.0-only
// diegetic_asop: ASOP and hangar lift terminal. A sco.widgets panel plus the same actions as
// commands, so a host without widgets (or a script) can still drive game.hangar.
#include "terminal.h"

#include "scosdk/scosdk.hpp"

namespace {

using diegetic_asop::Outcome;

class DiegeticAsop : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        terminal_.Attach(Api(), Self(), &widgets_);

        sco_result r = Command("diegetic_asop.status", "Terminal status", "game.hangar and game.asop availability")
                           .Handle([this](const sco::sdk::Args&, sco::sdk::Reply& reply) {
                               reply.Set(terminal_.Summary().c_str());
                               return SCO_OK;
                           })
                           .Register();
        if (r != SCO_OK) return r;

        r = IdCommand("diegetic_asop.find_lift", "Find lift", "The ship lift serving a landing area",
                      "landing_area", [this](const char* id) { return terminal_.FindLift(id); });
        if (r != SCO_OK) return r;
        r = IdCommand("diegetic_asop.lift_open", "Open lift", "Ask a ship lift to come up", "lift",
                      [this](const char* id) { return terminal_.Lift(id, SC_HANGAR_LIFT_OPEN); });
        if (r != SCO_OK) return r;
        r = IdCommand("diegetic_asop.lift_close", "Close lift", "Ask a ship lift to go down", "lift",
                      [this](const char* id) { return terminal_.Lift(id, SC_HANGAR_LIFT_CLOSE); });
        if (r != SCO_OK) return r;
        r = IdCommand("diegetic_asop.atc_cancel", "Cancel ATC request", "Withdraw your pending ATC request", "atc",
                      [this](const char* id) { return terminal_.CancelAtc(id); });
        if (r != SCO_OK) return r;

        r = Command("diegetic_asop.asop", "Read ASOP", "game.asop's claim timeout and shard persisted flag")
                .Handle([this](const sco::sdk::Args&, sco::sdk::Reply& reply) {
                    const Outcome o = terminal_.ReadAsop();
                    reply.Set(o.message.c_str());
                    return o.result;
                })
                .Register();
        if (r != SCO_OK) return r;

        r = Command("diegetic_asop.last_error", "Last hangar error", "game.hangar's reason for the last failure")
                .Handle([this](const sco::sdk::Args&, sco::sdk::Reply& reply) {
                    const Outcome o = terminal_.LastError();
                    reply.Set(o.message.c_str());
                    return o.result;
                })
                .Register();
        if (r != SCO_OK) return r;

        // The game pack publishes its services by game.ready; refresh the panel then too.
        ready_ = Subscribe("game.ready", [this](const void*) { ShowPanel(); });
        if (!ready_) return ready_.Result();

        if (widgets_.Open(*this) == SCO_OK)
            ShowPanel();
        else
            Info("sco.widgets is not available: commands only");
        Status("%s", terminal_.Summary().c_str());
        return SCO_OK;
    }

    void OnUnload() override {
        if (widgets_) (void)widgets_.RemovePanel(diegetic_asop::kPanelId);
    }

private:
    sco::sdk::CommandBuilder Command(const char* name, const char* title, const char* help) {
        sco::sdk::CommandBuilder b(*this, name);
        b.Title(title).Help(help);
        return b;
    }

    template <class F>
    sco_result IdCommand(const char* name, const char* title, const char* help, const char* arg, F action) {
        return Command(name, title, help)
            .Arg<const char*>(arg, "entity id, decimal or 0x hex")
            .Handle([action](const sco::sdk::Args& args, sco::sdk::Reply& reply) {
                const Outcome o = action(args.String(0));
                reply.Set(o.message.c_str());
                return o.result;
            })
            .Register();
    }

    void ShowPanel() {
        if (!widgets_) return;
        const sco_result r = terminal_.Refresh();
        if (r != SCO_OK)
            Warn("terminal panel not shown (%s) %s", diegetic_asop::ResultName(r), terminal_.BuildError().c_str());
    }

    sco::sdk::Widgets widgets_;
    diegetic_asop::Terminal terminal_;
    sco::sdk::Subscription ready_;
};

}  // namespace

SCO_PLUGIN(DiegeticAsop, "diegetic_asop", "0.1.0", "scuba-engine");
