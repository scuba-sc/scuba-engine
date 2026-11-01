#include "hostd/reserved_mock_host.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#ifdef SCUBA_RESERVED_COPY_TESTING
#include "reserved_copy_probe.h"
#include <exception>
#endif

namespace scuba::hostd {
namespace {
using namespace sco::reserved;
[[noreturn]] void Bad(const char* text) { throw HandlerError(rpc_error::kInvalidParams, text); }
void Check(sco::Result result) {
    if (result != sco::Result::Ok)
        throw HandlerError(result == sco::Result::BadArg ? rpc_error::kInvalidParams : rpc_error::kUnavailable,
                           "reserved mock operation refused (result " + std::to_string(static_cast<unsigned>(result)) + ")");
}
const Json& Field(const Json& object, const char* name) {
    const auto* field = object.find(name);
    if (!object.is_object() || !field) Bad("missing object field");
    return *field;
}
template<class T> T Integer(const Json& value) {
    T out{};
    if (value.is_string()) {
        const auto& text = value.as_string();
        auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) Bad("invalid decimal integer");
        return out;
    }
    // JSON numbers are doubles; only accept integers exactly representable in all clients.
    constexpr double maxExact = 9007199254740991.0;
    if (!value.is_number()) Bad("integer must be a decimal string or an exact JSON number");
    double number = value.as_number();
    if (!std::isfinite(number) || std::trunc(number) != number || number > maxExact || number < -maxExact ||
        (std::numeric_limits<T>::is_signed ? false : number < 0) ||
        number < static_cast<double>((std::numeric_limits<T>::lowest)()) ||
        number > static_cast<double>((std::numeric_limits<T>::max)())) Bad("integer out of range");
    return static_cast<T>(number);
}
void Vector(const Json& value, double (&out)[3]) {
    if (!value.is_array() || value.size() != 3) Bad("position must contain three numbers");
    for (size_t i = 0; i < 3; ++i) {
        if (!value.at(i).is_number() || !std::isfinite(value.at(i).as_number())) Bad("position must be finite");
        out[i] = value.at(i).as_number();
    }
}
Json VectorJson(const double (&v)[3]) {
    Json out = Json::MakeArray();
    for (double item : v) out.push_back(item);
    return out;
}
Json EntityJson(const MockEntity& e
#ifdef SCUBA_RESERVED_COPY_TESTING
                , bool probe = false
#endif
                ) {
    Json out = Json::MakeObject();
    out.set("id", std::to_string(e.id)).set("zone", std::to_string(e.zone))
        .set("position", VectorJson(e.position)).set("flags", e.flags)
#ifdef SCUBA_RESERVED_COPY_TESTING
        ;
    if (probe) reserved_copy_probe::BeforeNameCopy();
    out.set("name", e.name);
    if (probe && reserved_copy_probe::ThrowAfterCopy())
        throw HandlerError(rpc_error::kHandlerFailed, "copy failed: " + out.find("name")->as_string(), out);
#else
        .set("name", e.name);
#endif
    return out;
}
Json ZoneJson(const MockZone& z) {
    Json out = Json::MakeObject();
    out.set("id", std::to_string(z.id)).set("parent", std::to_string(z.parent))
        .set("origin", VectorJson(z.origin)).set("name", z.name);
    return out;
}
MockEntity ReadEntity(const Json& params) {
    MockEntity e{};
    e.id = Integer<uint64_t>(Field(params, "id"));
    e.zone = Integer<uint64_t>(Field(params, "zone"));
    e.flags = Integer<uint32_t>(Field(params, "flags"));
    Vector(Field(params, "position"), e.position);
    const auto& name = Field(params, "name");
    if (!name.is_string() || name.as_string().empty() || name.as_string().size() >= sizeof(e.name) ||
        name.as_string().find('\0') != std::string::npos) Bad("invalid entity name");
    std::memcpy(e.name, name.as_string().data(), name.as_string().size());
    return e;
}
} // namespace

