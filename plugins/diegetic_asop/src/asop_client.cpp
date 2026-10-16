// SPDX-License-Identifier: GPL-3.0-only
#include "asop_client.h"

#include <cstddef>
#include <cstring>
#include <utility>

namespace diegetic_asop {

namespace {

template <class M>
bool Callable(const sc_asop_v1* t, std::size_t offset, M sc_asop_v1::*member) noexcept {
    return t && t->size >= offset + sizeof(M) && t->*member != nullptr;
}

bool ClaimCallable(const sc_asop_v1* t) noexcept {
    return Callable(t, offsetof(sc_asop_v1, claim_timeout), &sc_asop_v1::claim_timeout);
}

bool ShardCallable(const sc_asop_v1* t) noexcept {
    return Callable(t, offsetof(sc_asop_v1, shard_persisted), &sc_asop_v1::shard_persisted);
}

bool ApiHasServices(const sco_api* api) noexcept {
    return api && api->size >= offsetof(sco_api, query_service) + sizeof(api->query_service) &&
           api->query_service != nullptr;
}

Outcome Unavailable(std::string why) { return Outcome{SCO_UNAVAILABLE, std::move(why)}; }

Outcome TooShort(const sc_asop_v1* t, const char* member) {
    return Unavailable("game.asop table (" + std::to_string(t->size) + " bytes) has no " + member);
}

}  // namespace

bool AsopClient::Has(const char* capability) const noexcept {
    return api_ && api_->has && api_->has(capability) != 0;
}

const sc_asop_v1* AsopClient::Table(Outcome& why) const {
    if (!api_) {
        why = Outcome{SCO_BAD_ARG, "not attached to a host"};
        return nullptr;
    }
    if (!ApiHasServices(api_)) {
        why = Unavailable("this host's sco_api has no services (1.0): game.asop needs 1.1");
        return nullptr;
    }
    const void* out = nullptr;
    const sco_result r = api_->query_service(SC_ASOP_SERVICE_NAME, SC_ASOP_SERVICE_VERSION, &out);
    if (r != SCO_OK || !out) {
        why = Unavailable(std::string("game.asop 1.0 is not published by this host (") +
                          ResultName(r != SCO_OK ? r : SCO_UNAVAILABLE) + ")");
        return nullptr;
    }
    const auto* t = static_cast<const sc_asop_v1*>(out);
    if (t->size < sizeof(uint32_t)) {
        why = Unavailable("game.asop table reports no size");
        return nullptr;
    }
    return t;
}

AsopStatus AsopClient::Status() const noexcept {
    AsopStatus s;
    if (!ApiHasServices(api_)) return s;
    const void* out = nullptr;
    s.query = api_->query_service(SC_ASOP_SERVICE_NAME, SC_ASOP_SERVICE_VERSION, &out);
    if (s.query != SCO_OK || !out) return s;
    const auto* t = static_cast<const sc_asop_v1*>(out);
    std::memcpy(&s.tableSize, out, sizeof s.tableSize);
    s.active = s.tableSize >= sizeof(uint32_t);
    if (!s.active) return s;
    s.claimTimeout = Has(kCapClaimTimeout) && ClaimCallable(t);
    s.shardPersisted = Has(kCapShardPersisted) && ShardCallable(t);
    return s;
}

Outcome AsopClient::Failed(const sc_asop_v1* t, const char* call, sco_result r) const {
    std::string text = std::string(call) + ": " + ResultName(r);
    if (Callable(t, offsetof(sc_asop_v1, last_error), &sc_asop_v1::last_error)) {
        char reason[256];
        uint32_t size = sizeof reason;
        // A reason longer than the buffer is dropped: the result name still says what failed.
        if (t->last_error(self_, reason, &size) == SCO_OK && reason[0])
            text += std::string(": ") + std::string(reason, ::strnlen(reason, sizeof reason));
    }
    return Outcome{r, std::move(text)};
}

Outcome AsopClient::ClaimTimeout(int32_t& outSeconds) const {
    outSeconds = 0;
    Outcome why;
    const sc_asop_v1* t = Table(why);
    if (!t) return why;
    if (!Has(kCapClaimTimeout)) return Unavailable(std::string(kCapClaimTimeout) + " is off on this game build");
    if (!ClaimCallable(t)) return TooShort(t, "claim_timeout");
    int32_t seconds = 0;
    const sco_result r = t->claim_timeout(self_, &seconds);
    if (r != SCO_OK) return Failed(t, "claim_timeout", r);
    outSeconds = seconds;
    return Outcome{SCO_OK, "claim timeout " + std::to_string(seconds) + " s"};
}

Outcome AsopClient::ShardPersisted(bool& outPersisted) const {
    outPersisted = false;
    Outcome why;
    const sc_asop_v1* t = Table(why);
    if (!t) return why;
    if (!Has(kCapShardPersisted)) return Unavailable(std::string(kCapShardPersisted) + " is off on this game build");
    if (!ShardCallable(t)) return TooShort(t, "shard_persisted");
    uint32_t persisted = 0;
    const sco_result r = t->shard_persisted(self_, &persisted);
    if (r != SCO_OK) return Failed(t, "shard_persisted", r);
    outPersisted = persisted != 0;
    return Outcome{SCO_OK, std::string("shard persisted: ") + (outPersisted ? "yes" : "no")};
}

}  // namespace diegetic_asop
