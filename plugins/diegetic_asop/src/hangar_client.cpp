// SPDX-License-Identifier: GPL-3.0-only
#include "hangar_client.h"

#include <cstddef>
#include <cstring>

namespace diegetic_asop {

namespace {

// The provider's size must cover the member before the pointer is read: a shorter table has
// nothing there.
template <class M>
bool Callable(const sc_hangar_v1* t, std::size_t offset, M sc_hangar_v1::*member) noexcept {
    return t && t->size >= offset + sizeof(M) && t->*member != nullptr;
}

bool LiftCallable(const sc_hangar_v1* t) noexcept {
    return Callable(t, offsetof(sc_hangar_v1, lift_of_landing_area), &sc_hangar_v1::lift_of_landing_area) &&
           Callable(t, offsetof(sc_hangar_v1, lift_request), &sc_hangar_v1::lift_request);
}

bool AtcCallable(const sc_hangar_v1* t) noexcept {
    return Callable(t, offsetof(sc_hangar_v1, atc_cancel_request), &sc_hangar_v1::atc_cancel_request);
}

bool ApiHasServices(const sco_api* api) noexcept {
    return api && api->size >= offsetof(sco_api, query_service) + sizeof(api->query_service) &&
           api->query_service != nullptr;
}

Outcome Unavailable(std::string why) { return Outcome{SCO_UNAVAILABLE, std::move(why)}; }

Outcome TooShort(const sc_hangar_v1* t, const char* member) {
    return Unavailable("game.hangar table (" + std::to_string(t->size) + " bytes) has no " + member);
}

}  // namespace

const char* ResultName(sco_result r) noexcept {
    switch (r) {
        case SCO_OK: return "ok";
        case SCO_UNAVAILABLE: return "unavailable";
        case SCO_NOT_FOUND: return "not_found";
        case SCO_BAD_ARG: return "bad_arg";
        case SCO_CRASHED: return "crashed";
        case SCO_WRONG_THREAD: return "wrong_thread";
        case SCO_TOO_MANY: return "too_many";
        case SCO_FAILED: return "failed";
        default: return "unknown";
    }
}

bool HangarClient::Has(const char* capability) const noexcept {
    return api_ && api_->has && api_->has(capability) != 0;
}

const sc_hangar_v1* HangarClient::Table(Outcome& why) const {
    if (!api_) {
        why = Outcome{SCO_BAD_ARG, "not attached to a host"};
        return nullptr;
    }
    if (!ApiHasServices(api_)) {
        why = Unavailable("this host's sco_api has no services (1.0): game.hangar needs 1.1");
        return nullptr;
    }
    const void* out = nullptr;
    const sco_result r = api_->query_service(SC_HANGAR_SERVICE_NAME, SC_HANGAR_SERVICE_VERSION, &out);
    if (r != SCO_OK || !out) {
        why = Unavailable(std::string("game.hangar 1.0 is not published by this host (") +
                          ResultName(r != SCO_OK ? r : SCO_UNAVAILABLE) + ")");
        return nullptr;
    }
    const auto* t = static_cast<const sc_hangar_v1*>(out);
    if (t->size < sizeof(uint32_t)) {
        why = Unavailable("game.hangar table reports no size");
        return nullptr;
    }
    return t;
}

HangarStatus HangarClient::Status() const noexcept {
    HangarStatus s;
    s.services = ApiHasServices(api_);
    if (!s.services) return s;
    try {
        Outcome why;
        const sc_hangar_v1* t = Table(why);
        s.published = t != nullptr;
        s.lift = t && Has(kCapLift) && LiftCallable(t);
        s.atcCancel = t && Has(kCapAtcCancel) && AtcCallable(t);
    } catch (...) {
        // An allocation failure building a reason: report what was found so far.
    }
    return s;
}

std::string HangarClient::Reason(const sc_hangar_v1* t) const {
    if (!Callable(t, offsetof(sc_hangar_v1, last_error), &sc_hangar_v1::last_error)) return {};
    char small[256];
    uint32_t size = sizeof small;
    sco_result r = t->last_error(self_, small, &size);
    if (r == SCO_OK) return std::string(small, ::strnlen(small, sizeof small));
    // The size handshake: SCO_TOO_MANY answers the bytes needed (NUL included).
    if (r != SCO_TOO_MANY || size <= sizeof small || size > 64u * 1024u) return {};
    std::string big(size, '\0');
    uint32_t got = size;
    r = t->last_error(self_, big.data(), &got);
    if (r != SCO_OK) return {};
    big.resize(::strnlen(big.c_str(), big.size()));
    return big;
}

Outcome HangarClient::Failed(const sc_hangar_v1* t, const char* call, sco_result r) const {
    std::string text = std::string(call) + ": " + ResultName(r);
    const std::string reason = Reason(t);
    if (!reason.empty()) text += ": " + reason;
    return Outcome{r, std::move(text)};
}

Outcome HangarClient::FindLift(uint64_t landingAreaId, uint64_t& outLiftId) const {
    outLiftId = 0;
    Outcome why;
    const sc_hangar_v1* t = Table(why);
    if (!t) return why;
    if (!Has(kCapLift)) return Unavailable(std::string(kCapLift) + " is off on this game build");
    if (!Callable(t, offsetof(sc_hangar_v1, lift_of_landing_area), &sc_hangar_v1::lift_of_landing_area))
        return TooShort(t, "lift_of_landing_area");
    if (!landingAreaId) return Outcome{SCO_BAD_ARG, "landing area id 0"};
    uint64_t lift = 0;
    const sco_result r = t->lift_of_landing_area(self_, landingAreaId, &lift);
    if (r != SCO_OK) return Failed(t, "lift_of_landing_area", r);
    outLiftId = lift;
    return Outcome{SCO_OK, "landing area " + std::to_string(landingAreaId) + " -> lift " + std::to_string(lift)};
}

Outcome HangarClient::RequestLift(uint64_t liftId, sc_hangar_lift_op op) const {
    Outcome why;
    const sc_hangar_v1* t = Table(why);
    if (!t) return why;
    if (!Has(kCapLift)) return Unavailable(std::string(kCapLift) + " is off on this game build");
    if (!Callable(t, offsetof(sc_hangar_v1, lift_request), &sc_hangar_v1::lift_request))
        return TooShort(t, "lift_request");
    if (!liftId) return Outcome{SCO_BAD_ARG, "lift id 0"};
    const sco_result r = t->lift_request(self_, liftId, static_cast<uint32_t>(op));
    if (r != SCO_OK) return Failed(t, "lift_request", r);
    return Outcome{SCO_OK, std::string(op == SC_HANGAR_LIFT_OPEN ? "open" : "close") + " requested for lift " +
                               std::to_string(liftId)};
}

Outcome HangarClient::CancelAtc(uint64_t atcId) const {
    Outcome why;
    const sc_hangar_v1* t = Table(why);
    if (!t) return why;
    if (!Has(kCapAtcCancel)) return Unavailable(std::string(kCapAtcCancel) + " is off on this game build");
    if (!AtcCallable(t)) return TooShort(t, "atc_cancel_request");
    if (!atcId) return Outcome{SCO_BAD_ARG, "ATC id 0"};
    const sco_result r = t->atc_cancel_request(self_, atcId);
    if (r != SCO_OK) return Failed(t, "atc_cancel_request", r);
    return Outcome{SCO_OK, "ATC request withdrawn at " + std::to_string(atcId)};
}

Outcome HangarClient::LastError() const {
    Outcome why;
    const sc_hangar_v1* t = Table(why);
    if (!t) return why;
    if (!Callable(t, offsetof(sc_hangar_v1, last_error), &sc_hangar_v1::last_error))
        return TooShort(t, "last_error");
    const std::string reason = Reason(t);
    return Outcome{SCO_OK, reason.empty() ? "no game.hangar error" : "game.hangar: " + reason};
}

}  // namespace diegetic_asop