ReservedMockHost& ReservedMockHost::Instance() {
    static ReservedMockHost* const root = new ReservedMockHost();
    return *root;
}
ReservedMockHost::ReservedMockHost() = default;
void ReservedMockHost::RegisterHandlers() {
    registry_.register_handler("mock.status", [this](const Json&, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        Json out = Json::MakeObject();
        out.set("nativeUnavailable", true).set("mockOnly", true).set("scalarTickOnly", true)
            .set("coreSource", SCUBA_RESERVED_CORE_COMMIT)
            .set("bootstrapVersion", kBootstrapVersion).set("mockVersion", kMockVersion)
            .set("graphVersion", kMockGraphVersion);
        return out;
    });
    registry_.register_handler("mock.entity_put", [this](const Json& p, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        auto entity = ReadEntity(p); const auto& t = table(); Check(t.put_entity(t.context, &entity));
        return EntityJson(entity);
    });
    registry_.register_handler("mock.entity_get", [this](const Json& p, const CallContext&) {
        std::optional<CallPin> pin(std::in_place); Check(TryEnter(session_, *pin));
#ifdef SCUBA_RESERVED_COPY_TESTING
        struct UnwindProbe {
            int exceptions = std::uncaught_exceptions();
            ~UnwindProbe() { if (std::uncaught_exceptions() > exceptions) reserved_copy_probe::DuringUnwind(); }
        } unwindProbe;
#endif
        const auto id = Integer<uint64_t>(Field(p, "id")); MockEntity entity{};
        const auto& t = table(); Check(t.get_entity(t.context, id, &entity));
#ifdef SCUBA_RESERVED_COPY_TESTING
        if (reserved_copy_probe::ReleasePinEarly()) pin.reset();
#endif
        return EntityJson(entity
#ifdef SCUBA_RESERVED_COPY_TESTING
                          , true
#endif
                          );
    });
    registry_.register_handler("mock.entity_remove", [this](const Json& p, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        const auto id = Integer<uint64_t>(Field(p, "id")); const auto& t = table();
        Check(t.remove_entity(t.context, id)); return Json(true);
    });
    registry_.register_handler("mock.graph", [this](const Json&, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        const auto& t = table(); uint32_t zones = 0, entities = 0;
        Check(t.zone_count(t.context, &zones)); Check(t.entity_count(t.context, &entities));
        Json zs = Json::MakeArray(), es = Json::MakeArray();
        for (uint32_t i = 0; i < zones; ++i) { MockZone z{}; Check(t.zone_at(t.context, i, &z)); zs.push_back(ZoneJson(z)); }
        for (uint32_t i = 0; i < entities; ++i) { MockEntity e{}; Check(t.entity_at(t.context, i, &e)); es.push_back(EntityJson(e)); }
        Json out = Json::MakeObject(); out.set("zones", std::move(zs)).set("entities", std::move(es)); return out;
    });
    registry_.register_handler("mock.scalar_set", [this](const Json& p, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        const auto value = Integer<int64_t>(Field(p, "value")); const auto& t = table();
        Check(t.set(t.context, value)); return Json(true);
    });
    registry_.register_handler("mock.scalar_tick", [this](const Json& p, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        const auto amount = Integer<uint64_t>(Field(p, "amount")); const auto& t = table();
        Check(t.tick(t.context, amount)); return Json(true);
    });
    registry_.register_handler("mock.scalar_snapshot", [this](const Json&, const CallContext&) {
        CallPin pin; Check(TryEnter(session_, pin));
        const auto& t = table(); int64_t value = 0; uint64_t ticks = 0;
        Check(t.snapshot(t.context, &value, &ticks)); Json out = Json::MakeObject();
        out.set("value", std::to_string(value)).set("ticks", std::to_string(ticks)); return out;
    });
}
const MockTable& ReservedMockHost::table() const {
    if (!table_) throw HandlerError(rpc_error::kUnavailable, "reserved mock unavailable");
    return *table_;
}
bool ReservedMockHost::Start() { return StartImpl(false); }
bool ReservedMockHost::StartImpl(bool abandon) {
    if (attempted_) return false;
    attempted_ = true;
    const auto* bootstrap = CreateExecutableAuthority();
    if (!bootstrap || BindCoreInstance(bootstrap, binding_) != sco::Result::Ok) return false;
    MockGraph graph{};
    graph.zoneCount = 1; graph.zones[0].id = 1; std::memcpy(graph.zones[0].name, "world", 6);
    if (ReserveSession(binding_, graph, session_) != sco::Result::Ok) return false;
    try { RegisterHandlers(); } catch (...) { Fault(session_); return false; }
    StartupControl control;
    if (BeginStartup(session_, control) != sco::Result::Ok) { Fault(session_); return false; }
    if (abandon) return false; // control's real destructor faults and retains
    if (PublishTypedProvider(control) != sco::Result::Ok || FinishStartup(control) != sco::Result::Ok) return false;
    table_ = QueryMock(session_);
    if (!table_ || table_->size < sizeof(MockTable) || table_->version != kMockVersion || !table_->context ||
        !table_->set || !table_->tick || !table_->snapshot || !table_->put_entity || !table_->get_entity ||
        !table_->remove_entity || !table_->entity_count || !table_->zone_count || !table_->zone_at || !table_->entity_at) {
        Fault(session_); table_ = nullptr; return false;
    }
    return true;
}
sco::Result ReservedMockHost::Close() noexcept { return sco::reserved::Close(session_); }
RetireResult ReservedMockHost::RetireUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    return sco::reserved::RetireUntil(session_, deadline);
}
Phase ReservedMockHost::phase() noexcept { return Status(session_); }
} // namespace scuba::hostd
