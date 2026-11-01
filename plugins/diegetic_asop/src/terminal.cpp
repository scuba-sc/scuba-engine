// SPDX-License-Identifier: GPL-3.0-only
#include "terminal.h"

#include <charconv>
#include <cstddef>
#include <utility>

namespace diegetic_asop {

namespace {

// Labels carry text from the game (last_error): keep printable ASCII and stay inside the 255-byte
// widget limit, so an odd byte can never make the whole panel fail validation.
std::string LabelText(std::string_view in) {
    std::string out;
    out.reserve(in.size() < SCO_WIDGETS_MAX_TEXT ? in.size() : SCO_WIDGETS_MAX_TEXT);
    for (const char c : in) {
        if (out.size() == SCO_WIDGETS_MAX_TEXT) break;
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u >= 0x20 && u < 0x7f ? c : '?');
    }
    return out;
}

Outcome BadId(const char* what, std::string_view text) {
    return Outcome{SCO_BAD_ARG, std::string(what) + " '" + LabelText(text) + "' is not a non-zero id (decimal or 0x hex)"};
}

}  // namespace

bool ParseEntityId(std::string_view text, uint64_t& out) noexcept {
    out = 0;
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return false;
    uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (ec != std::errc() || ptr != end || value == 0) return false;
    out = value;
    return true;
}

void Terminal::Attach(const sco_api* api, sco_plugin* self, sco::sdk::Widgets* widgets) noexcept {
    api_ = api;
    client_ = HangarClient(api, self);
    asopClient_ = AsopClient(api, self);
    widgets_ = widgets;
}

std::string Terminal::Summary() const {
    const HangarStatus s = client_.Status();
    std::string text = "game.hangar: ";
    text += !s.services ? "no services (sco_api 1.0)" : s.published ? "published" : "not published";
    text += "; lift: ";
    text += s.lift ? "ready" : "off";
    text += "; atc_cancel: ";
    text += s.atcCancel ? "ready" : "off";
    text += "; asop: ";
    if (!asop_.active) return text + "inert";
    text += "active (claim_timeout: ";
    text += asop_.claimTimeout ? "ready" : "off";
    text += ", shard_persisted: ";
    text += asop_.shardPersisted ? "ready" : "off";
    text += ")";
    return text;
}

scosdk::PanelTree Terminal::Build() {
    const HangarStatus s = client_.Status();
    const bool anyReady = s.lift || s.atcCancel;
    scosdk::PanelBuilder b;
    b.Column([&] {
        b.Label("Hangar lift and ATC");
        b.Label(LabelText(Summary())).Severity(anyReady ? SCO_WIDGET_SEVERITY_INFO : SCO_WIDGET_SEVERITY_WARNING);
        b.Row([&] {
            b.TextInput("Landing area id", &landing_);
            b.Button("Find lift", [this] { (void)FindLift(landing_); })
                .Tooltip("game.hangar.lift_of_landing_area; fills the lift id");
        });
        b.Row([&] {
            b.TextInput("Lift id", &lift_);
            b.Button("Open lift", [this] { (void)Lift(lift_, SC_HANGAR_LIFT_OPEN); });
            b.Button("Close lift", [this] { (void)Lift(lift_, SC_HANGAR_LIFT_CLOSE); });
        });
        b.Row([&] {
            b.TextInput("ATC id", &atc_);
            b.Button("Cancel ATC request", [this] { (void)CancelAtc(atc_); });
        });
        b.Button("Show last error", [this] { (void)LastError(); });
        b.Label(last_.message.empty() ? std::string("Ready") : LabelText(last_.message))
            .Severity(last_.ok() ? SCO_WIDGET_SEVERITY_INFO : SCO_WIDGET_SEVERITY_ERROR);
        if (asop_.active) {
            const bool anyAsop = asop_.claimTimeout || asop_.shardPersisted;
            b.Label("ASOP fleet");
            b.Label(LabelText("Claim timeout: " + claimText_ + "; shard persisted: " + shardText_))
                .Severity(anyAsop ? SCO_WIDGET_SEVERITY_INFO : SCO_WIDGET_SEVERITY_WARNING);
            b.Button("Read ASOP", [this] { (void)ReadAsop(); })
                .Tooltip("game.asop.claim_timeout and game.asop.shard_persisted (read only)");
        }
    });
    return b.Build();
}

void Terminal::SyncAsopStatus() {
    const AsopStatus nextAsop = asopClient_.Status();
    // Values are observations, not defaults. Once a gate closes, a later
    // reopening must not make an old observation look current again.
    if (asop_.claimTimeout && !nextAsop.claimTimeout) claimText_ = "not read";
    if (asop_.shardPersisted && !nextAsop.shardPersisted) shardText_ = "not read";
    if (lastWasAsopRead_ && last_.ok() &&
        ((asop_.claimTimeout && !nextAsop.claimTimeout) ||
         (asop_.shardPersisted && !nextAsop.shardPersisted))) {
        last_ = Outcome{SCO_UNAVAILABLE, "ASOP availability changed; read again"};
    }
    asop_ = nextAsop;
}

sco_result Terminal::Refresh() noexcept {
    try {
        SyncAsopStatus();
        scosdk::PanelTree next = Build();
        if (!next) {
            buildError_ = next.Error();
            return next.Result();
        }
        buildError_.clear();
        if (widgets_ && *widgets_) {
            const sco_result r = next.Submit(*widgets_, kPanelId, kPanelTitle);
            if (r != SCO_OK) return r;
        }
        tree_ = std::move(next);
        return SCO_OK;
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

Outcome Terminal::Record(Outcome outcome, bool asopRead) {
    last_ = outcome;
    lastWasAsopRead_ = asopRead;
    (void)Refresh();
    return outcome;
}

Outcome Terminal::FindLift(std::string_view landingArea) {
    uint64_t id = 0;
    if (!ParseEntityId(landingArea, id)) return Record(BadId("landing area", landingArea));
    uint64_t lift = 0;
    Outcome o = client_.FindLift(id, lift);
    if (o.ok()) lift_ = std::to_string(lift);
    return Record(std::move(o));
}

Outcome Terminal::Lift(std::string_view lift, sc_hangar_lift_op op) {
    uint64_t id = 0;
    if (!ParseEntityId(lift, id)) return Record(BadId("lift", lift));
    return Record(client_.RequestLift(id, op));
}

Outcome Terminal::CancelAtc(std::string_view atc) {
    uint64_t id = 0;
    if (!ParseEntityId(atc, id)) return Record(BadId("ATC", atc));
    return Record(client_.CancelAtc(id));
}

Outcome Terminal::LastError() { return Record(client_.LastError()); }

Outcome Terminal::ReadAsop() {
    // Invalidate older observations before recording the results of this read.
    SyncAsopStatus();
    int32_t seconds = 0;
    bool persisted = false;
    const Outcome claim = asopClient_.ClaimTimeout(seconds);
    const Outcome shard = asopClient_.ShardPersisted(persisted);
    claimText_ = claim.ok() ? std::to_string(seconds) + " s" : ResultName(claim.result);
    shardText_ = shard.ok() ? (persisted ? "yes" : "no") : ResultName(shard.result);
    // Both reads succeed or the first failure is the outcome; its message carries the reason.
    if (!claim.ok()) return Record(claim, true);
    if (!shard.ok()) return Record(shard, true);
    return Record(Outcome{SCO_OK, claim.message + "; " + shard.message}, true);
}

}  // namespace diegetic_asop
