// SPDX-License-Identifier: GPL-3.0-only
// diegetic_asop: the terminal panel (scosdk::PanelBuilder over sco.widgets) and the actions its
// buttons and the plugin's commands share. Game thread only, like every sco.widgets call.
#ifndef DIEGETIC_ASOP_TERMINAL_H
#define DIEGETIC_ASOP_TERMINAL_H

#include "asop_probe.h"
#include "hangar_client.h"

#include "scosdk/menu_builder.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace diegetic_asop {

inline constexpr const char* kPanelId = "diegetic_asop.terminal";
inline constexpr const char* kPanelTitle = "ASOP Terminal";

// Decimal or 0x-prefixed hex, surrounding spaces allowed; 0 and anything else is rejected.
bool ParseEntityId(std::string_view text, uint64_t& out) noexcept;

class Terminal {
public:
    // widgets may be null (no sco.widgets on this host, or a unit test): the tree is still built.
    void Attach(const sco_api* api, sco_plugin* self, sco::sdk::Widgets* widgets) noexcept;

    // Re-probes game.asop, rebuilds the tree and submits it when widgets are attached. On a failed
    // submit the previous tree stays registered and alive.
    sco_result Refresh() noexcept;

    Outcome FindLift(std::string_view landingArea);
    Outcome Lift(std::string_view lift, sc_hangar_lift_op op);
    Outcome CancelAtc(std::string_view atc);
    Outcome LastError();

    std::string Summary() const;
    const Outcome& Last() const noexcept { return last_; }
    const AsopStatus& Asop() const noexcept { return asop_; }
    scosdk::PanelTree& Tree() noexcept { return tree_; }
    const std::string& BuildError() const noexcept { return buildError_; }

private:
    scosdk::PanelTree Build();
    Outcome Record(Outcome outcome);

    HangarClient client_;
    const sco_api* api_ = nullptr;
    sco::sdk::Widgets* widgets_ = nullptr;
    AsopStatus asop_;
    Outcome last_;
    std::string landing_, lift_, atc_;
    std::string buildError_;
    scosdk::PanelTree tree_;
};

}  // namespace diegetic_asop

#endif
