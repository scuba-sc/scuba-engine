#include "check.h"
#include "hostd/handlers.h"

#include <stdexcept>
#include <thread>

using namespace scuba::hostd;

namespace {

Json Call(const Registry& r, const std::string& text) {
    CallContext ctx;
    ctx.client = 42;
    const auto parsed = Json::parse(r.dispatch_text(text, ctx));
    CHECK(parsed.has_value());
    return parsed ? *parsed : Json();
}

int ErrorCode(const Json& response) {
    const Json* e = response.find("error");
    return e ? static_cast<int>(e->find("code")->as_number()) : 0;
}

void TestRegistration() {
    Registry r;
    r.register_handler("test.echo", [](const Json& params, const CallContext&) { return params; });
    CHECK(r.has("test.echo"));
    CHECK(!r.has("test.other"));
    bool threw = false;
    try {
        r.register_handler("test.echo", [](const Json&, const CallContext&) { return Json(); });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    for (const char* bad : { "", "ping", "Test.x", "test.", ".x", "test..x", "test.x-y", "1a.b" }) {
        threw = false;
        try {
            r.register_handler(bad, [](const Json&, const CallContext&) { return Json(); });
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    CHECK(IsValidMethodName("system.ping"));
    CHECK(IsValidMethodName("widgets.panel.render_2"));
    CHECK(r.methods().size() == 1);
}

void TestDispatch() {
    Registry r;
    r.register_handler("test.echo", [](const Json& params, const CallContext& ctx) {
        Json out = Json::MakeObject();
        out.set("params", params);
        out.set("client", ctx.client);
        return out;
    });
    r.register_handler("test.fail", [](const Json&, const CallContext&) -> Json {
        throw HandlerError(rpc_error::kInvalidParams, "need x", Json("hint"));
    });
    r.register_handler("test.crash", [](const Json&, const CallContext&) -> Json { throw std::runtime_error("boom"); });

    Json ok = Call(r, R"({"id":"a1","method":"test.echo","params":{"x":1}})");
    CHECK(ok.find("id")->as_string() == "a1");
    CHECK(ok.find("result")->find("params")->find("x")->as_number() == 1);
    CHECK(ok.find("result")->find("client")->as_number() == 42);
    CHECK(!ok.find("error"));

    Json noParams = Call(r, R"({"id":2,"method":"test.echo"})");
    CHECK(noParams.find("result")->find("params")->is_object());

    Json fail = Call(r, R"({"id":3,"method":"test.fail"})");
    CHECK(ErrorCode(fail) == rpc_error::kInvalidParams);
    CHECK(fail.find("error")->find("message")->as_string() == "need x");
    CHECK(fail.find("error")->find("data")->as_string() == "hint");
    CHECK(fail.find("id")->as_number() == 3);

    CHECK(ErrorCode(Call(r, R"({"id":4,"method":"test.crash"})")) == rpc_error::kInternalError);
    CHECK(ErrorCode(Call(r, R"({"id":5,"method":"test.missing"})")) == rpc_error::kMethodNotFound);
    CHECK(ErrorCode(Call(r, R"({"method":"test.echo"})")) == rpc_error::kInvalidRequest);
    CHECK(ErrorCode(Call(r, R"({"id":6})")) == rpc_error::kInvalidRequest);
    CHECK(ErrorCode(Call(r, R"({"id":7,"method":"test.echo","params":3})")) == rpc_error::kInvalidParams);
    CHECK(ErrorCode(Call(r, R"([1,2])")) == rpc_error::kInvalidRequest);
    const Json parse = Call(r, "{not json");
    CHECK(ErrorCode(parse) == rpc_error::kParseError);
    CHECK(parse.find("id")->is_null());
}

void TestBroadcaster() {
    Broadcaster b;
    std::thread t([&b] {
        for (int i = 0; i < 100; ++i) b.broadcast("test.tick", Json(i));
    });
    t.join();
    b.broadcast("test.done", Json());
    const auto events = b.drain();
    CHECK(events.size() == 101);
    CHECK(events.front() == R"({"event":"test.tick","data":0})");
    CHECK(events.back() == R"({"event":"test.done","data":null})");
    CHECK(b.drain().empty());
}

}  // namespace

void TestIsValidMethodName() {
    // Valid cases
    CHECK(IsValidMethodName("a.b"));
    CHECK(IsValidMethodName("system.ping"));
    CHECK(IsValidMethodName("widgets.panel.render_2"));
    CHECK(IsValidMethodName("a_1.b_2_c"));
    CHECK(IsValidMethodName("a.b.c.d.e"));

    // Part length boundary tests (64 chars max)
    const std::string valid_64_part(64, 'a');
    CHECK(IsValidMethodName("prefix." + valid_64_part));
    CHECK(IsValidMethodName(valid_64_part + ".suffix"));

    // Invalid cases: part length exceeding 64 chars
    const std::string invalid_65_part(65, 'a');
    CHECK(!IsValidMethodName("prefix." + invalid_65_part));
    CHECK(!IsValidMethodName(invalid_65_part + ".suffix"));

    // Invalid cases: empty, structure, and dot placements
    CHECK(!IsValidMethodName(""));
    CHECK(!IsValidMethodName("ping"));
    CHECK(!IsValidMethodName(".ping"));
    CHECK(!IsValidMethodName("system."));
    CHECK(!IsValidMethodName("system..ping"));
    CHECK(!IsValidMethodName("a.b."));
    CHECK(!IsValidMethodName(".a.b"));

    // Invalid cases: first character of a part must be 'a'-'z'
    CHECK(!IsValidMethodName("1a.b"));
    CHECK(!IsValidMethodName("a.1b"));
    CHECK(!IsValidMethodName("_a.b"));
    CHECK(!IsValidMethodName("a._b"));
    CHECK(!IsValidMethodName("A.b"));
    CHECK(!IsValidMethodName("a.B"));

    // Invalid cases: disallowed characters
    CHECK(!IsValidMethodName("test.x-y"));
    CHECK(!IsValidMethodName("test.x y"));
    CHECK(!IsValidMethodName("test.x/y"));
    CHECK(!IsValidMethodName("test.x@y"));
    CHECK(!IsValidMethodName("test.x#y"));
    CHECK(!IsValidMethodName("test.x!y"));
    CHECK(!IsValidMethodName("test.x?y"));
}

int main() {
    TestIsValidMethodName();
    TestRegistration();
    TestDispatch();
    TestBroadcaster();
    return CheckExit("test_handlers");
}
