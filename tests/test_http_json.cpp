#include "check.h"
#include "hostd/http.h"
#include "hostd/json.h"

#include <string>

using namespace scuba::hostd;

namespace {

void TestHttpParse() {
    HttpRequest req;
    size_t consumed = 0;
    const std::string text = "GET /assets/a%20b.js?v=1 HTTP/1.1\r\nHost: 127.0.0.1:7878\r\nX-Test:  padded  \r\n\r\nextra";
    CHECK(ParseHttpRequest(text, req, consumed) == HttpParse::Done);
    CHECK(consumed == text.size() - 5);
    CHECK(req.method == "GET");
    CHECK(req.path == "/assets/a b.js");
    CHECK(req.target == "/assets/a%20b.js?v=1");
    CHECK(req.header("host") && *req.header("host") == "127.0.0.1:7878");
    CHECK(req.header("x-test") && *req.header("x-test") == "padded");

    CHECK(ParseHttpRequest("GET / HTTP/1.1\r\nHost: x\r\n", req, consumed) == HttpParse::Incomplete);
    CHECK(ParseHttpRequest("GET / HTTP/2\r\n\r\n", req, consumed) == HttpParse::Error);
    CHECK(ParseHttpRequest("GET nopath HTTP/1.1\r\n\r\n", req, consumed) == HttpParse::Error);
    CHECK(ParseHttpRequest("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nbody", req, consumed) == HttpParse::Error);
    CHECK(ParseHttpRequest("GET /%zz HTTP/1.1\r\n\r\n", req, consumed) == HttpParse::Error);
    CHECK(ParseHttpRequest(std::string(kMaxHttpHead + 1, 'a'), req, consumed) == HttpParse::Error);
}

void TestStaticPaths() {
    const std::filesystem::path root = "dist";
    CHECK(ResolveStaticPath(root, "/") == root / "index.html");
    CHECK(ResolveStaticPath(root, "/assets/app.js") == root / "assets" / "app.js");
    CHECK(ResolveStaticPath(root, "/assets/") == root / "assets" / "index.html");
    CHECK(!ResolveStaticPath(root, "/../secret"));
    CHECK(!ResolveStaticPath(root, "/a/../../secret"));
    CHECK(!ResolveStaticPath(root, "/a//b"));
    CHECK(!ResolveStaticPath(root, "/a\\..\\b"));
    CHECK(!ResolveStaticPath(root, "/C:/windows"));
    CHECK(!ResolveStaticPath(root, std::string("/a\0b", 4)));
    CHECK(MimeTypeFor("x/index.html") == "text/html; charset=utf-8");
    CHECK(MimeTypeFor("x/app.JS") == "text/javascript; charset=utf-8");
    CHECK(MimeTypeFor("x/blob") == "application/octet-stream");
}

void TestJson() {
    const auto v = Json::parse(R"({"id":7,"method":"system.ping","params":{"a":[1,2.5,-3e2,true,false,null],"s":"x\"\\\u00e9\ud83d\ude00"}})");
    CHECK(v.has_value());
    CHECK(v->find("id")->as_number() == 7);
    CHECK(v->find("method")->as_string() == "system.ping");
    const Json* a = v->find("params")->find("a");
    CHECK(a && a->is_array() && a->size() == 6);
    CHECK(a->at(1).as_number() == 2.5 && a->at(2).as_number() == -300);
    CHECK(a->at(3).as_bool() && !a->at(4).as_bool(true) && a->at(5).is_null());
    CHECK(v->find("params")->find("s")->as_string() == "x\"\\\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(Json::parse(v->dump()) == *v);

    Json o = Json::MakeObject();
    o.set("b", 1);
    o.set("a", "t\n");
    o.set("b", 2);
    CHECK(o.dump() == R"({"b":2,"a":"t\n"})");
    CHECK(Json(0.5).dump() == "0.5");
    CHECK(Json(-12).dump() == "-12");
    CHECK(Json(std::string("\x01", 1)).dump() == "\"\\u0001\"");

    std::string err;
    for (const char* bad : { "", "{", "[1,]", "{\"a\" 1}", "01", "1.", "tru", "\"\\ud800\"", "\"a\nb\"", "[1] x", "{\"a\":1,}" }) {
        CHECK(!Json::parse(bad, &err));
        CHECK(!err.empty());
    }
    CHECK(!Json::parse(std::string(100, '[') + std::string(100, ']')));
    CHECK(Json::parse(std::string(60, '[') + std::string(60, ']')).has_value());
}

void TestHeaderHasToken() {
    HttpRequest req;

    // Empty request / missing header name
    CHECK(!req.header_has_token("connection", "keep-alive"));
    CHECK(!req.header_has_token("connection", ""));

    // Single token, case insensitivity in header value and token parameter
    req.headers = {{"connection", "Keep-Alive"}};
    CHECK(req.header_has_token("connection", "keep-alive"));
    CHECK(req.header_has_token("connection", "KEEP-ALIVE"));
    CHECK(req.header_has_token("connection", "Keep-Alive"));
    CHECK(!req.header_has_token("connection", "close"));
    CHECK(!req.header_has_token("upgrade", "keep-alive"));

    // Comma-separated tokens (first, middle, last) and space/tab padding
    req.headers = {{"connection", " \t keep-alive \t ,  Upgrade ,  close  "}};
    CHECK(req.header_has_token("connection", "keep-alive"));
    CHECK(req.header_has_token("connection", "upgrade"));
    CHECK(req.header_has_token("connection", "close"));
    CHECK(!req.header_has_token("connection", "open"));

    // Empty token search against empty item list and non-empty item list
    req.headers = {{"connection", ",,"}};
    CHECK(!req.header_has_token("connection", ""));
    req.headers = {{"connection", "keep-alive, upgrade"}};
    CHECK(!req.header_has_token("connection", ""));

    // Consecutive commas and leading/trailing commas
    req.headers = {{"connection", ",, keep-alive, , upgrade,,"}};
    CHECK(req.header_has_token("connection", "keep-alive"));
    CHECK(req.header_has_token("connection", "upgrade"));
    CHECK(!req.header_has_token("connection", ""));

    // Partial / substring match prevention
    req.headers = {{"connection", "keep-alive"}};
    CHECK(!req.header_has_token("connection", "keep"));
    CHECK(!req.header_has_token("connection", "alive"));
    CHECK(!req.header_has_token("connection", "keep-alive-extra"));

    // Multiple header fields with the same lowerName across different headers and parameterized values
    req.headers = {
        {"accept-encoding", "gzip;q=1.0"},
        {"accept-encoding", "deflate, br"}
    };
    CHECK(req.header_has_token("accept-encoding", "gzip;q=1.0"));
    CHECK(req.header_has_token("accept-encoding", "deflate"));
    CHECK(req.header_has_token("accept-encoding", "br"));
    CHECK(!req.header_has_token("accept-encoding", "gzip"));
    CHECK(!req.header_has_token("accept-encoding", "zstd"));
}

}  // namespace

int main() {
    TestHttpParse();
    TestHeaderHasToken();
    TestStaticPaths();
    TestJson();
    return CheckExit("test_http_json");
}
