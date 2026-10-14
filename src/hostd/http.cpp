#include "hostd/http.h"

#include <cctype>

namespace scuba::hostd {

namespace {

char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = Lower(c);
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool PercentDecode(std::string_view in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size()) return false;
        const int hi = HexValue(in[i + 1]);
        const int lo = HexValue(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
    }
    return true;
}

bool IsTokenChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u > 32 && u < 127 && c != '(' && c != ')' && c != '<' && c != '>' && c != '@' && c != ',' && c != ';' &&
           c != ':' && c != '\\' && c != '"' && c != '/' && c != '[' && c != ']' && c != '?' && c != '=' && c != '{' &&
           c != '}';
}

}  // namespace

const std::string* HttpRequest::header(std::string_view lowerName) const {
    for (const auto& [name, value] : headers)
        if (name == lowerName) return &value;
    return nullptr;
}

bool HttpRequest::header_has_token(std::string_view lowerName, std::string_view token) const {
    const std::string want = ToLower(token);
    for (const auto& [name, value] : headers) {
        if (name != lowerName) continue;
        std::string_view rest = value;
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view item = Trim(rest.substr(0, comma));
            if (ToLower(item) == want) return true;
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
    return false;
}

HttpParse ParseHttpRequest(std::string_view buffer, HttpRequest& out, size_t& consumed) {
    const size_t end = buffer.find("\r\n\r\n");
    if (end == std::string_view::npos) return buffer.size() > kMaxHttpHead ? HttpParse::Error : HttpParse::Incomplete;
    if (end + 4 > kMaxHttpHead) return HttpParse::Error;

    out = HttpRequest{};
    std::string_view head = buffer.substr(0, end + 2);
    const size_t lineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, lineEnd);
    head.remove_prefix(lineEnd + 2);

    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? sp1 : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return HttpParse::Error;
    out.method = std::string(requestLine.substr(0, sp1));
    out.target = std::string(requestLine.substr(sp1 + 1, sp2 - sp1 - 1));
    out.version = std::string(requestLine.substr(sp2 + 1));
    if (out.method.empty() || out.target.empty() || out.target[0] != '/') return HttpParse::Error;
    for (const char c : out.method)
        if (!IsTokenChar(c)) return HttpParse::Error;
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") return HttpParse::Error;
    if (!PercentDecode(std::string_view(out.target).substr(0, out.target.find('?')), out.path)) return HttpParse::Error;

    while (!head.empty()) {
        const size_t e = head.find("\r\n");
        const std::string_view line = head.substr(0, e);
        head.remove_prefix(e + 2);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return HttpParse::Error;
        const std::string_view name = line.substr(0, colon);
        for (const char c : name)
            if (!IsTokenChar(c)) return HttpParse::Error;
        out.headers.emplace_back(ToLower(name), std::string(Trim(line.substr(colon + 1))));
    }

    if (out.header("transfer-encoding")) return HttpParse::Error;
    if (const std::string* len = out.header("content-length"); len && *len != "0") return HttpParse::Error;
    consumed = end + 4;
    return HttpParse::Done;
}

std::string_view HttpStatusText(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 426: return "Upgrade Required";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

std::string HttpResponse(int status, std::string_view contentType, std::string_view body,
                         const std::vector<std::pair<std::string, std::string>>& extraHeaders, bool headOnly) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(HttpStatusText(status)) + "\r\n";
    if (!contentType.empty()) out += "Content-Type: " + std::string(contentType) + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& [name, value] : extraHeaders) out += name + ": " + value + "\r\n";
    out += "\r\n";
    if (!headOnly) out += body;
    return out;
}

std::string_view MimeTypeFor(const std::filesystem::path& file) {
    const std::string ext = ToLower(file.extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".glb") return "model/gltf-binary";
    if (ext == ".gltf") return "model/gltf+json";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

std::optional<std::filesystem::path> ResolveStaticPath(const std::filesystem::path& root, std::string_view urlPath) {
    if (urlPath.empty() || urlPath[0] != '/') return std::nullopt;
    if (urlPath.find('\0') != std::string_view::npos || urlPath.find('\\') != std::string_view::npos ||
        urlPath.find(':') != std::string_view::npos)
        return std::nullopt;
    std::string rel(urlPath.substr(1));
    if (rel.empty() || rel.back() == '/') rel += "index.html";
    std::filesystem::path out = root;
    std::string_view rest = rel;
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        const std::string_view seg = rest.substr(0, slash);
        if (seg.empty() || seg == "." || seg == "..") return std::nullopt;
        out /= std::filesystem::path(std::string(seg));
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    return out;
}

}  // namespace scuba::hostd
